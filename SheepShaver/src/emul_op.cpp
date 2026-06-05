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

#define DEBUG 0
#include "debug.h"

extern bool tick_inhibit;

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
// Is `a` a guest pointer we can safely dereference? Valid Mac pointers/handles are even-aligned and
// live in mapped Mac RAM. A WILD handle (a background pseudo-window's titleHandle can be junk) outside
// RAM would, under DIRECT_ADDRESSING, deref to UNMAPPED host memory and SIGSEGV the emulator — so
// every guest deref below is gated on this. (Reads stay within [0, RAMBase+RAMSize), which the live
// window pointers satisfy; anything beyond RAM is rejected before the deref.)
static inline bool e2e_guest_ptr_ok(uint32 a)
{
	return (a & 1) == 0 && a >= 0x100 && a < RAMBase + RAMSize;
}

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
	if (!e2e_guest_ptr_ok(win))
		return false;
	uint32 hdl = ReadMacInt32(win + 0x86);		// titleHandle
	if (!hdl)
		return true;			// untitled window = valid empty
	if (!e2e_guest_ptr_ok(hdl))
		return false;
	uint32 ptr = ReadMacInt32(hdl);				// *titleHandle -> Str255
	if (!ptr)
		return true;
	if (!e2e_guest_ptr_ok(ptr))
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

	// [BOOT]: one-shot at the FIRST idle (boot-ready). Kept as a diagnostic; it can fire before the
	// Finder finishes drawing the desktop, so prefer [READY] (below) for "desktop actually usable".
	static bool boot_emitted = false;
	if (!boot_emitted) {
		boot_emitted = true;
		fprintf(stderr, "[BOOT] idle frontApp='%s' modal=%d win=0x%x title='%s' ticks=%u (%.1fs)\n",
		        app, modal, front, title, ticks, ticks / 60.0);
		fflush(stderr);
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
		uint16 mbar = ReadMacInt16(0x0baa);	// MBarHeight
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
//   1. ADB Power key down+up (with dwell) -> Mac OS raises the "Shut Down / Restart / Sleep"
//      confirmation dialog (verified on Mac OS 9.0.4).
//   2. wait for the dialog to appear, then press Return -> activates the default "Shut Down"
//      button -> the OS runs its real shutdown (procs + flush/unmount) -> patched PowerOff() ->
//      OP_POWEROFF -> "Shutdown complete." -> clean exit.
// Technique: ADB key DWELL — ADBKeyDown/Up buffer into key_buffer, drained in one pass by
// ADBInterrupt on the 60 Hz VBL (adb.cpp); back-to-back down+up = instantaneous press the OS
// ignores, so we hold across cycles. Return's Mac key code is 0x24. Posting events (vs
// Execute68kTrap) is non-reentrant: the guest acts from its own top-level/modal event loop.
static void e2e_check_host_shutdown(void)
{
	static int phase = 0;		// 0=idle 1=hold power 2=wait-for-dialog 3=done
	static int counter = 0;
	switch (phase) {
	case 0:
		if (!host_shutdown_requested)
			return;
		host_shutdown_requested = 0;
		fprintf(stderr, "[BOOT] host shutdown requested — ADB Power key down\n");
		fflush(stderr);
		ADBKeyDown(0x7f);
		phase = 1; counter = 0;
		break;
	case 1:				// hold the power key a few cycles, then release
		if (++counter < 4)
			break;
		ADBKeyUp(0x7f);
		fprintf(stderr, "[BOOT] Power key up; waiting for Shut Down dialog\n");
		fflush(stderr);
		phase = 2; counter = 0;
		break;
	case 2:				// let the dialog appear (~1s of idle cycles), then confirm with Return
		if (++counter < 45)
			break;
		fprintf(stderr, "[BOOT] confirming Shut Down dialog (Return)\n");
		fflush(stderr);
		ADBKeyDown(0x24); ADBKeyUp(0x24);	// Return = default "Shut Down" button
		phase = 3;
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
			e2e_emit_idle_signals();
			e2e_check_host_shutdown();	// inject Power key (with dwell) if host asked (A5)
			// Sleep if no events pending
			if (ReadMacInt32(0x14c) == 0)
				idle_wait();
			r->a[0] = ReadMacInt32(0x2b6);
			break;

		case OP_IDLE_TIME_2:
			e2e_emit_idle_signals();	// some ROMs patch the 0x70fe SynchIdleTime variant (A5)
			e2e_check_host_shutdown();
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
