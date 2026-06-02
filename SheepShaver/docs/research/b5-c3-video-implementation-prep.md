# B5 + C3 Video Implementation Prep — SheepShaver (macos-arm64)

Implementation-prep for backlog items **B5 (host hardware cursor)** and **C3 (paravirtual
video acceleration protocol)**. Date: 2026-06-02.

> **Coordination:** This prep touches only `src/video.cpp`, `src/gfxaccel.cpp`,
> `src/SDL/video_sdl2.cpp`, `src/SDL/vnc_server.cpp`, `src/prefs_items.cpp`. It does **not**
> touch the JIT files another agent owns (`ppc-jit.cpp/.h`, `ppc-cpu.cpp`, `jit-test/run.sh`).
> No code was edited for this prep — output is this document only.

---

## Headline verdict (this reframes both items)

Both B5 and C3 are **already substantially implemented in this tree** — they are *enable +
verify + extend* tasks, not *design-from-scratch* tasks. The landscape-3 survey described
them as greenfield because it surveyed the ecosystem, not the local code. Reading the actual
source changes the picture:

| Item | Believed status (landscape-3) | **Actual status in this tree** |
|------|-------------------------------|-------------------------------|
| B5 host cursor | "natural extension, QEMU's first step" | Guest protocol (`video.cpp`) **fully implemented**; SDL2 host rendering (`MagCursor`/`SDL_CreateCursor`) **fully implemented**; gated behind `hardcursor` pref (**default false**). Real gap: **VNC path is a hard no-op** + B&W-only. |
| C3 paravirt accel | "extend control/status protocol; QEMU is the reference" | `gfxaccel.cpp` **already implements** Native QuickDraw acceleration (fillrect, invert, srcCopy bitblt) via the `NQDMisc(6,…)` accelerator-hook mechanism; **default on** (`gfxaccel=true`). C3 = *widen NQD coverage*, not invent a protocol. |

The most useful thing this doc can do is document what exists, where the real (narrow) gaps
are, and give an honest ceiling for driver-level accel. The external references (QEMU, MOL)
turned out to be *confirmatory* and in one case **less capable than SheepShaver's own code**.

---

## 1. Current video stack map (file:line)

**Which host video file is actually built:** `src/SDL/video_sdl2.cpp` (USE_SDL2 in
`src/Unix/config.h:500`; listed in `SYSSRCS` at `src/Unix/Makefile:25` as
`../SDL/video_sdl2.cpp`). **`src/Unix/video_x.cpp` is NOT compiled** — it is the legacy X11
reference. It contains a complete X11 cursor implementation
(`video_can_change_cursor`/`video_set_cursor` at `video_x.cpp:2308/2318`, `XCreatePixmapCursor`
at 2576) which is useful as a second reference but is dead code in this build.

**Guest-side virtual driver — `src/video.cpp` (platform-independent, the hand-written `.ndrv`):**
- `MacCursor[68]` global (`video.cpp:51`) — 4-byte header (`{16,1,hotX,hotY}`) + 32 bytes
  1-bit image + 32 bytes 1-bit mask. This is the shared cursor handoff buffer between guest
  protocol and host renderer.
- `VidLocals` cursor state init: `video.cpp:171-178` (`cursorHardware`, `cursorX/Y`,
  `cursorVisible`, `cursorSet`, `cursorHotFlag`, `cursorHotX/Y`); struct decl in
  `src/include/video.h:120-130`.
- `UseHardwareCursor()` `video.cpp:150` → returns `video_can_change_cursor()`.
- **`cscSetHardwareCursor` control code** `video.cpp:470-519`: reads the guest `CursorImage`
  (pixmap + bitmask), **rejects anything but `rowBytes==2`** (1-bit 16×16 B&W) →
  `controlErr` (`video.cpp:487-489`), copies image+mask into `MacCursor[4..]`, calls
  `video_set_cursor()`.
- **`cscDrawHardwareCursor` control code** `video.cpp:521-588`: updates `cursorX/Y/Visible`,
  infers hotspot (heuristic, special-cases the standard arrow at `video.cpp:560-566`), calls
  `video_set_cursor()` on change.
- Display driver also answers Driver Gestalt (`video.cpp:590`), `cscSetMode`/`cscSwitchMode`,
  `cscSetEntries` (palette), gamma, gray — the full DRVR/`.ndrv` control/status contract.
- `cscSupportsHardwareCursor` is implicitly satisfied: the guest learns HW-cursor capability
  because `cscSetHardwareCursor` returns `noErr` (vs `controlErr`) when
  `csSave->cursorHardware` is set. `cursorHardware` is assigned host-side (see §2).

**Host-side renderer — `src/SDL/video_sdl2.cpp`:**
- `sdl_cursor` SDL_Cursor* `video_sdl2.cpp:296`.
- `MagCursor(bool hot)` `video_sdl2.cpp:1231-1252`: scales `MacCursor` image+mask to window
  magnification, builds an SDL cursor via **`SDL_CreateCursor`** (1-bit B&W path — *not*
  `SDL_CreateColorCursor`).
- Cursor capability + initial setup `video_sdl2.cpp:1394-1415`: `hardware_cursor =
  video_can_change_cursor()`; if true, creates+sets the SDL cursor and writes back
  `private_data->cursorHardware = hardware_cursor` (`video_sdl2.cpp:1405`) — *this* is how the
  guest is told HW cursor is available.
- **`video_can_change_cursor()`** `video_sdl2.cpp:2381-2384` → `return PrefsFindBool("hardcursor")`.
- **`video_set_cursor()`** `video_sdl2.cpp:2392-2433`: **early-returns if `vncserver`**
  (`video_sdl2.cpp:2398`); otherwise frees/recreates the SDL cursor, `SDL_ShowCursor` per
  `cursorVisible`, warps mouse on grab.
- VNC cursor suppression also at init `video_sdl2.cpp:1366-1370` (`SDL_ShowCursor(SDL_DISABLE)`
  when vncserver).

**VOSF / blit:** dirty tracking + `update_display` in `video_sdl2.cpp` (`update_display` near
2328 in the X11 file; SDL2 equivalent drives `VNCServerUpdate` at `video_sdl2.cpp:1187`).
Format-convert+blit cost is host-side; all *drawing* is guest-CPU (except NQD, see §4).

**VNC server — `src/SDL/vnc_server.cpp` (libvncserver, `-lvncserver`):** handles framebuffer
push (`VNCServerUpdate`), keyboard (`vnc_keyboard_callback`), and **pointer position only**
(`vnc_pointer_x/y`, `vnc_push_pointer_motion` `vnc_server.cpp:167`). **No cursor-shape support**
— no `rfbSetCursor`, no Cursor/RichCursor pseudo-encoding anywhere in the 650-line file.

**Default run mode (`Makefile:71`):** `make run-jit` writes a prefs file with
`vncserver true`, `vncport 5999`, `screen win/640/480`, and **does not set `hardcursor`** (so
it stays default-false). `gfxaccel` defaults **true** (`prefs_items.cpp:96`
`PrefsAddBool("gfxaccel", true)` overrides the table default at `prefs_items.cpp:50`).
`hardcursor` defaults **false** (`prefs_items.cpp:64`).

---

## 2. Host cursor implementation plan (B5 — ready to code)

**Goal restated:** make the guest cursor render host-side (smooth, decoupled from VOSF
refresh) in the *actual* dev workflow, which is VNC. Native SDL-window already works.

### Two paths, very different effort

**Path A — native SDL window (no VNC): already done. One-line enable.**
- Set `hardcursor` to default true, or pass it in the prefs. Concretely either:
  - `src/prefs_items.cpp:64` flip the table default `false → true`, **and** add
    `PrefsAddBool("hardcursor", true)` in `AddPrefsDefaults()` (`prefs_items.cpp:~88`) to
    match how `gfxaccel` is forced on at line 96 (the table default alone is overridden); or
  - add `hardcursor true` to the prefs emitted at `Makefile:71`.
- Result: `video_can_change_cursor()` returns true → `video.cpp` reports HW-cursor capability
  → guest stops compositing the cursor into the framebuffer → `MagCursor`/`SDL_CreateCursor`
  renders it on the SDL window. **No new code.** Verify per §B5-verify below.

**Path B — VNC window (the `make run-jit` path): real work, ~60–100 lines in `vnc_server.cpp`.**
The existing host cursor is a deliberate no-op under VNC (`video_sdl2.cpp:2398`,
`1366-1370`) because SDL's window cursor is invisible to a VNC client and warping the Xvfb
pointer makes the client snap. To get a host cursor over VNC you must push the cursor *shape*
to the RFB client via libvncserver's cursor API. Plan:

1. **Add `VNCServerSetCursor(const uint8 *image, const uint8 *mask, int w, int h, int hotX,
   int hotY)` to `vnc_server.cpp` + decl in a `vnc_server.h`** (note: no `vnc_server.h` cursor
   decls exist today; the include is referenced at `video_sdl2.cpp:82`). Implementation: build
   an `rfbCursor` from the 1-bit `MacCursor` image+mask (libvncserver
   `rfbMakeXCursor`/`rfbSetCursor`), set `vnc_server->cursor` under `vnc_mutex`. libvncserver
   then advertises the Cursor pseudo-encoding (`rfbEncodingXCursor`/`rfbEncodingRichCursor`)
   and ships the shape to capable clients automatically.
2. **Stop suppressing cursor under VNC, route to VNC instead.** In `video_set_cursor()`
   (`video_sdl2.cpp:2392`): replace the `if (PrefsFindBool("vncserver")) return;` early-out
   with a branch that, when vncserver is on, calls `VNCServerSetCursor(&MacCursor[4],
   &MacCursor[36], 16, 16, MacCursor[2], MacCursor[3])` and **does not** touch the SDL cursor
   or warp the mouse.
3. **Visibility:** on `cscDrawHardwareCursor` visibility changes, set/clear the rfb cursor (a
   hidden cursor = empty/zero-size `rfbCursor`, or track and re-send on show). The guest still
   sends absolute positions via `cscDrawHardwareCursor`, but for VNC the *position* is owned by
   the client's own pointer — you push only the **shape**, so the local client cursor renders at
   the client's pointer. (This is the correct RFB model and avoids the snap problem entirely.)
4. **Capability gating:** `video_can_change_cursor()` (`video_sdl2.cpp:2381`) should return true
   when `hardcursor` is set **regardless** of vncserver once Path B lands. Until then, keep it
   pref-gated so VNC users opt in.

Functions to modify (current file:line):
- `src/SDL/video_sdl2.cpp:2381` `video_can_change_cursor` — allow VNC.
- `src/SDL/video_sdl2.cpp:2392-2433` `video_set_cursor` — VNC branch → `VNCServerSetCursor`.
- `src/SDL/video_sdl2.cpp:1366-1370` init — don't unconditionally disable; push initial shape.
- `src/SDL/vnc_server.cpp` (+ new `vnc_server.h` decls) — new `VNCServerSetCursor`.
- `src/prefs_items.cpp:64/88` and/or `Makefile:71` — enable `hardcursor`.
- **No change to `src/video.cpp`** — the guest protocol is complete and correct as-is.

### Extension: color cursors (`SDL_CreateColorCursor`) — separate, optional
`cscSetHardwareCursor` rejects non-1-bit cursors (`video.cpp:487-489`, `rowBytes!=2 →
controlErr`), and `MagCursor` uses 1-bit `SDL_CreateCursor`. Color hardware cursors (Mac OS 8/9
*do* use them, e.g. tinted/anti-aliased pointers) therefore fall back to the slow
software/framebuffer path. To support them: relax the guest gate to accept the indexed/direct
pixmap formats, expand `MacCursor` (or a parallel buffer) to carry ARGB, and add an
`SDL_CreateColorCursor` path in `MagCursor`. Recommend deferring — B&W covers the arrow and
most system cursors and is the QEMU/MOL parity baseline.

### B5 verification
- `make run` (interpreter) or `make run-jit` with `hardcursor` enabled; move the mouse while
  the guest is busy (e.g. Finder copy). Pointer must stay smooth and decoupled from VOSF refresh.
- **No double cursor:** the guest must hide its software cursor. It does this automatically once
  `cursorHardware` is reported true (the standard Display Manager behavior). Confirm visually.
- Path B: connect a VNC client (`vncdotool`/Screen Sharing); confirm the Mac arrow shape is the
  client cursor and no center-snap occurs on cursor change.

---

## 3. MOL + QEMU protocol findings (primary-source where fetched)

### QEMU `qemu_vga.ndrv` (QemuMacDrivers, GPL-2.0) — VERIFIED, and it *corrects* landscape-3
Read directly: `QemuVGADriver/src/DriverQDCalls.c`, `QemuVga.h`, `QemuVga.c`.
- **Hardware cursor: NOT implemented.** `GraphicsCoreSupportsHardwareCursor`
  (`DriverQDCalls.c:206`) sets `csSupportsHardwareCursor = false`;
  `GraphicsCoreSetHardwareCursor`/`DrawHardwareCursor` (`:221/:229`) are stubs. landscape-3's
  claim that qemu_vga.ndrv has a working HW cursor is **inaccurate for the upstream `.ndrv`**
  (the cursor work lives in QEMU's *device/UI* side for some chips, not this generic VGA ndrv).
  **Implication: SheepShaver's `video.cpp` is already *ahead* of qemu_vga.ndrv on cursor.**
- **Host transport = Bochs/QEMU VBE dispi I/O ports**, not a custom command buffer:
  `VBE_DISPI_IOPORT_INDEX 0x01CE` / `..._DATA 0x01CF` with indexed registers
  (`INDEX_XRES/YRES/BPP/ENABLE/VIRT_WIDTH/X_OFFSET…`, `QemuVga.h:19-46`). The ndrv does
  mode-set + linear-framebuffer + EDID (`QemuEdid.c`) only.
- **2D/3D acceleration: NOT implemented** (confirmed — no blit/fill register path; matches the
  qemu-devel "needs updating to pass 2D/3D to virtio-gpu" note in landscape-3).
- **Buildability (Part 2.5):** the project ships `QemuVGADriver/QemuVGADriver.mcp.xml` — a
  **Metrowerks CodeWarrior** project (`.mcp`). It is *not* buildable with a modern toolchain
  off the shelf; you'd need legacy CodeWarrior or the Retro68 cross-toolchain to produce the
  `.ndrv`. **SheepShaver needs none of this** — its driver is the in-process `video.cpp`
  (EMUL_OP based), so there is no guest `.ndrv` binary to compile (see §6).
- **Takeaway:** QEMU is a structural reference for the *guest ndrv shape* (which SheepShaver
  already embodies) and for mode-set-over-MMIO. It is **not** a reference for cursor or accel —
  SheepShaver's own `video.cpp`+`gfxaccel.cpp` are the stronger references.

### Mac-on-Linux (MOL) — design reference only (source NOT retrievable via available tools)
> **Tool limitation, documented:** MOL lives on SourceForge CVS, not GitHub. Attempts to locate
> a fetchable mirror (`gh search repos mac-on-linux`; trees for `joevt/`, `smemsh/`,
> `mac-on-linux/mac-on-linux`) all returned nothing usable. The OSI/video-driver details below
> are therefore **carried from landscape-3's prior research, not re-verified against source.**
> If MOL primitives become load-bearing, retrieve via `cvs -d:pserver:anonymous@…sourceforge`.

- Architecture: guest-side `.ndrv` ↔ host MoL kernel module over **OSI (Operating System
  Interface) hypercalls** — a `sc`/illegal-instruction trap carrying a selector + args, the
  PPC-Linux analogue of SheepShaver's EMUL_OP traps.
- Accelerated primitives MOL offloaded (per landscape-3 §5 and MOL's `video`/osi driver): the
  same QuickDraw-bottleneck class SheepShaver's NQD already covers — **rectangle fill, blit/
  copy, and scroll** — executed natively in the host module. MOL is the *historical proof* that
  driver-level (not silicon) accel works on Mac OS 9.
- Host side is PPC/Linux-kernel-bound → **design reference, not portable code**. The protocol
  *shape* (selector + param-block in guest memory, host executes, returns) is exactly
  SheepShaver's `NQDMisc` hook model (§4) — so SheepShaver already has MOL's design, in
  userspace, GPL-2.0-clean.

---

## 4. Accel protocol design for SheepShaver (C3)

### The protocol already exists: `gfxaccel.cpp` + `NQDMisc`
`src/gfxaccel.cpp` (461 lines, GPL-2.0+) is SheepShaver's paravirtual 2D accelerator. It is the
C3 mechanism, already shipping and **on by default** (`gfxaccel=true`). How it works:

- **Registration:** `VideoInstallAccel()` (`gfxaccel.cpp:426-460`) calls
  `NQDMisc(6, info)` once per QuickDraw accel op-class. Each `info` block carries
  `{DrawProc-installer-hook TVECT, sync-hook TVECT, op-class}`. `NQDMisc` is the Mac OS
  Native QuickDraw "register acceleration hook" entry (`video.cpp:99-104` thunk). This is the
  Mac OS QuickDraw **bottleneck-acceleration registration** mechanism — the OS-blessed way for a
  display driver to intercept drawing.
- **Per-draw negotiation:** when QuickDraw is about to draw, it calls the registered
  *hook* (`NQD_fillrect_hook` `gfxaccel.cpp:294`, `NQD_bitblt_hook` `:384`). The hook inspects
  the operation's param block (transfer mode, pixel depth, rects) and, if accelerable, writes a
  native `DrawProc` TVECT into `acclDrawProc` and returns true. QuickDraw then calls that native
  proc instead of doing the draw in the emulated CPU.
- **Native execution (the host-side draw):** `NQD_fillrect`/`NQD_invrect` (`gfxaccel.cpp:253`),
  `NQD_bitblt` (`gfxaccel.cpp:321`) run as native C against `Mac2HostAddr(...)` framebuffer
  pointers — `memmove` per row for blit, `do_fillrect<bpp>` for fill. `NQD_set_dirty_area`
  marks VOSF dirty so the host blit/VNC picks it up.

### What is accelerated today (this is the literal ceiling of current coverage)
- **Fill:** `acclTransferMode == 8`, dest depth ≥ 8bpp → `NATIVE_NQD_FILLRECT` (`gfxaccel.cpp:302-306`).
- **Invert:** `acclTransferMode == 10` → `NATIVE_NQD_INVRECT` (`:307-311`).
- **Blit:** `acclTransferMode == 0` (srcCopy only), src depth == dest depth ≥ 8bpp, same
  row-bytes sign, plus the masks at `0x018/0x128/0x130/0x15c` (`gfxaccel.cpp:390-401`) →
  `NATIVE_NQD_BITBLT`.
- **Everything else** is registered via `NQD_unknown_hook` (`gfxaccel.cpp:406`) which only marks
  dirty and **returns false** → QuickDraw draws it in the emulated CPU. The transfer-mode table
  at `gfxaccel.cpp:363-382` lists the 18 modes that exist; only 3 are offloaded.

### C3 = widen NQD coverage (concrete, ordered, low-risk)
This is the real C3 work — extend `gfxaccel.cpp`, no new protocol, no guest `.ndrv` to build:
1. **More blit transfer modes:** srcOr(1)/srcXor(2)/srcBic(3) and the `not*` variants
   (`gfxaccel.cpp:364-372`) — add raster-op inner loops alongside the `memmove` path. These are
   common (text/icon masking) and currently fall to the interpreter.
2. **Scroll:** ScrollRect is the highest-value missing primitive (window/list scrolling).
   QuickDraw routes scroll through CopyBits with overlapping src/dest in the *same* pixmap —
   `NQD_bitblt` already uses `memmove` and handles overlap, but the hook's accept predicate
   (`gfxaccel.cpp:390`) and Mac OS's routing may need confirming for self-blit. Verify whether
   `csc*`/`NQDMisc` exposes a distinct scroll op-class beyond the 8 in the
   `VideoInstallAccel` loop (`gfxaccel.cpp:447`).
3. **Color-key / pattern fill** (transfer mode 36 transparent, hilite 50) — niche, defer.
4. **Depth coverage:** 1/2/4-bpp paths are punted (`>= 8` gates). Low value for modern use; defer.

Each item: keep harness/boot green, gate behind `gfxaccel` (already default on). No JIT
interaction — these are native C handlers invoked via TVECT, orthogonal to the PPC JIT.

---

## 5. Honest assessment of the QuickDraw-routing ceiling

**The ceiling is fixed by what Mac OS QuickDraw chooses to route through the driver's accel
hooks — and it is genuinely limited.** Evidence is in our own code, not speculation:

- Mac OS only calls the registered DrawProc hooks for operations it has decided to route
  through the **acceleration-hook (bottleneck) path**. The `NQDMisc(6,…)` mechanism covers the
  StdBits/CopyBits + fill/invert bottleneck class — that is why `gfxaccel.cpp` only ever sees
  fill, invert, and bitblt op-classes (the 8-entry loop at `gfxaccel.cpp:447`). Anything
  QuickDraw renders **directly into the framebuffer** without consulting a bottleneck proc — and
  that is *most* drawing: line/poly/region/arc rasterization, text glyph rendering, patterns,
  CopyMask, anything in odd modes/depths — never reaches the driver at all. Those run in the
  emulated PPC and the host only sees the resulting dirty pixels via VOSF.
- So driver-level accel can only ever offload the subset QuickDraw bottlenecks out: bulk fills,
  bulk srcCopy blits, scrolls, invert. That subset *is* a meaningful chunk of UI cost (window
  fills, scrolling, icon/region blits) — MOL proved it's worth doing — but it is a **ceiling, not
  a path to full GPU-class 2D accel.** Text and vector drawing stay on the CPU.
- The only ways past that ceiling are the two paths landscape-3 already rejected: emulate real
  silicon so Mac OS's native accelerated `.ndrv` takes over more ops (approach b — GPL-3.0,
  unproven, discards our stack), or reimplement QuickDraw natively (Executor — out of scope).
- **Bottom line:** widening NQD (C3) has a real but bounded payoff. It will not make the guest
  "GPU accelerated"; it offloads the bulk-pixel bottlenecks Mac OS already hands us, and nothing
  more. That is exactly what MOL and (the device side of) QEMU settled for.

---

## 6. Recommendation

1. **B5 now, in two commits.** (a) Enable `hardcursor` by default
   (`prefs_items.cpp` + force-on like `gfxaccel`) — zero new code, immediate win on native SDL
   windows. (b) Add `VNCServerSetCursor` (libvncserver `rfbSetCursor`/Cursor pseudo-encoding) so
   the *actual `make run-jit` VNC workflow* gets the smooth host cursor. Defer color cursors.
2. **C3 = extend `gfxaccel.cpp`, do not invent a protocol.** The `NQDMisc` paravirtual accel
   layer is the C3 mechanism and it already ships on by default. Add blit raster-op modes
   (Or/Xor/Bic) and validate/extend ScrollRect first; these have the best UI payoff inside the
   fixed ceiling.
3. **References, corrected priority:** SheepShaver's own `video.cpp` + `gfxaccel.cpp` are the
   primary spec (they already do more than qemu_vga.ndrv). QemuMacDrivers = guest-ndrv shape +
   mode-set-over-MMIO reference (but its cursor/accel are stubs — verified). MOL = design proof
   for fill/blit/scroll offload (GPL, PPC-host-bound, not portable). DingusPPC = register docs
   only, GPL-3.0, do not copy.
4. **Sequencing per backlog:** B5(a) → B5(b) VNC cursor → VOSF dirty-rect tightening → C3 NQD
   widening. All independent of the JIT work in flight.
