# Landscape: Classic Mac OS Video / Graphics Acceleration in Emulation

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Landscape survey of classic Mac OS video/graphics acceleration in emulation.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Research survey for SheepShaver (macos-arm64 branch). Question: what does "nascent
video acceleration for classic macOS" look like across the emulator ecosystem, and
which architecture is worth pursuing for SheepShaver?

Date: 2026-06-02. All findings drawn from project source, GitHub, and community forums
(E-Maculation, MacOS9Lives, VOGONS, qemu-devel).

---

## TL;DR — the architectural comparison (centerpiece)

There are exactly three ways an emulator can put pixels on the host screen for a classic
Mac OS guest. SheepShaver uses (a). The interesting frontier is (c).

| | (a) Framebuffer-only | (b) Emulate real video hardware | (c) Custom guest .ndrv + host rendering |
|---|---|---|---|
| **What it is** | Present a dumb linear framebuffer; guest's generic driver just writes pixels; host blits the whole (or dirty) buffer to a window. | Cycle-accurate emulation of a real chip (ATI Mach64/Rage, Control, Platinum) so Mac OS loads its *real* native `.ndrv` + accelerator extension. | Ship a paravirtual `.ndrv` into the guest that talks a private MMIO protocol to the host, which executes the operation natively. |
| **Who does it** | **SheepShaver, Basilisk II, MoL framebuffer mode** | **DingusPPC** | **QEMU (qemu_vga.ndrv)**, MoL's accelerated path |
| **Accel?** | None. QuickDraw runs in the emulated CPU; host only blits. | In principle yes (native QuickDraw accel ops hit emulated registers) — in practice **not implemented yet anywhere**. | Yes, incrementally: hardware cursor, mode/resolution changes today; blit/2D offload is the natural extension. |
| **Guest changes** | None | None | Requires installing a custom extension in the guest |
| **Emulator effort** | Low (done) | **Very high** — register-accurate chip + working VRAM/CRTC/RAMDAC | Medium — define a protocol, write the guest driver once, host handler per op |
| **Maturity** | Shipping for 25 years | DingusPPC ATI Mach64 GX: boots, *no acceleration*, GUI/desktop bring-up still WIP | qemu_vga.ndrv: mature framebuffer + HW cursor + dynamic resolution; **2D/3D accel explicitly not done** |

**Verdict for SheepShaver: pursue (c), not (b).** SheepShaver already *is* a paravirtual
video driver host — `video.cpp` is literally a hand-written virtual `.ndrv` (open/control/
status/close + a PCI native-driver entry). The cheapest real win is to extend that existing
control/status protocol with a hardware cursor and offloaded rectangle fills/blits, exactly
the path QEMU is walking. Approach (b) means writing a register-accurate ATI Mach64 — months
of work for a chip whose acceleration *no emulator has gotten working yet*, and it throws
away SheepShaver's whole existing video stack. (a) is the floor we already have.

The single highest-leverage, lowest-risk improvement: **a hardware/host cursor** (decouple
cursor from framebuffer redraw) and **VOSF-style dirty-region blit tuning** — both stay
inside the existing model and need zero guest changes.

---

## 1. How SheepShaver does video today — framebuffer-only (approach a)

Files: `SheepShaver/src/video.cpp` (1139 lines, platform-independent), `SheepShaver/src/Unix/video_x.cpp`
(2635 lines, host/SDL/X11 side). GPL-2.0+.

- `video.cpp` is a **virtual video driver**: it implements the Mac OS DRVR/`.ndrv` contract
  directly — `VideoDriverOpen`, a `control` routine (`cscSetMode`, `cscSetEntries` palette,
  gamma via `set_gamma`, `cscSetGray`), a `status` routine, `close`, and a "Native (PCI) driver
  entry" (line ~1027). Global `screen_base` is the guest-visible framebuffer base; `VModes[]`
  is the mode table built at `VideoInit`.
- No hardware is emulated. The guest's generic Toolbox QuickDraw renders into the linear
  framebuffer in emulated PPC; SheepShaver's host side just copies those bytes to a window.
- The host blit + dirty tracking (VOSF — Video On SEGV Fault) lives in `video_x.cpp` /
  `video_blit.h`: pages of the framebuffer are write-protected, a SIGSEGV handler marks dirty
  pages, and only dirty regions are converted (endian/pixel-format) and blitted to SDL.
- So: **all drawing cost is in the emulated CPU**; the host contributes only a periodic
  format-convert + blit. There is no path for the guest to ask the host to draw anything.

This is the baseline that (b) and (c) are alternatives to.

---

## 2. DingusPPC — emulate real video hardware (approach b)

- URL: https://github.com/dingusdev/dingusppc · License: **GPL-3.0**
- Video devices (`devices/video/`): `control.cpp` (Control), `pdmonboard.cpp` (Platinum/PDM
  on-board), `valkyrie.cpp`, `appleramdac.cpp`, `displayid.cpp`/EDID, and the ATI path
  `atirage.cpp` + `atimach64gx.cpp` + `atimach64defs.h`. (`sixty6.cpp` is a video *encoder*,
  not a GUI accelerator.)
- **Does emulating a real ATI chip let Mac OS use native accelerated QuickDraw drivers?**
  In theory yes — that is the whole appeal: Mac OS loads its real ATI `.ndrv` and the
  "ATI Graphics Accelerator" extension, and accelerated QuickDraw ops hit emulated MMIO.
  **In practice: not yet.** The project manual lists the G3 Beige as "No ATI Rage
  acceleration"; the ATI Mach64 GX controller emulation (commit 0df1b2c4) boots but the GUI
  engine isn't fully implemented — the documented workaround is to *disable* the ATI
  Accelerator extension. So this approach is unproven even in its flagship implementation.
- Maturity: most-complete machines are PM 6100 and G3 Beige; both still bringing up
  SCSI/ATA. Actively developed (Internet Archive snapshot 2025-07).
- **What we could take:** Reference register layouts and CRTC/RAMDAC/EDID semantics if we
  ever emulated a real chip. But GPL-3.0 vs SheepShaver's GPL-2.0+ is a **license-incompatibility
  hazard** for copying code (GPL-2-only callers can't link GPL-3). Treat as documentation, not
  a code source. **Verdict: do not pursue this path for SheepShaver** — high cost, unproven
  acceleration, license friction, discards the existing video stack.

---

## 3. QEMU PPC Mac (mac99 / g3beige) + the qemu_vga.ndrv path (approach c)

- QEMU presents a **standard PC-style VGA device** (`-vga std`) to the mac99/g3beige machine,
  *not* a Mac-specific chip. Stock Mac OS has no driver for it.
- The community fix is a **paravirtual native driver**: `qemu_vga.ndrv`, shipped in
  `qemu/pc-bios/qemu_vga.ndrv`, source in **https://github.com/qemu/QemuMacDrivers**
  (`QemuVGADriver/`). License: **GPL-2.0** (compatible with SheepShaver). Authored by
  Mark Cave-Ayland (mcayland) et al.; upstreamed to QEMU in 2017. OpenBIOS NVRAM var
  `vga-ndrv?=true` selects it.
- Capabilities today: linear framebuffer + endian handling, **dynamic resolution / mode set**
  (resolutions read from EDID, up to 2560x1080), and **hardware cursor** work (decouples cursor
  from the framebuffer to fix absolute-mode responsiveness). Tested on Mac OS 9.2.x and OS X 10.2.
- **2D/3D acceleration: explicitly NOT done.** qemu-devel notes the VGA ndrv "needs to be
  updated to pass 2D/3D rendering calls from the guest to the virtio-gpu device." So even the
  most mature paravirtual driver is still nascent on accel — it stops at framebuffer + cursor +
  mode-set. There is also a virtio-gpu/virtio family of drivers in the same repo (newer, more
  experimental — community "Screamer" work and the MacOS9Lives forum revolve around audio
  (Screamer = the Sound Manager driver) and virtio storage/net more than video accel).
- **What we could take (highest-value finding):** QemuMacDrivers is the **single best
  reference for SheepShaver's own path** — it is GPL-2.0, it is a working PPC Mac `.ndrv`
  built against the same Toolbox driver model SheepShaver already emulates, and it demonstrates
  exactly the incremental protocol (mode-set, HW cursor, the planned blit offload) we'd add to
  `video.cpp`. **Verdict: study/borrow heavily.**

Note "Screamer" in the question is the QEMU *sound* driver effort (Sound Manager / audio),
not video — relevant context but orthogonal to acceleration.

---

## 4. Paravirtual / custom .ndrv display drivers for Mac OS 9

- **qemu_vga.ndrv** (above) is the canonical example. Source: QemuMacDrivers (GPL-2.0),
  maintained loosely (low commit count, but upstreamed into QEMU proper). The build artifact
  lives at `qemu/pc-bios/qemu_vga.ndrv`.
- **elliotnunn/x-ndrv** (https://github.com/elliotnunn/x-ndrv) — a catalogue of Mac OS X
  video `.ndrv`s that are potentially 7/8/9-compatible; useful for understanding the on-disk
  ndrv format and dependency surface, not an accel project itself.
- MacOS9Lives thread "Creating an OS 9 'NDRV' driver for unsupported Geforce cards" shows the
  community can author display ndrvs from scratch — i.e. the guest-driver authoring skill exists.
- **What we could take:** the ndrv ABI knowledge and the qemu_vga driver structure are directly
  reusable as the guest half of a SheepShaver accel protocol. **Verdict: this is the body of
  prior art the (c) approach builds on.**

---

## 5. Mac-on-Linux (MoL) video

- MoL (SourceForge `mac-on-linux`, GPL) ran Mac OS 9/X guests on PPC Linux and had both a
  framebuffer path and an **accelerated video driver** delivered as a guest-side `.ndrv` talking
  to the host MoL kernel module — architecturally identical to approach (c).
- Source availability: MoL is GPL and the tree (including the `mol_video`/osi video driver and
  the guest driver sources) is in the SourceForge repository, but the project is long dormant
  (Leopard/2.6.24-era), PPC-host-specific, and uses an OSI (Operating System Interface)
  hypercall mechanism tied to its kernel module.
- **What we could take:** MoL is the *historical proof* that a guest-driver + host-render
  protocol delivers real acceleration on classic Mac OS, and it's GPL so the **guest-side .ndrv
  design and the OSI call conventions are adaptable**. The host side is PPC/Linux-kernel-bound
  and not directly portable to SheepShaver's userspace macOS arm64 model, but the protocol shape
  is a useful second reference alongside QemuMacDrivers. **Verdict: reference for design, not
  code reuse.**

---

## 6. Basilisk II / vMac / Executor — QuickDraw-level approaches

- **Basilisk II:** same framebuffer-only model as SheepShaver (shared lineage). A 2000s
  B2-devel thread "Native QuickDraw acceleration" floated intercepting QuickDraw traps to
  render host-side, but it never shipped — for a 68K guest you'd have to trap and reimplement
  QuickDraw entry points, which is fragile and version-specific.
- **Executor (ARDI; modern fork autc04/executor):** the most aggressive approach — it does **not
  emulate a Mac at all**; it **reimplements the Toolbox and QuickDraw natively** and translates
  68K via syn68k. QuickDraw calls become host drawing calls, so it's "accelerated" by
  construction. But this is a clean-room OS reimplementation (System-6/7 era, no real Mac OS),
  a completely different product category from a full-system emulator like SheepShaver, and
  doesn't run Mac OS 8/9. **Verdict: instructive ceiling, not a path** — reimplementing
  QuickDraw for Mac OS 9 is out of scope.
- **vMac:** framebuffer-only, no acceleration work of note.

---

## Recommendation

1. **Stay in approach (c); SheepShaver is already 80% there.** `video.cpp` is a virtual ndrv —
   extend its control/status protocol rather than emulate silicon (b) or reimplement QuickDraw (6).
2. **First two wins, no guest install required, inside the current model:**
   - Host/hardware cursor (decouple cursor from framebuffer redraw) — exactly QEMU's first accel step.
   - Tighten VOSF dirty-rect blit (`video_x.cpp`) and format-convert cost.
3. **Reference, in priority order:** QemuMacDrivers (GPL-2.0, same ndrv model, the live template) >
   MoL guest-driver/OSI protocol (GPL, design only) > DingusPPC (GPL-3.0, register docs only — do
   not copy code).
4. **Do not** start a register-accurate ATI Mach64 (b): highest cost, license friction, and no
   emulator has working ATI QuickDraw acceleration to even validate against.

## Sources

- DingusPPC: https://github.com/dingusdev/dingusppc · manual: https://github.com/dingusdev/dingusppc/blob/master/zdocs/users/manual.md · ATI Mach64 GX commit: https://git.applefritter.com/Macintosh-Tools/dingusppc/commit/0df1b2c408cc5ae37940149d6c12135db81826a5
- QemuMacDrivers: https://github.com/qemu/QemuMacDrivers · qemu_vga.ndrv binary: https://github.com/qemu/qemu/blob/master/pc-bios/qemu_vga.ndrv · upstream patchset: https://patchew.org/QEMU/1493646214-3342-1-git-send-email-mark.cave-ayland@ilande.co.uk/
- VGA HW cursor / 2D-accel discussion: https://lists.nongnu.org/archive/html/qemu-devel/2022-08/msg01119.html
- Mac-on-Linux: https://sourceforge.net/p/mac-on-linux/
- x-ndrv catalogue: https://github.com/elliotnunn/x-ndrv · MacOS9Lives custom ndrv: http://macos9lives.com/smforum/index.php?topic=4250.0
- Executor: https://github.com/autc04/executor · https://en.wikipedia.org/wiki/Executor_(software) · syn68k: https://www.emaculation.com/doku.php/syn68k
- Basilisk II native QuickDraw thread: https://sourceforge.net/p/basilisk/mailman/message/2165218/
- ATI Mac drivers reference: https://gona.mactar.hu/ATI_Mac/
