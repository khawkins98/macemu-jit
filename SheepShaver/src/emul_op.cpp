/*
 *  emul_op.cpp - 68k opcodes for ROM patches
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>	// E2E harness frontmost-app change tracking (strcmp/strncpy)

#include "sysdeps.h"
#include "main.h"
#include "version.h"
#include "prefs.h"
#include "cpu_emulation.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "video.h"
#include "audio.h"
#include "ether.h"
#include "serial.h"
#include "clip.h"
#include "extfs.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "rsrc_patches.h"
#include "name_registry.h"
#include "user_strings.h"
#include "emul_op.h"
#include "thunks.h"
#include "ui_introspect.h"

#define DEBUG 0
#include "debug.h"

extern bool tick_inhibit;

// Stub-pressure trace (SS_STUB_TRACE): flip boot→steady at first guest idle. Defined in
// ppc-execute.cpp; no-op unless the probe is enabled. See MMU-NANOKERNEL-MP-PLAN.md.
extern "C" void ss_stub_trace_steady(void);

// CopyBits HLE go/no-go probe (SS_COPYBITS_TRACE): resolve _CopyBits (trap 0xA8EC) once the System is
// up, and report its address so a SS_JIT_PROFILE run can read CopyBits's execution count straight from
// the JIT's per-block (PC-keyed) profiler — NO guest patching, zero crash surface. (An earlier
// come-from heap-stub that counted via an EMUL_OP crashed the guest on the first CopyBits call from the
// ADB cursor-draw path — the EMUL_OP mis-resumed in nested execute_68k, jmp(a0) → garbage PC; abandoned.
// COMPATIBILITY-PAYOFF memo #1.) To get the frequency: run with `SS_COPYBITS_TRACE=1 SS_JIT_PROFILE=/p`,
// note the logged CopyBits PC, then grep the profile's hot-block table for that PC. Phase 2 (rect sizes)
// will need real interception — do it the safe ROM-patch way, not a heap come-from. Env-gated; default off.
static bool g_copybits_resolved = false;
static void copybits_probe_install(void)
{
	const char *e = getenv("SS_COPYBITS_TRACE");
	if (g_copybits_resolved || !(e && *e && *e != '0'))
		return;
	g_copybits_resolved = true;
	M68kRegisters r = {};
	r.d[0] = 0xA8EC;                 // _CopyBits
	Execute68kTrap(0xa146, &r);      // GetToolboxTrapAddress -> a0
	uint32 addr = r.a[0];
	// On a PPC Mac, CopyBits is PowerPC code reached via Mixed Mode: the trap address is a
	// RoutineDescriptor (magic 0xAAFE), not the PPC code the JIT runs. Follow RD -> first
	// RoutineRecord.procDescriptor (@+0x18) -> PPC TVector -> [codeAddr, tocAddr]. Read-only.
	uint32 ppc_entry = 0;
	if (addr && guest_ptr_ok(addr) && (ReadMacInt16(addr) == 0xAAFE)) {
		uint32 pd = ReadMacInt32(addr + 0x18);          // procDescriptor (TVector* for PPC)
		if (pd && guest_ptr_ok(pd)) ppc_entry = ReadMacInt32(pd);  // TVector[0] = code address
	}
	fprintf(stderr, "[COPYBITS] _CopyBits trap 0xA8EC -> descriptor %08x%s. "
	        "Use SS_JIT_PROFILE_PC=<the PPC entry> for the call count (Finder barely blits — use a graphics app).\n",
	        (unsigned)addr,
	        ppc_entry ? "" : " (not a RoutineDescriptor / no PPC TVector found)");
	if (ppc_entry)
		fprintf(stderr, "[COPYBITS] PPC code entry (the JIT block PC to profile): %08x\n", (unsigned)ppc_entry);
}

void PlayStartupSound();

// TVector of MakeExecutable
static uint32 MakeExecutableTvec;


// E2E harness boot-ready signal (ROADMAP A5). Emit ONE line the first time the guest reaches
// Process-Manager idle (OP_IDLE_TIME = SynchIdleTime patch), enriched with frontmost-app + modal
// state so an automated harness can tell "idle at the Finder desktop" from "idle blocked on a
// modal dialog" (disk-repair prompt etc.). The two heuristic alternatives (heartbeat block-rate
// collapse / compiled-block plateau) cannot make that distinction; this idle hook can, because it
// can read guest state. See docs/superpowers/specs/2026-06-04-e2e-vnc-harness-design.md §11.
//
// Technique: reading classic Mac OS low-memory globals to inspect the running system from the
// host. Source: Inside Macintosh (Operating System Utilities / Toolbox) — CurApName ($0910, the
// frontmost app name as a Str31), WindowList ($09D6, head of the window list), the WindowRecord
// windowKind field (offset +$6C; dialogKind == 2), and Ticks ($016A, 60/s since boot). These
// fixed low-mem addresses are stable across classic Mac OS; SheepShaver maps guest low memory via
// Mac2HostAddr/ReadMacIntN. The idle hook itself reuses SheepShaver's own SynchIdleTime ROM patch
// (rom_patches.cpp), so this adds only the state read, not a new trap.
// Make a byte safe to drop into a single-quoted log field: non-printable -> '?' (and flags a junk
// read via *bad); a literal single-quote -> '`' so it can't break the harness's frontApp='...' /
// title='...' regexes (which capture with '[^']*').
static inline char e2e_log_safe_char(uint8 c, bool *bad)
{
	if (c < 32 || c >= 127) { if (bad) *bad = true; return '?'; }
	if (c == '\'')
		return '`';
	return (char)c;
}

// Front window title, for instrumentation. WindowRecord.titleHandle is at +0x86 (a StringHandle =
// Handle to a Str255: deref the handle to a master ptr, then read length byte + chars). Empty for
// untitled dialogs/alerts. Sanitized to log-safe ASCII. Returns FALSE if a pointer is wild or the
// read looks like garbage (insane length / non-printable bytes) — under cooperative multitasking the
// frontmost "window" briefly points at background-extension pseudo-windows whose title deref yields
// junk; the caller suppresses those frames, and the bounds-checks keep a wild deref from crashing.
static bool e2e_front_window_title(uint32 win, char *out, int outsz)
{
	out[0] = '\0';
	if (!win)
		return true;			// bare desktop / no front window = a valid empty title
	if (!guest_ptr_ok(win))
		return false;
	uint32 hdl = ReadMacInt32(win + 0x86);		// titleHandle
	if (!hdl)
		return true;			// untitled window = valid empty
	if (!guest_ptr_ok(hdl))
		return false;
	uint32 ptr = ReadMacInt32(hdl);				// *titleHandle -> Str255
	if (!ptr)
		return true;
	if (!guest_ptr_ok(ptr))
		return false;
	uint8 *s = Mac2HostAddr(ptr);
	int len = s[0];
	if (len > 63)
		return false;			// insane Str255 length = junk read
	bool valid = true;
	for (int i = 0; i < len && i < outsz - 1; i++)
		out[i] = e2e_log_safe_char(s[1 + i], &valid);
	out[(len < outsz - 1) ? len : outsz - 1] = '\0';
	return valid;
}

// ---- AltiVec detection enabler (opt-in: `altivec` pref / SS_FORCE_ALTIVEC env) --------------
//
// WHAT: makes the guest OS report that the (emulated) PowerPC has a vector unit, so real apps
// take their AltiVec code path. Our AArch64 JIT already COMPILES PPC AltiVec → ARM64 NEON
// unconditionally (no MSR[VEC] gate); the only thing missing was the guest *detecting* AltiVec.
//
// WHY IT'S NEEDED (verified 2026-06-07): in SheepShaver's OldWorld-1.1-ROM environment the
// gestalt selector 'ppcf' (gestaltPowerPCProcessorFeatures, 0x70706366) is NOT registered at
// all — Gestalt('ppcf') returns gestaltUndefSelectorErr under BOTH Mac OS 8.6 AND 9.0 ('sysv'
// control reads fine). The OldWorld nanokernel never advertises a vector unit, so no OS version
// fixes it. PVR is already a 7400 (G4), but apps key off the gestalt, not PVR.
//
// HOW: at the first post-boot idle (System up, safe to call traps), REGISTER 'ppcf' ourselves
// via _NewGestalt ($A3AD) with a tiny 68k SelectorFunction that returns the vector-feature mask
// 0x10 = (1 << gestaltPowerPCHasVectorInstructions). NOTE the constant is bit NUMBER 4, so the
// mask is 0x10 — NOT 0x40 (bit 6 = gestaltPowerPCHas64BitSupport; an earlier 0x40 set the wrong
// feature and confounded the whole experiment — see LEARNINGS 2026-06-07). Verified end-to-end:
// AltiVec Fractal Carbon then detects AltiVec, runs its vector kernel, and the JIT compiles it
// (`SS_JIT_PROFILE` → [JIT-COMPILED-MIX] AltiVec=160, AltiVec hot blocks).
//
// OPT-IN + CAVEAT (why this is NOT default-on): under 8.6/9.0 here the OS/nanokernel does not do
// VR (vector register) context save/restore across task switches — fine for a single compute
// app, but advertising AltiVec system-wide could corrupt vector state in true preemptive/MP
// vector use. So it's an explicit opt-in (`altivec` pref, default false; SS_FORCE_ALTIVEC env
// overrides for dev). Roadmap §B5 tracks promoting this to fully-safe (model VR context).
static void force_altivec_idle_service(void)
{
	static int s_enabled = -1;
	static bool s_done = false;
	if (s_enabled < 0) {
		// `altivec` pref is the user-facing opt-in; SS_FORCE_ALTIVEC env is a dev override that
		// forces it on even when the pref is absent/false (and "=0" forces it off).
		const char *e = getenv("SS_FORCE_ALTIVEC");
		if (e && *e) s_enabled = (*e != '0') ? 1 : 0;
		else s_enabled = PrefsFindBool("altivec") ? 1 : 0;
	}
	if (!s_enabled || s_done)
		return;
	// Gate on a KNOWN-registered selector ('sysv' = system version) so we don't act before the
	// System Gestalt is up. Once 'sysv' resolves, the System is initialized.
	M68kRegisters sv = {};
	sv.d[0] = 0x73797376;			// 'sysv'
	Execute68kTrap(0xa1ad, &sv);		// Gestalt()
	if ((sv.d[0] & 0xffff) == 0xea51)	// System gestalt not up yet; retry next idle
		return;
	s_done = true;

	// Read 'ppcf' (gestaltPowerPCProcessorFeatures). In SheepShaver's OldWorld environment it is
	// NOT registered (undefSelectorErr) under 8.6 or 9.0 — so there is no bit to flip. FORCE it by
	// REGISTERING the selector ourselves with the vector bit set, then read it back to confirm.
	M68kRegisters pf = {};
	pf.d[0] = 0x70706366;			// 'ppcf'
	Execute68kTrap(0xa1ad, &pf);		// Gestalt()
	uint32 pre_err = pf.d[0] & 0xffff;

	uint32 reg_err = 0xffff, proc = 0;
	if (pre_err != 0) {
		// Allocate a system-heap block for a tiny 68k Gestalt SelectorFunction and write it.
		// SelectorFunction ABI (Pascal): pascal OSErr fn(OSType selector, long *response).
		// On entry: 0(sp)=retaddr, 4(sp)=response(long*), 8(sp)=selector, 12(sp)=result(OSErr,2B).
		// We ignore the selector, write *response = 0x10 = (1 << gestaltPowerPCHasVectorInstructions).
		// CRITICAL: the gestalt constant is bit NUMBER 4, so the mask is 0x10 — NOT 0x40 (which is
		// bit 6 = gestaltPowerPCHas64BitSupport). Verified against Apple CarbonCore Gestalt.h
		// (2026-06-07). The earlier 0x40 set the wrong feature, confounding the FC experiment.
		// set result=noErr, and Pascal-return (pop retaddr, drop 8B params, leave result slot).
		M68kRegisters m = {};
		m.d[0] = 32;
		Execute68kTrap(0xa71e, &m);	// NewPtrSysClear()
		proc = m.a[0];
		if (proc) {
			static const uint16 sel_code[] = {
				0x206F, 0x0004,			// movea.l 4(a7),a0      ; a0 = response
				0x20BC, 0x0000, 0x0010,		// move.l  #$10,(a0)      ; *response = 1<<4 (vector bit)
				0x426F, 0x000C,			// clr.w   12(a7)        ; result = noErr
				0x205F,				// movea.l (a7)+,a0      ; pop return addr
				0x4FEF, 0x0008,			// lea     8(a7),a7      ; drop 2 params (8B)
				0x4ED0				// jmp     (a0)
			};
			for (unsigned i = 0; i < sizeof(sel_code)/sizeof(sel_code[0]); i++)
				WriteMacInt16(proc + i*2, sel_code[i]);
			// NewGestalt(selector=d0, selectorFunction=a0). Raw 68k proc ptr → Mixed Mode calls
			// it as 68k via the SelectorFunction ProcInfo. Trap _NewGestalt = $A0AD.
			M68kRegisters n = {};
			n.d[0] = 0x70706366;		// 'ppcf'
			n.a[0] = proc;
			Execute68kTrap(0xa3ad, &n);	// NewGestalt() = $A3AD (Gestalt family: bits 9-10 select op)
			reg_err = n.d[0] & 0xffff;
		}
	}

	// Read back — this CALLS our selector function, exercising the whole chain.
	M68kRegisters rb = {};
	rb.d[0] = 0x70706366;			// 'ppcf'
	Execute68kTrap(0xa1ad, &rb);		// Gestalt()
	fprintf(stderr, "[FORCE_AV] sysv=0x%08x | ppcf pre:err=%u | NewGestalt(proc=%08x):err=%u | "
	        "readback:err=%u features=0x%08x vectorBit(0x10)=%s\n",
	        (unsigned)sv.a[0], (unsigned)pre_err, (unsigned)proc, (unsigned)reg_err,
	        (unsigned)(rb.d[0] & 0xffff), (unsigned)rb.a[0], (rb.a[0] & 0x10) ? "SET" : "clear");
}

static void e2e_emit_idle_signals(void)
{
	// CurApName: low-mem 0x910, Pascal Str31 (length byte + chars). Sanitize to log-safe ASCII so an
	// app name containing a quote/control char can't break the harness's frontApp='...' parser.
	char app[32];
	uint8 *namep = Mac2HostAddr(0x910);
	int len = namep[0];
	if (len > 31)
		len = 31;
	for (int i = 0; i < len; i++)
		app[i] = e2e_log_safe_char(namep[1 + i], NULL);
	app[len] = '\0';

	// Modal check: is the front window a dialog? WindowList head = 0x9D6; windowKind at +0x6C.
	int modal = 0;
	uint32 front = ReadMacInt32(0x9d6);
	if (front) {
		int16 kind = (int16)ReadMacInt16(front + 0x6c);
		if (kind == 2)			// dialogKind
			modal = 1;
	}
	char title[64];
	bool title_valid = e2e_front_window_title(front, title, sizeof(title));
	uint32 ticks = ReadMacInt32(0x16a);	// Ticks since boot (60/s)
	uint16 mbar = ReadMacInt16(0x0baa);	// MBarHeight (menu-bar height; non-zero once Finder drew it)

	// [BOOT]: one-shot at the FIRST idle (boot-ready). Kept as a diagnostic; it can fire before the
	// Finder finishes drawing the desktop, so prefer [READY] (below) for "desktop actually usable".
	// menubar= is included here too so its first-idle value can be compared with [READY]'s — if it's
	// already non-zero at first idle, MBarHeight isn't a useful extra readiness discriminator.
	// Guest OS version: SysVersion low-memory global ($015A), BCD-packed (0x0860 = 8.6.0).
	// Read on every idle until non-zero (may not be initialized at first idle).
	static uint16 detected_sysv = 0;
	if (!detected_sysv) {
		uint16 sv = ReadMacInt16(0x015a);
		if (sv >= 0x0700 && sv <= 0x0fff) {
			detected_sysv = sv;
			fprintf(stderr, "[SYSV] osVersion=0x%04X (%d.%d.%d)\n",
			        sv, (sv >> 8) & 0xf, (sv >> 4) & 0xf, sv & 0xf);
			fflush(stderr);
		}
	}

	static bool boot_emitted = false;
	if (!boot_emitted) {
		boot_emitted = true;
		fprintf(stderr, "[BOOT] idle frontApp='%s' modal=%d win=0x%x title='%s' menubar=%u ticks=%u (%.1fs)\n",
		        app, modal, front, title, mbar, ticks, ticks / 60.0);
		fflush(stderr);
		// Flip the stub-pressure trace (SS_STUB_TRACE) from boot to steady-state at first idle
		// (no-op unless that probe is enabled). See ppc-execute.cpp / MMU-NANOKERNEL-MP-PLAN.md.
		ss_stub_trace_steady();
		// Install the CopyBits-frequency probe (SS_COPYBITS_TRACE) now the System + traps are up.
		copybits_probe_install();
	}

	// [READY]: one-shot when the desktop is SETTLED — the Finder has been seen frontmost at least
	// once (CurApName churns ~6/s under cooperative MT, so a latch beats "currently Finder"), no
	// modal dialog is up, and that has held for a ~2 s dwell. More robust than first-idle, which can
	// fire mid-draw. MBarHeight ($0BAA, the menu-bar height — non-zero once the Finder has drawn its
	// menu bar) is emitted as instrumentation to evaluate folding it into the gate later. NOTE: this
	// fires only while the desktop stays non-modal, so on the benchmark disk it may not fire before a
	// Startup Item (Speedometer) grabs the foreground — that path gates on [APP], not [READY].
	static bool ready_emitted = false;
	static bool finder_seen = false;
	static uint32 settled_since = 0;		// guest tick the settled condition began (0 = not settled)
	if (strcmp(app, "Finder") == 0)
		finder_seen = true;
	bool settled = finder_seen && !modal;
	if (!settled)
		settled_since = 0;
	else if (settled_since == 0)
		settled_since = ticks;
	if (!ready_emitted && settled && settled_since != 0 && (ticks - settled_since) >= 120) {
		ready_emitted = true;
		fprintf(stderr, "[READY] desktop settled frontApp='%s' menubar=%u modal=%d ticks=%u (%.1fs)\n",
		        app, mbar, modal, ticks, ticks / 60.0);
		fflush(stderr);
	}

	// [APP]: emit on frontmost-app change, front-window MODAL change, or front-window TITLE change.
	//  - App changes let the harness wait for an app to launch (Speedometer from Startup Items).
	//    CurApName churns among background extensions under cooperative multitasking (~6/s of
	//    noise), so pure app-name emits are rate-limited (~0.5s).
	//  - Modal + title changes are emitted immediately (meaningful): the title distinguishes the
	//    important dialogs (e.g. Speedometer's "All Done!" = the benchmark-finished hook, vs its
	//    untitled splash/registration/"choose a disk" dialogs, which all reuse ONE window so they're
	//    NOT individually distinguishable — see docs). win=/title= are instrumentation.
	//    NOTE: we deliberately do NOT trigger on the front-window POINTER changing — under
	//    cooperative multitasking the frontmost window oscillates among background extensions every
	//    frame, which floods the log without adding signal.
	static char last_app[32] = { 0 };
	static int last_modal = -1;
	static char last_title[64] = { 0 };
	static uint32 last_app_emit = 0;
	bool app_changed = (strcmp(app, last_app) != 0);
	bool modal_changed = (modal != last_modal);
	bool title_changed = (strcmp(title, last_title) != 0);
	// Skip transient junk frames (garbage front-window title = a background-extension pseudo-window
	// momentarily frontmost). This suppresses the cooperative-multitasking churn that otherwise
	// floods the log, leaving the real foreground states (Finder 'Desktop', Speedometer dialogs).
	if (title_valid && (app_changed || modal_changed || title_changed)) {
		if (modal_changed || title_changed || (ticks - last_app_emit) >= 30) {	// 0.5s debounce on app churn
			fprintf(stderr, "[APP] frontApp='%s' modal=%d win=0x%x title='%s' ticks=%u\n",
			        app, modal, front, title, ticks);
			fflush(stderr);
			last_app_emit = ticks;
		}
		strncpy(last_app, app, sizeof(last_app) - 1);
		last_app[sizeof(last_app) - 1] = '\0';
		strncpy(last_title, title, sizeof(last_title) - 1);
		last_title[sizeof(last_title) - 1] = '\0';
		last_modal = modal;
	}
}


// E2E harness clean-shutdown trigger (ROADMAP A5). When the host requests shutdown (SIGUSR1 ->
// host_shutdown_requested), inject the ADB Power key — the same call the SDL window-close handler
// uses (video_sdl3.cpp). The guest routes the power key to the Shutdown Manager, which runs the
// REAL shutdown (procs + flush/unmount volumes) then powers off -> patched PowerOff() ->
// OP_POWEROFF -> "Shutdown complete." -> clean exit. Posting an event (vs re-entering via
// Execute68kTrap) is non-reentrant: the guest shuts down from its own top-level event loop.
//
// Sequence (driven across idle-hook cycles, each >= ~1 VBL apart):
//   1. ADB Power key down, hold a few cycles, up -> Mac OS raises the "Shut Down / Restart / Sleep"
//      confirmation dialog (verified on Mac OS 8.6 and 9.0.4).
//   2. WAIT for that dialog to actually be modal (windowKind==2), then press Return -> activates the
//      default "Shut Down" button -> the OS runs its real shutdown (procs + flush/unmount) ->
//      patched PowerOff() -> OP_POWEROFF -> "Shutdown complete." -> clean exit.
//   3. if the dialog is still up shortly after, the keystroke didn't take -> RESEND the Return.
// This is self-correcting (gate on the dialog + resend), mirroring the Python harness's _drive_until,
// rather than pressing Return blindly after a fixed delay. Return's Mac key code is 0x24, Power 0x7f.
// Posting events (vs Execute68kTrap) is non-reentrant: the guest acts from its own modal event loop.
static void e2e_check_host_shutdown(void)
{
	enum { PH_IDLE, PH_HOLD_POWER, PH_WAIT_DIALOG, PH_VERIFY, PH_DONE };
	static int phase = PH_IDLE;
	static int counter = 0;
	static int returns_sent = 0;

	if (phase == PH_IDLE) {
		if (!host_shutdown_requested)
			return;
		host_shutdown_requested = 0;
		fprintf(stderr, "[BOOT] host shutdown requested — ADB Power key down\n");
		fflush(stderr);
		ADBKeyDown(0x7f);
		phase = PH_HOLD_POWER; counter = 0;
		return;
	}

	// Is a modal dialog up front? (the Shut Down confirmation — WindowList head, windowKind==2.)
	uint32 fw = ReadMacInt32(0x9d6);
	bool dialog_up = fw && ((int16)ReadMacInt16(fw + 0x6c) == 2);

	switch (phase) {
	case PH_HOLD_POWER:			// hold the power key a few cycles, then release
		if (++counter < 4)
			break;
		ADBKeyUp(0x7f);
		fprintf(stderr, "[BOOT] Power key up; waiting for Shut Down dialog\n");
		fflush(stderr);
		phase = PH_WAIT_DIALOG; counter = 0; returns_sent = 0;
		break;
	case PH_WAIT_DIALOG:		// gate on the dialog being modal, then confirm with Return
		// Wait for the dialog to actually be up (not a blind cycle count); fall back after ~150 idle
		// cycles so a missed modal probe can never wedge the shutdown.
		if (!dialog_up && ++counter < 150)
			break;
		fprintf(stderr, "[BOOT] confirming Shut Down dialog (Return, attempt %d)\n", returns_sent + 1);
		fflush(stderr);
		ADBKeyDown(0x24); ADBKeyUp(0x24);	// Return = the default "Shut Down" button
		returns_sent++;
		phase = PH_VERIFY; counter = 0;
		break;
	case PH_VERIFY:				// if the dialog didn't clear, the keystroke didn't take -> resend
		if (++counter < 30)		// give the OS ~0.5 s to act on the keystroke
			break;
		if (dialog_up && returns_sent < 4) {
			fprintf(stderr, "[BOOT] dialog still modal; re-pressing Return\n");
			fflush(stderr);
			phase = PH_WAIT_DIALOG; counter = 0;
		} else {
			phase = PH_DONE;	// dialog cleared (shutting down) or out of retries
		}
		break;
	}
}


/*
 *  Execute EMUL_OP opcode (called by 68k emulator)
 */

void EmulOp(M68kRegisters *r, uint32 pc, int selector)
{
	D(bug("EmulOp %04x at %08x\n", selector, pc));
	switch (selector) {
		case OP_BREAK:				// Breakpoint
			printf("*** Breakpoint\n");
			Dump68kRegs(r);
			break;


		case OP_XPRAM1: {			// Read/write from/to XPRam
			uint32 len = r->d[3];
			uint8 *adr = Mac2HostAddr(r->a[3]);
			D(bug("XPRAMReadWrite d3: %08lx, a3: %p\n", len, adr));
			int ofs = len & 0xffff;
			len >>= 16;
			if (len & 0x8000) {
				len &= 0x7fff;
				for (uint32 i=0; i<len; i++)
					XPRAM[((ofs + i) & 0xff) + 0x1300] = *adr++;
			} else {
				for (uint32 i=0; i<len; i++)
					*adr++ = XPRAM[((ofs + i) & 0xff) + 0x1300];
			}
			break;
		}

		case OP_XPRAM2:				// Read from XPRam
			r->d[1] = XPRAM[(r->d[1] & 0xff) + 0x1300];
			break;

		case OP_XPRAM3:				// Write to XPRam
			XPRAM[(r->d[1] & 0xff) + 0x1300] = r->d[2];
			break;

		case OP_NVRAM1: {			// Read from NVRAM
			int ofs = r->d[0];
			r->d[0] = XPRAM[ofs & 0x1fff];
			bool localtalk = !(XPRAM[0x13e0] || XPRAM[0x13e1]);	// LocalTalk enabled?
			switch (ofs) {
				case 0x13e0:			// Disable LocalTalk (use EtherTalk instead)
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e1:
					if (localtalk)
						r->d[0] = 0x01;
					break;
				case 0x13e2:
					if (localtalk)
						r->d[0] = 0x00;
					break;
				case 0x13e3:
					if (localtalk)
						r->d[0] = 0x0a;
					break;
			}
			break;
		}

		case OP_NVRAM2:				// Write to NVRAM
			XPRAM[r->d[0] & 0x1fff] = r->d[1];
			break;

		case OP_NVRAM3:				// Read/write from/to NVRAM
			if (r->d[3]) {
				r->d[0] = XPRAM[(r->d[4] + 0x1300) & 0x1fff];
			} else {
				XPRAM[(r->d[4] + 0x1300) & 0x1fff] = r->d[5];
				r->d[0] = 0;
			}
			break;

		case OP_FIX_MEMTOP:			// Fixes MemTop in BootGlobs during startup
			D(bug("Fix MemTop\n"));
			WriteMacInt32(BootGlobsAddr - 20, RAMBase + RAMSize);	// MemTop
			r->a[6] = RAMBase + RAMSize;
			break;

		case OP_FIX_MEMSIZE: {		// Fixes physical/logical RAM size during startup
			D(bug("Fix MemSize\n"));
			uint32 diff = ReadMacInt32(0x1ef8) - ReadMacInt32(0x1ef4);
			WriteMacInt32(0x1ef8, RAMSize);			// Physical RAM size
			WriteMacInt32(0x1ef4, RAMSize - diff);	// Logical RAM size
			break;
		}

		case OP_FIX_BOOTSTACK:		// Fixes boot stack pointer in boot 3 resource
			D(bug("Fix BootStack\n"));
			r->a[1] = r->a[7] = RAMBase + RAMSize * 3 / 4;
			break;

		case OP_SONY_OPEN:			// Floppy driver functions
			r->d[0] = SonyOpen(r->a[0], r->a[1]);
			break;
		case OP_SONY_PRIME:
			r->d[0] = SonyPrime(r->a[0], r->a[1]);
			break;
		case OP_SONY_CONTROL:
			r->d[0] = SonyControl(r->a[0], r->a[1]);
			break;
		case OP_SONY_STATUS:
			r->d[0] = SonyStatus(r->a[0], r->a[1]);
			break;

		case OP_DISK_OPEN:			// Disk driver functions
			r->d[0] = DiskOpen(r->a[0], r->a[1]);
			break;
		case OP_DISK_PRIME:
			r->d[0] = DiskPrime(r->a[0], r->a[1]);
			break;
		case OP_DISK_CONTROL:
			r->d[0] = DiskControl(r->a[0], r->a[1]);
			break;
		case OP_DISK_STATUS:
			r->d[0] = DiskStatus(r->a[0], r->a[1]);
			break;

		case OP_CDROM_OPEN:			// CD-ROM driver functions
			r->d[0] = CDROMOpen(r->a[0], r->a[1]);
			break;
		case OP_CDROM_PRIME:
			r->d[0] = CDROMPrime(r->a[0], r->a[1]);
			break;
		case OP_CDROM_CONTROL:
			r->d[0] = CDROMControl(r->a[0], r->a[1]);
			break;
		case OP_CDROM_STATUS:
			r->d[0] = CDROMStatus(r->a[0], r->a[1]);
			break;

		case OP_AUDIO_DISPATCH:		// Audio component functions
			r->d[0] = AudioDispatch(r->a[3], r->a[4]);
			break;

		case OP_SOUNDIN_OPEN:		// Sound input driver functions
			r->d[0] = SoundInOpen(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_PRIME:
			r->d[0] = SoundInPrime(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CONTROL:
			r->d[0] = SoundInControl(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_STATUS:
			r->d[0] = SoundInStatus(r->a[0], r->a[1]);
			break;
		case OP_SOUNDIN_CLOSE:
			r->d[0] = SoundInClose(r->a[0], r->a[1]);
			break;

		case OP_ADBOP:				// ADBOp() replacement
			ADBOp(r->d[0], Mac2HostAddr(ReadMacInt32(r->a[0])));
			break;

		case OP_INSTIME:			// InsTime() replacement
			r->d[0] = InsTime(r->a[0], r->d[1]);
			break;
		case OP_RMVTIME:			// RmvTime() replacement
			r->d[0] = RmvTime(r->a[0]);
			break;
		case OP_PRIMETIME:			// PrimeTime() replacement
			r->d[0] = PrimeTime(r->a[0], r->d[0]);
			break;

		case OP_MICROSECONDS:		// Microseconds() replacement
			Microseconds(r->a[0], r->d[0]);
			break;

		case OP_ZERO_SCRAP:			// ZeroScrap() patch
			ZeroScrap();
			break;

		case OP_PUT_SCRAP:			// PutScrap() patch
			PutScrap(ReadMacInt32(r->a[7] + 8), Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_GET_SCRAP:			// GetScrap() patch
			GetScrap((void **)Mac2HostAddr(ReadMacInt32(r->a[7] + 4)), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 12));
			break;

		case OP_DEBUG_STR:			// DebugStr() shows warning message
			if (PrefsFindBool("nogui")) {
				uint8 *pstr = Mac2HostAddr(ReadMacInt32(r->a[7] + 4));
				char str[256];
				int i;
				for (i=0; i<pstr[0]; i++)
					str[i] = pstr[i+1];
				str[i] = 0;
				WarningAlert(str);
			}
			break;

		case OP_INSTALL_DRIVERS: {	// Patch to install our own drivers during startup
			// Install drivers
			InstallDrivers();

			// Patch MakeExecutable()
			MakeExecutableTvec = FindLibSymbol("\023PrivateInterfaceLib", "\016MakeExecutable");
			D(bug("MakeExecutable TVECT at %08x\n", MakeExecutableTvec));
			WriteMacInt32(MakeExecutableTvec, NativeFunction(NATIVE_MAKE_EXECUTABLE));
#if !EMULATED_PPC
			WriteMacInt32(MakeExecutableTvec + 4, (uint32)TOC);
#endif

			// Patch DebugStr()
			static const uint8 proc_template[] = {
				M68K_EMUL_OP_DEBUG_STR >> 8, M68K_EMUL_OP_DEBUG_STR & 0xFF,
				0x4e, 0x74,			// rtd	#4
				0x00, 0x04
			};
			BUILD_SHEEPSHAVER_PROCEDURE(proc);
			WriteMacInt32(0x1dfc, proc);
			break;
		}

		case OP_NAME_REGISTRY:		// Patch Name Registry and initialize CallUniversalProc
			r->d[0] = (uint32)-1;
			PatchNameRegistry();
			InitCallUniversalProc();
			break;

		case OP_RESET:				// Early in MacOS reset
			D(bug("*** RESET ***\n"));
			tick_inhibit = true;
			CDROMRemount(); // for System 7.x
			TimerReset();
			MacOSUtilReset();
			EtherResetCachedAllocation();
			ether_reset();
			AudioReset();
#ifdef USE_SDL_AUDIO
			PlayStartupSound();
#endif
			// Enable DR emulator (disabled for now)
			if (PrefsFindBool("jit68k") && 0) {
				D(bug("DR activated\n"));
				WriteMacInt32(KernelDataAddr + 0x17a0, 3);		// Prepare for DR emulator activation
				WriteMacInt32(KernelDataAddr + 0x17c0, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x17c4, DR_CACHE_SIZE);
				WriteMacInt32(KernelDataAddr + 0x1b04, DR_CACHE_BASE);
				WriteMacInt32(KernelDataAddr + 0x1b00, DR_EMULATOR_BASE);
				memcpy((void *)DR_EMULATOR_BASE, (void *)(ROMBase + 0x370000), DR_EMULATOR_SIZE);
				MakeExecutable(0, DR_EMULATOR_BASE, DR_EMULATOR_SIZE);
			}
			tick_inhibit = false;
			break;

		case OP_IRQ:			// Level 1 interrupt
			WriteMacInt16(ReadMacInt32(KernelDataAddr + 0x67c), 0);	// Clear interrupt
			r->d[0] = 0;
			if (HasMacStarted()) {
				if (InterruptFlags & INTFLAG_VIA) {
					ClearInterruptFlag(INTFLAG_VIA);
#if !PRECISE_TIMING
					TimerInterrupt();
#endif
					ExecuteNative(NATIVE_VIDEO_VBL);

					static int tick_counter = 0;
					if (++tick_counter >= 60) {
						tick_counter = 0;
						SonyInterrupt();
						DiskInterrupt();
						CDROMInterrupt();
					}

					r->d[0] = 1;		// Flag: 68k interrupt routine executes VBLTasks etc.
				}
				if (InterruptFlags & INTFLAG_SERIAL) {
					ClearInterruptFlag(INTFLAG_SERIAL);
					SerialInterrupt();
				}
				if (InterruptFlags & INTFLAG_ETHER) {
					ClearInterruptFlag(INTFLAG_ETHER);
					ExecuteNative(NATIVE_ETHER_IRQ);
				}
				if (InterruptFlags & INTFLAG_TIMER) {
					ClearInterruptFlag(INTFLAG_TIMER);
					TimerInterrupt();
				}
				if (InterruptFlags & INTFLAG_AUDIO) {
					ClearInterruptFlag(INTFLAG_AUDIO);
					AudioInterrupt();
				}
				if (InterruptFlags & INTFLAG_ADB) {
					ClearInterruptFlag(INTFLAG_ADB);
					ADBInterrupt();
				}
			} else
				r->d[0] = 1;
			break;

		case OP_SCSI_DISPATCH: {	// SCSIDispatch() replacement
			uint32 ret = ReadMacInt32(r->a[7]);
			uint16 sel = ReadMacInt16(r->a[7] + 4);
			r->a[7] += 6;
//			D(bug("SCSIDispatch(%d)\n", sel));
			int stack;
			switch (sel) {
				case 0:		// SCSIReset
					WriteMacInt16(r->a[7], SCSIReset());
					stack = 0;
					break;
				case 1:		// SCSIGet
					WriteMacInt16(r->a[7], SCSIGet());
					stack = 0;
					break;
				case 2:		// SCSISelect
				case 11:	// SCSISelAtn
					WriteMacInt16(r->a[7] + 2, SCSISelect(ReadMacInt8(r->a[7] + 1)));
					stack = 2;
					break;
				case 3:		// SCSICmd
					WriteMacInt16(r->a[7] + 6, SCSICmd(ReadMacInt16(r->a[7]), Mac2HostAddr(ReadMacInt32(r->a[7] + 2))));
					stack = 6;
					break;
				case 4:		// SCSIComplete
					WriteMacInt16(r->a[7] + 12, SCSIComplete(ReadMacInt32(r->a[7]), ReadMacInt32(r->a[7] + 4), ReadMacInt32(r->a[7] + 8)));
					stack = 12;
					break;
				case 5:		// SCSIRead
				case 8:		// SCSIRBlind
					WriteMacInt16(r->a[7] + 4, SCSIRead(ReadMacInt32(r->a[7])));
					stack = 4;
					break;
				case 6:		// SCSIWrite
				case 9:		// SCSIWBlind
					WriteMacInt16(r->a[7] + 4, SCSIWrite(ReadMacInt32(r->a[7])));
					stack = 4;
					break;
				case 10:	// SCSIStat
					WriteMacInt16(r->a[7], SCSIStat());
					stack = 0;
					break;
				case 12:	// SCSIMsgIn
					WriteMacInt16(r->a[7] + 4, 0);
					stack = 4;
					break;
				case 13:	// SCSIMsgOut
					WriteMacInt16(r->a[7] + 2, 0);
					stack = 2;
					break;
				case 14:	// SCSIMgrBusy
					WriteMacInt16(r->a[7], SCSIMgrBusy());
					stack = 0;
					break;
				default:
					printf("FATAL: SCSIDispatch: illegal selector\n");
					stack = 0;
					//!! SysError(12)
			}
			r->a[0] = ret;
			r->a[7] += stack;
			break;
		}

		case OP_SCSI_ATOMIC:		// SCSIAtomic() replacement
			D(bug("SCSIAtomic\n"));
			r->d[0] = (uint32)-7887;
			break;

		case OP_CHECK_SYSV: {		// Check we are not using MacOS < 8.1 with a NewWorld ROM
			r->a[1] = r->d[1];
			r->a[0] = ReadMacInt32(r->d[1]);
			uint32 sysv = ReadMacInt16(r->a[0]);
			D(bug("Detected MacOS version %d.%d.%d\n", (sysv >> 8) & 0xf, (sysv >> 4) & 0xf, sysv & 0xf));
			if (ROMType == ROMTYPE_NEWWORLD && sysv < 0x0801)
				r->d[1] = 0;
			break;
		}

		case OP_NTRB_17_PATCH:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			if (ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH2:
			r->a[7] += 8;
			PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH3:
			r->a[2] = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_NTRB_17_PATCH4:
			r->d[0] = ReadMacInt16(r->a[7]);
			r->a[7] += 2;
		 	D(bug("%d %d\n", ReadMacInt16(r->a[2]), ReadMacInt16(r->a[2] + 6)));
			if (ReadMacInt16(r->a[2]) == 11 && ReadMacInt16(r->a[2] + 6) == 17)
				PatchNativeResourceManager();
			break;

		case OP_CHECKLOAD: {		// vCheckLoad() patch
			uint32 type = ReadMacInt32(r->a[7]);
			r->a[7] += 4;
			int16 id = ReadMacInt16(r->a[2]);
			if (r->a[0] == 0)
				break;
			uint32 adr = ReadMacInt32(r->a[0]);
			if (adr == 0)
				break;
			uint16 *p = (uint16 *)Mac2HostAddr(adr);
			uint32 size = ReadMacInt32(adr - 8) & 0xffffff;
			CheckLoad(type, id, p, size);
			break;
		}

		case OP_EXTFS_COMM:			// External file system routines
			WriteMacInt16(r->a[7] + 14, ExtFSComm(ReadMacInt16(r->a[7] + 12), ReadMacInt32(r->a[7] + 8), ReadMacInt32(r->a[7] + 4)));
			break;

		case OP_EXTFS_HFS:
			WriteMacInt16(r->a[7] + 20, ExtFSHFS(ReadMacInt32(r->a[7] + 16), ReadMacInt16(r->a[7] + 14), ReadMacInt32(r->a[7] + 10), ReadMacInt32(r->a[7] + 6), ReadMacInt16(r->a[7] + 4)));
			break;

		case OP_IDLE_TIME:
			force_altivec_idle_service();
			e2e_emit_idle_signals();
			e2e_check_host_shutdown();	// inject Power key (with dwell) if host asked (A5)
			ui_introspect_service();
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);
			break;

		case OP_IDLE_TIME_2:
			force_altivec_idle_service();
			e2e_emit_idle_signals();	// some ROMs patch the 0x70fe SynchIdleTime variant (A5)
			e2e_check_host_shutdown();
			ui_introspect_service();
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->d[0] = (uint32)-2;
			break;

		case OP_POWEROFF:			// Guest Mac OS shut down (Special > Shut Down)
			printf("\n"
			       "    ┌───────────┐\n"
			       "    │  ┌─────┐  │\n"
			       "    │  │ ◠ ◠ │  │\n"
			       "    │  │ ╰─╯ │  │\n"
			       "    │  └─────┘  │\n"
			       "    └────┬──────┘\n"
			       "         │\n"
			       "  Shutdown complete.\n\n");
			power_off_requested = true;
			break;

		default:
			printf("FATAL: EMUL_OP called with bogus selector %08x\n", selector);
			QuitEmulator();
			break;
	}
}
