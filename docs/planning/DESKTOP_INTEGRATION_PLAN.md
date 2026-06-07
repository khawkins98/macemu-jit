# SiliconSheep — Desktop Integration Feature Plan

> **Status:** 🟡 Active — Tier 1 largely complete · **Created:** 2026-06-02 · **Updated:** 2026-06-06
> **Why this doc exists:** "SiliconSheep" — a Tauri v2 launcher/VM manager for SheepShaver (first-run wizard, VM library, hot-reload, coherence-lite). Framework pivoted from Cocoa to Tauri (2026-06-05). Scaffold at `SiliconSheep/`.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._


> *Do they dream of OS X?*

A modern Cocoa launcher and desktop integration layer for SheepShaver, targeting Apple Silicon + macOS. Research synthesis from lateral-thinking agent review, June 2026.

---

## Repository Strategy

### Hard fork, not a submodule

SiliconSheep will be a **hard fork of kanjitalk755/macemu** (the most actively maintained upstream), not a submodule or thin wrapper. Reasons:

- We need to make structural changes to the emulator core (extfs multi-volume, prefs hot-reload, ARM64 JIT) that upstream is unlikely to accept wholesale.
- A clean, pruned repo is easier to reason about and faster to build.
- Cherry-picking individual upstream commits is less friction than managing a submodule with diverging history.
- The existing `cebix/macemu` upstream is effectively unmaintained; kanjitalk755 is the real upstream to track.

**Upstream tracking:** Two named remotes, cherry-picks only — no merges:

| Remote name | Repo | What to track |
|---|---|---|
| `upstream-kanjitalk755` | `kanjitalk755/macemu` | macOS bug fixes, SDL2 improvements, general stability |
| `upstream-rcarmo-jit` | `rcarmo/macemu-jit` (branch `master`) | ARM64 JIT work targeting Apple Silicon |

Periodically `git log <remote>/master` on each and cherry-pick relevant commits. Keeping them as separate remotes makes it easy to attribute and audit what came from where.

### What gets removed in the initial fork cleanup

The repo currently carries a lot of weight from platforms and tooling we will never target. Proposed purge list:

| Path / Component | Reason to remove |
|---|---|
| `BasiliskII/src/BeOS/` | BeOS platform target; dead platform |
| `BasiliskII/src/AmigaOS/` | AmigaOS target; dead platform |
| `BasiliskII/src/Windows/` | Windows target; out of scope |
| `BasiliskII/src/SDL/` old SDL1 paths | SDL1 is superseded by SDL2; keeping SDL2 only |
| `SheepShaver/src/BeOS/` | Same as above |
| `SheepShaver/src/Windows/` | Same as above |
| `SheepShaver/src/Unix/prefs_editor_gtk.cpp` | GTK prefs editor; replaced by SiliconSheep launcher |
| `SheepShaver/src/MacOSX/SheepShaver.xcodeproj` | Replaced by programmatic Cocoa build |
| `SheepShaver/src/MacOSX/SheepShaver_Xcode8.xcodeproj` | Same |
| `SheepShaver/src/MacOSX/Launcher/SheepShaverLauncher.xcodeproj` | Replaced by SiliconSheep |
| Old 32-bit / i386 JIT backends | Targeting ARM64 + x86_64 only |

**What stays:**

- `BasiliskII/` core — shares `extfs.cpp`, `clip_macosx64.mm`, and other components with SheepShaver. Keep it; it also fills the 68k gap (System 6/7) that SheepShaver can't.
- `cxmon/` — the built-in debugger; useful for development.
- All `src/Unix/` and `src/MacOSX/` code not covered above.
- SDL2 video/audio backends.

### SiliconSheep launcher location

Lives at `SiliconSheep/` in the repo root — a sibling to `BasiliskII/` and `SheepShaver/`. It references the emulator binaries as auxiliary executables in the app bundle, built separately via the Autotools path. One repo, one `make` invocation builds everything.

---

## TL;DR

No modern GUI frontend for SheepShaver exists. The field is wide open. The highest-leverage work is a **guided first-run wizard + VM profile manager**, which addresses the #1 user pain point (7+ manual setup steps) without touching the emulator core. Deeper desktop integration (clipboard improvements, multi-folder sharing, coherence-lite) is feasible in phases. True seamless/coherence windowing is technically infeasible for classic Mac OS without novel research.

---

## Current State Assessment

### What Already Works (and is underappreciated)

- **HiDPI/Retina**: Already fixed via `SDL_WINDOW_ALLOW_HIGHDPI` in `video_sdl2.cpp:700` + `NSHighResolutionCapable` in plist. Sharp rendering on Apple Silicon Retina displays works today.
- **Clipboard sync**: `clip_macosx64.mm` is a sophisticated bidirectional implementation — handles TEXT/styl (Mac Roman + style runs), PICT (with RGBA conversion), RTF, and UTI-mapped arbitrary flavors. Polling-free; triggered lazily via `NSPasteboard changeCount`. Main gap: `utxt`/UT16 Unicode scrap types are silently skipped.
- **Unix folder sharing**: `extfs.cpp` (2285 lines) implements a complete HFS File System Manager in software, translating all Mac File Manager calls to POSIX. Works but limited to one shared directory; not live-reloadable.

### Actual User Pain Points (ranked)

1. **ROM acquisition** — requires legally-grey file with no in-app guidance; #1 first-run blocker.
2. **Setup complexity** — 7+ manual steps, multiple separate downloads, no wizard.
3. **Preferences don't hot-apply** — every change requires full quit + relaunch (2–3 min cycle).
4. **Gatekeeper/permissions crashes** — launch failures that require copy-to-Desktop workaround; documented, unsolved.
5. **No JIT on Apple Silicon ARM64** — native ARM64 build is slower than x86_64 + Rosetta 2 + JIT.
6. **No VM library management** — each installation is a standalone folder; no multi-config UI.
7. **Fullscreen traps users** — no visible escape path without knowing Control-Return shortcut.

### Existing Frontends and Wrappers

Everything here is either dead, Windows-only, or a static bundle — not an interactive macOS GUI app.

| Project | Type | Status | Notes |
|---|---|---|---|
| **SheepShaver Wrapper** (mendelson.org) | Static bundle | Last updated Feb 2024 | Pre-configured app bundle; bundles a 4GB virtual disk, auto-mounts Documents. You drop in a ROM and it launches. No settings UI, no VM management. Requires macOS 10.13+. |
| **Medusa** (GitHub: sentient06) | Cocoa preferences manager | Last updated Oct 2014, never left beta | 32-bit only — incompatible with Big Sur and later. The closest thing to a real GUI manager that ever existed. |
| **Virtual-Mac** (GitHub) | VB.NET GUI wrapper | Abandoned 2021 | Windows-only. 4 commits, 7 stars. Readme requests a macOS contributor it never got. |
| **kanjitalk755/macemu** built-in UI | 3-tab Cocoa prefs dialog | Actively maintained | The current best option, but it's the same dialog that has always been there. No VM library, no wizard, no dark mode. |

GitHub topic search for `sheepshaver` returns exactly 2 repositories, neither of which is a modern frontend. **The field is genuinely open.**

### What Doesn't Exist Yet

- A modern macOS GUI frontend.
- Multi-folder shared volumes.
- Live preference application without full restart.
- Any form of seamless/coherence windowing.
- VM profile library management.

---

## Framework Decision

### Decision: Tauri v2 (revised 2026-06-05)

**Architecture:** A Tauri v2 app with a Rust backend and web frontend (HTML/CSS/JS). SheepShaver
runs as a Tauri **sidecar** child process — the same process model the existing Cocoa launcher
already uses (`NSTask`). The launcher manages VM profiles, preferences, and lifecycle; the
emulator owns its own SDL window for the guest display. IPC between the launcher and a running
emulator uses a Unix domain socket (for hot-reload, status, shutdown).

**Why Tauri over the existing Cocoa launcher (the previous decision):**

1. **Cross-platform door stays open.** The user wants to keep Linux/Windows as a future option,
   not build for them now. Tauri lets the UI port naturally; Cocoa/ObjC locks to macOS.
2. **No Xcode.app required.** The dev environment has only Command Line Tools. The Cocoa launcher
   depends on NIBs (`VMListWindow.nib`, `VMSettingsWindow.nib`) that require Xcode.app or
   pre-compiled artifacts. Tauri builds with `cargo` + `npm` — CLI-only.
3. **Modern UI for free.** Dark mode, responsive layout, accessibility, animations — CSS handles
   what would be manual `NSAppearance` / Auto Layout work in Cocoa.
4. **The "two-window" concern is moot.** The existing Cocoa launcher *already* spawns SheepShaver
   as a separate process with a separate window. Tauri does the same thing. The framebuffer-
   sharing concern only applies if embedding the guest display inside the launcher, which is not
   a Tier 1 or Tier 2 goal.

**What the Cocoa launcher did well (preserve in Tauri):**
- `.sheepvm` bundle directories (prefs + disk images together)
- VM list in `NSUserDefaults` → migrate to a JSON manifest
- Settings editor that reads/writes the plain-text prefs format
- Child process lifecycle management with termination notification

| Option | Verdict |
|---|---|
| **Tauri v2** | **Chosen.** CLI-buildable (Rust + Node, no Xcode.app). Cross-platform door open. ~5 MB overhead. Web UI = modern UX with minimal effort. Sidecar model matches the existing launcher's architecture. |
| **Extend existing Cocoa/ObjC launcher** | Previously chosen, reversed. NIB dependency requires Xcode.app or ground-up programmatic rewrite. Locks to macOS. The existing ~1100 LOC is heavily deprecated (pre-10.5 APIs). |
| **SDL3 + Dear ImGui** | Best for an in-process developer overlay (RetroArch style). No separate launcher window. Weakest for first-run wizard UX aimed at non-technical users. |
| **Qt6** | VirtualBox precedent; solid cross-platform. 60–80 MB binary, Qt + SDL event loop bridging non-trivial. Only wins if Linux/Windows parity is a first-class goal from day one. |
| **Electron** | No advantage over Tauri; 150–200 MB overhead. Skip. |
| **SwiftUI** | Xcode-dependent in practice; locks to Apple platforms. Skip. |

---

## Feature Roadmap

### Tier 1 — First-Run & VM Management (launcher-only, no emulator changes)

Pure launcher/wrapper work. SheepShaver binary is a sidecar child process; Tauri manages it.
**Happy path: 3 clicks + 1 file drop (the ROM).** ROM is the only irreducible user-supplied input.

#### First-Run Wizard (screen by screen)

- [x] **Screen 1 — Welcome.** "SiliconSheep — Classic Mac OS on Apple Silicon." Single button.
- [x] **Screen 2 — ROM.** Browse + drag-drop. SHA-256 check: green verified / amber accepted /
  "proceed anyway" for unrecognized. Accepts compressed/trimmed ROMs (512K–8MB).
- [x] **Screen 3 — Disk.** Create new (size picker) or use existing. Optional CD image.
- [x] **Screen 4 — Review & Boot.** Summary card, editable inline, "Create & Start."

#### VM Library

- [x] **Card grid**, responsive. Per card: screenshot (live VNC every ~10s + final on shutdown),
  OS version (from `[SYSV]` Gestalt hook), RAM, last-booted date. Status pill.
- [x] **Actions per card:** Start/Stop, Settings, Duplicate (APFS clonefile), Reveal in Finder,
  Delete.
- [x] **Top-level "+" button** + **"Import Prefs"** button.
- [x] **Drag-and-drop:** ROM, disk, CD, prefs files → routed to the right context.
- [ ] **Search/filter bar** appears once library exceeds 6 VMs.

#### Settings Panel

- [x] **7 sidebar sections:** General, Display (custom resolution + presets, frameskip, QD accel),
  Storage (disks, CDs, nocdrom, extfs), Network (slirp, VNC server/port), Input (mouse wheel,
  swap opt/cmd, keycodes), Advanced (sound, JIT cache, boot driver, expert fold-out with all
  remaining prefs + explainer text), Debug (env vars, log viewer).
- [x] **Hot-reload indicators per setting.**

#### Other Tier 1 Items

- [ ] **Auto-restart on pref change**: Write prefs, SIGTERM child, relaunch. Eliminates the
  manual quit/relaunch cycle for non-hot-reloadable settings.
- [x] **Fullscreen escape overlay**: 3-second fade overlay on VM launch showing mouse-release
  (Ctrl-F5) and fullscreen-exit (Esc) shortcuts.
- [x] **Dark mode**: CSS `prefers-color-scheme`.
- [x] **Gatekeeper mitigation**: startup quarantine check + amber warning banner with one-click
  "Clear Quarantine" button (`xattr -cr`). Banner disappears after clearing.
- [x] **Disk backup ("snapshots")**: Available only when VM is stopped. Copies `.dsk` with
  timestamp suffix. Restore = swap file back. Labelled honestly as "Disk Backup", not
  "snapshot" (no saved CPU/RAM state). Uses APFS `clonefile` when possible (instant).
- [x] **Screenshot capture**: Live VNC screenshots every ~10s + final frame on shutdown. Stored
  in `.sheepvm/screenshot.png`. BGR→RGB channel fix via `vnc_capture.py`.
- [x] **Drag-and-drop file import**: ROM, disk, CD, prefs files → routed to wizard step or import.
- [ ] **CRT/scanline shaders + integer scaling** (stretch): SDL render pipeline or Metal
  post-process pass.
- [x] **Coach marks**: Mouse capture toast on first launch ("Click inside the classic desktop to
  capture the mouse. Press Ctrl-F5 to release.").

#### Error States

- [x] ROM not found / emulator not found: error banner with guidance.
- [ ] Disk missing/corrupt: card warning badge → "Locate" / "Remove from VM".
- [ ] Emulator crashed: card flips to "Crashed" (red pill), shows stderr tail.
- [x] Mouse capture toast on first launch.

### Tier 2 — Enhanced Desktop Integration (emulator IPC/hooks needed)

Requires C++ changes to the emulator alongside Tauri frontend work.

#### C2.0 Bidirectional UDS RPC — the foundation for all Tier 2 work ← 🔜 next

**Status:** designed, not started. **Effort:** ~150 LOC emulator + ~100 LOC Tauri. **Risk:** low.

**The problem:** We currently have three ad-hoc IPC mechanisms (env vars at launch, prefs file
at startup, file-polling every ~5s for runtime control). Each is one-directional, high-latency,
and doesn't scale. The Inspector panels, hot-reload, and runtime toggles all need sub-second
bidirectional communication between SiliconSheep and the running emulator.

**The solution:** Flip the emulator from RPC *client* to RPC *server* on its existing
`rpc_unix.cpp` UDS framework. This is the same pattern QEMU (QMP) and VirtualBox (COM/XPCOM)
converged on independently.

**What exists today (`rpc_unix.cpp`):**
- Full UDS RPC with typed messages (int32, bool, string, byte arrays), method dispatch tables
- 3 methods: ERROR_ALERT, WARNING_ALERT, EXIT — emulator → GUI only
- Activated via `--gui-connection <path>` — emulator is the client
- Wire format: network-byte-order int32 framing, method IDs, typed args, ACK/REPLY

**What to build:**
1. Emulator becomes the server — `rpc_init_server()` on `/tmp/sheepshaver-<pid>.sock`
   (path written to `.sheepvm/rpc_socket` for discovery)
2. Non-blocking poll in `do_video_refresh()` — `rpc_wait_dispatch(conn, 0)` once per frame
   at 60 Hz. Sub-16ms command latency, zero extra threads.
3. New method IDs in `rpc.h`:
   - `RPC_METHOD_SET_PREF` (key, value) — runtime pref change
   - `RPC_METHOD_INPUT_LOCKOUT` (bool) — instant toggle
   - `RPC_METHOD_FRAMESKIP` (int32) — instant change
   - `RPC_METHOD_MOUSE_GRAB` (bool) — instant toggle
   - `RPC_METHOD_GET_STATE` → reply with heartbeat stats
   - `RPC_METHOD_READ_MEMORY` (addr, len) → reply with bytes (Tier 2 Inspector)
   - `RPC_METHOD_DUMP_REGISTERS` → reply with GPR/SPR/CR/PC (Tier 2 Inspector)
   - `RPC_METHOD_INSERT_DISK` (path) — hot disk insertion
4. SiliconSheep Rust backend connects as UDS client via `std::os::unix::net::UnixStream`,
   sends binary RPC frames matching the existing wire format.

**Replaces:** env var launch mechanism (still useful for agents/CLI), runtime_control file
polling (retire once C2.0 lands), the stale `--gui-connection` GTK path.

**Unblocks:** all Tier 2 items below, Inspector Tier 2 panels (registers, memory), hot-reload,
hot disk insertion, Tier 4 automation layers.

**Sequence:** C2.0 first → then all other Tier 2 items ride on it.

---

Key enabler: the existing `rpc_unix.cpp` UDS framework (extended via C2.0 above). Extend
this with new methods rather than inventing a new protocol.

- [ ] **True hot-reload** for frameskip/mouse/display: Add `SIGHUP` handler +
  `PrefsReloadFromDisk()` (~200 LOC in `prefs.cpp`), or extend the UDS RPC with
  `RPC_METHOD_RELOAD_PREFS`.
- [ ] **Multi-folder shared volumes**: Extend `extfs.cpp` beyond its single `RootPath` to
  support N configured volumes. Use the multi-value pref pattern (multiple `extfs` lines, like
  `disk`). ~2–3 days. See Implementation Notes §extfs.
- [ ] **Live folder mount/unmount**: Hook `ExtFSInit()`/`ExtFSExit()` for hot-add/remove via
  UDS RPC.
- [ ] **Hot disk/CD insertion + software library** *(Infinite-Mac-inspired, 2026-06-06)*: "insert a
  disk into the running instance" and "pick software from a library." **The mount machinery already
  exists** — `disk.cpp` has `DiskMountVolume`/`to_be_mounted` and `mount_mountable_volumes()` run from
  `accRun` (the periodic mount path classic Mac OS uses for media insertion). So a UDS RPC
  (`RPC_METHOD_INSERT_DISK <path>`) that adds an image to the mount list and flags `to_be_mounted`
  should surface a new volume/CD in the *running* guest without restart — verify against the CD
  media-change path. Pair with a **content-addressed image library** (curated `.dsk`/`.iso` set,
  deduped via APFS `clonefile` so duplicates cost nothing) for a "software shelf" UI. For loose files,
  reuse the ExtFS "drop folder" path above with Infinite Mac's **Downloads / Uploads / Saved** watched-
  folder convention. *Transfers from Infinite Mac:* the runtime-injection model (it uses the **same
  `extfs.cpp`** we have) + the watched-folder convention + content-addressed dedup. *Does **not**
  transfer:* its browser plumbing (256 K HTTP-range chunks, service worker, IndexedDB, Emscripten FS) —
  native macOS uses real files and APFS, so we skip the streaming layer entirely. Optional: a
  `machfs`-based tool (as Infinite Mac uses) to build library images + preserve Finder metadata.
  **Detail/source:** persistent.info Infinite Mac write-up; `INFINITE-MAC-EVALUATION-PLAN.md`.
- [ ] **`utxt`/UT16 clipboard fix**: `clip_macosx64.mm` explicitly skips Unicode scrap types.
  Implement the UTF-16 ↔ Mac Roman converter. Mac OS 8.5+ supports `utxt` natively. Low-medium
  effort.
- [ ] **Emulator lifecycle control via IPC**: Pause/resume/quit/config-reload commands from
  launcher to emulator. Parse `CommandEvent::Stderr` for `[HB ...]` diagnostic heartbeats for
  status without any emulator change.
- [ ] **Disk-image snapshots** (beyond file copy): APFS clonefile overlay chains or qcow2-style
  layering. Needs launcher orchestration to pause emulator, snapshot, resume.
- [ ] **Network configuration assistant**: Guided UI for slirp vs. VDE, port forwarding,
  AppleTalk. Currently requires manual pref editing.
- [ ] **Display scaling controls**: Live slider in the Tauri sidebar; write pref + signal
  SheepShaver to re-scale without full restart.
- [ ] **Guest-initiated file import**: Host writes file to ExtFS shared dir, then injects a
  Finder `odoc` AppleEvent via `Execute68kTrap(AESend)`. Medium complexity.
- [ ] **Direct framebuffer screenshot** (non-VNC): Read `ScrnBase` (0x824) + `ScreenRow`
  (0x106) + depth from `GDevice` list (0xCC8) — raw framebuffer blit. Faster than VNC.
- [ ] **Clipboard enhancements** (emulator-side C++ changes to `clip_macosx64.mm`):
  - **Directional control**: new prefs `clipboardDirection` (both/host-to-guest/guest-to-host/off)
    gating `GetScrap`/`PutScrap` independently. Currently macOS clipboard sync is always
    bidirectional with no off switch (~30 lines in `clip_macosx64.mm`).
  - **Unicode support** (`utxt`/`UT16`): the clipboard handler explicitly skips Unicode scrap
    types with a "sometime, it might be interesting" comment. Implement UTF-16 ↔ Mac Roman
    converter for Carbon apps (AppleWorks, BBEdit). Mac OS 8.5+ supports `utxt` natively.
  - **Clipboard status feedback**: expose last-sync direction and content type to the launcher
    via stderr signal or UDS RPC for a live "Last synced: text / image" indicator.
- [ ] **Disk image resize**: extend an existing raw `.dsk` image (append zeros via `truncate`
  or `dd`). The HFS partition inside doesn't auto-expand — the guest must re-initialize or
  use a disk utility. The UI should warn: "The disk file will grow but the Mac OS partition
  inside must be reformatted to use the new space." Safe for blank/fresh disks; risky for
  disks with existing data (backup first). Implementation: Rust `File::set_len()` for the
  raw extend, SiliconSheep Storage tab "Resize" button next to each disk.

### Tier 3 — Coherence Lite (significant research, novel work)

No prior art exists for classic Mac OS. The key insight making this tractable: SheepShaver has
**full read access to guest RAM** via `Mac2HostAddr()`, and the Window Manager's structures are
at well-known addresses. No guest agent needed for *reading* — only for *modifying* guest
behaviour.

- [ ] **Guest window list polling**: Read `WindowList` (0x9D6) from a host thread on a timer.
  Walk the chain: `+0x18` (nextWindow) for Z-order, `+0x08` (titleHandle) for titles, `+0x04`
  (port.portRect) for bounds, `+0x6C` (windowKind: 2=dialog, 8+=DA). Already proven in the
  `e2e_emit_boot_ready_once()` code. Pipe to Tauri frontend as a live window list.
- [ ] **Frontmost app tracking**: Poll `CurApName` (0x910, Str31) periodically, or patch
  `_Launch`/`_ExitToShell` A-traps to emit an EmulOp. Display in Tauri titlebar.
- [ ] **Host title bar overlay**: Draw styled title-bar panels on the host desktop mirroring
  guest window positions. Adds a floating chrome layer above the SDL window.
- [ ] **Per-window Dock entries**: Use extracted window titles + bounds to show individual
  classic Mac OS windows in a custom HUD panel. Approximates Parallels-style task switching.
- [ ] **True coherence / seamless windows** *(research project)*: Decompose the guest
  framebuffer into per-window regions using guest WindowList bounds, composite each into a
  separate host `NSWindow`. Unprecedented for classic Mac OS. Parallels/VMware do this via
  guest agents + custom display drivers — neither exists for Mac OS 9.
- [ ] **Suspend / Resume (execution-state save/restore)**: Parallels-style suspend — serialize
  the full VM state to disk and restore it later, resuming exactly where you left off. Prior art:
  DOSBox-X has 100-slot save states; Parallels/VMware serialize the full hardware abstraction.
  **Effort: 2–3 weeks for a working prototype. High complexity.**

  What needs serializing:
  - **CPU registers** (`powerpc_registers` struct) — easy, one struct
  - **Guest RAM** (up to 512 MB) — easy, contiguous mmap dump to disk
  - **JIT state** — flush code cache + block address cache; rebuild on resume (not serialized)
  - **Device state** — every subsystem (disk, audio, video, network, timers, ADB, interrupt
    flags, spcflags) needs a `Serialize()`/`Deserialize()` pair; none exist today
  - **Host OS resources** — file descriptors (disk images, sockets, VNC) can't be serialized;
    must reopen on resume and reconnect to the restored device state

  The hard part is device state — SheepShaver's subsystems don't have serialization interfaces.
  Each one (disk.cpp, audio_sdl.cpp, video_sdl3.cpp, ether_unix.cpp, timer.cpp, adb.cpp) would
  need explicit save/load functions.

  **Cheaper 80% alternative (already available):** disk-image snapshots (APFS `clonefile`, Tier 1)
  + fast boot (~10s with JIT). "Resume from snapshot" = boot fresh with disk state preserved. Not
  instant, but the JIT makes it fast enough that the gap is tolerable for most use cases.

  **Recommended approach if pursued:** start with CPU + RAM only (produces a "warm reboot from
  saved state" that skips the ROM init), defer device state to later iterations. Study DOSBox-X's
  `SaveState`/`LoadState` architecture for the subsystem serialization pattern.

### Tier 4 — Automation & Scripting (drive the guest without screenshots)

**Added 2026-06-05.** The ambition: an external tool (CI, an AI agent, a `Shortcuts` workflow)
can *drive and observe* a VM programmatically — not by hunting pixels in a VNC screenshot, but
through structured commands and structured state. This is the natural evolution of the E2E VNC
harness (ROADMAP **A5**): "Playwright-for-VNC" → "Playwright-for-AppleEvents." It also reframes
part of the **Infeasible** list below — a guest agent is only infeasible for *retrofitting an
arbitrary user's OS*; for the **managed VM images we build and ship**, installing a small guest
helper is exactly what Parallels Tools / VirtualBuddyGuest / VMware Tools do.

Prior art (see Sources at end of section): **UTM** exposes an AppleScript dictionary + a
`utmctl` CLI + Shortcuts intents for VM-lifecycle control. **Lume** (MIT) runs an HTTP control
server *and an MCP server for AI-agent integration*, and provisions VMs unattended by automating
the Setup Assistant over VNC+OCR. **Tart** is CLI-only and exists to run macOS VMs in CI.
**VirtualBuddy** ships `VirtualBuddyGuest` for clipboard + shared-folder auto-mount. Silicon
Sheep can take the *control-surface* and *guest-tools* ideas wholesale; the *guest interaction*
layer is the novel classic-Mac work.

#### Shared architecture (2026-06-05 codebase investigation)

Three parallel investigations (one per layer) converged on a single design and surfaced one
correction to the premise above:

- **Correction — the bidirectional RPC does not exist yet.** The text below originally assumed the
  CLI rides "the UDS RPC the launcher already speaks." In fact the emulator is an RPC *client*
  (outbound only — `main_unix.cpp:992`; the method enum in `rpc.h:96-100` is just EXIT /
  ERROR_ALERT / WARNING_ALERT), and the Tauri launcher has **no socket code at all** — it spawns
  with bare args (`SiliconSheep/src-tauri/src/main.rs:104`) and stops a VM via `SIGUSR1`
  (`main.rs:116`). So **Layer A's true first task (A0) is making that channel bidirectional**;
  every other Tier-4 item rides on it.
- **One spine: one protocol, one broker, one idle-hook mailbox.** Every guest-affecting command
  (input injection, AppleEvents, the guest-agent doorbell) must run on the **emulator thread**, and
  the safe service point already exists: the 60 Hz `OP_IDLE_TIME`/`OP_IDLE_TIME_2` idle hook
  (`emul_op.cpp:609/618`), where the proven `e2e_check_host_shutdown()` state machine
  (`emul_op.cpp:149`) already injects ADB events. The design generalizes that hook into a
  **host-side command mailbox**: front-ends (CLI / AppleScript / MCP) → the launcher (single
  broker, enforces the one-VM invariant) → UDS RPC → mailbox → drained in the idle hook on the emul
  thread. Read-only observation (RAM reads via `Mac2HostAddr`) is thread-safe and answers
  synchronously; anything touching guest state is deferred to the hook. **One pump, three consumers.**
- **Three execution contexts govern feasibility:** (1) RAM **observation** — any thread, instant,
  proven; (2) ADB **input** — buffers, drained on the 60 Hz VBL, proven; (3) **`Execute68kTrap` /
  AppleEvents / any Toolbox call** — emul-thread, EMUL_OP context only, **no precedent in the tree
  yet** (`cpu_emulation.h:117`; `HOST-GUEST-CHANNELS.md` §5). Get the context wrong and you corrupt
  the guest.

The per-layer integration designs (with `file:line` anchors, phased build orders, and risks) are in
the **"Integration design — per layer"** subsection below.

#### Layer A — Launcher control surface (pure launcher work, no emulator changes)

- [ ] **`siliconsheep` CLI** — `list / create / start / stop / snapshot / status / config` over
  the UDS RPC the launcher already speaks (`rpc_unix.cpp`, `--gui-connection`). Direct analogue
  of UTM's `utmctl` (which is itself a thin wrapper over its AppleScript bridge).
- [ ] **AppleScript dictionary + Shortcuts intents** — scriptable VM lifecycle for Automator /
  Shortcuts / Script Editor users (UTM ships exactly this).
- [ ] **MCP server** — expose VM control (and, later, the Layer-B guest bridge) as MCP tools so
  an AI agent can spin up, drive, and tear down VMs headlessly. This is Lume's model; it makes
  the emulator a first-class target for agentic workflows.
- [ ] **Headless / CI mode** — run with no SDL window for automated testing (Tart's raison
  d'être). This is the missing enabler for putting the E2E harness in CI (ROADMAP A5 §CI) and
  for the `SS_JIT_VERIFY`-under-E2E gate (ROADMAP A5-V).

#### Layer B — Guest control bridge (the screenshot-free part; novel classic-Mac work)

- [ ] **Structured input injection** — `type(text)`, `click(x,y)`, `key(code)`, `menu(path)` as
  first-class commands over the existing ADB injection path (already used by the E2E shutdown
  hook), replacing pixel-coordinate hunting.
- [ ] **Structured observation API** — window list, frontmost app, menu state, modal/dialog
  detection, read straight from guest RAM (WindowList @ 0x9D6, CurApName @ 0x910, MenuList,
  ScrnBase) — the reads already proven in `e2e_emit_boot_ready_once()`. Queryable instead of
  OCR'd. See `docs/planning/HOST-GUEST-CHANNELS.md`.
- [ ] **AppleEvents bridge** — Mac OS 8/9 shipped AppleScript/OSA. The host can inject AppleEvents
  via `Execute68kTrap(AESend)` (already noted in Tier 2 for `odoc`), so scriptable era apps
  (Finder and many others) can be *driven by command*, not by clicking. "Playwright without
  pixels" for any AppleScript-aware app.

#### Layer C — "SiliconSheep Tools" guest agent (managed images only)

The reframe of the Infeasible list. For VM images **we** create and ship, a tiny classic-Mac
faceless-background app / `INIT` extension that listens on a channel — poll a magic file in the
`extfs` share, a TCP socket via Open Transport, or a custom EmulOp mailbox — unlocks, for our
images, what commercial guest-tools packages do:

- [ ] **On-demand clipboard push/pull, scripted file import, "open URL/doc in guest", time-sync.**
- [ ] **A reliable command channel** for automation that doesn't depend on reading RAM offsets
  (push commands *into* the guest event loop, which host-side RAM reads can't do).
- **Caveats (be honest):** only works on *our* managed images, not a user's BYO disk; needs a
  classic-Mac build toolchain (Retro68 or CodeWarrior); ships as an optional "Install Silicon
  Sheep Tools" step. This is the line between Tier-4-feasible and the truly-Infeasible items below.

#### Smaller prior-art-grounded items

- [ ] **VM gallery with one-click prebuilt images** (Infinite Mac / UTM gallery model) — "Mac OS
  8.6, ready to boot," skipping the install dance entirely.
- [ ] **Scripted/unattended OS install** — automate the classic Mac installer the way Lume
  automates Setup Assistant; turns a blank disk + ISO into a ready VM with no clicks (ties to the
  E2E scratch-disk + boot-from-ISO setup).
- [ ] **Disk snapshots in the UI** — surface `clonefile`-of-disk snapshots as a near-term feature,
  distinct from the very-high-effort full CPU/device-state save/restore filed under Tier 3.

#### Integration design — per layer (2026-06-05 investigation)

Grounded in the actual codebase; `file:line` as of 2026-06-05 (`macos-arm64`). See **Shared
architecture** above for the cross-cutting spine these all reuse.

##### Layer A — control surface

**Integration points:**

| Concern | Symbol / location |
|---|---|
| Emulator spawn (no `--gui-connection` today) | `SiliconSheep/src-tauri/src/main.rs:104-108` (`launch_vm`) |
| Stop = SIGUSR1, not RPC | `main.rs:116-128` (`stop_vm`); handler `main_unix.cpp:275-278` (`sigusr1_handler`) |
| RPC arg parse / client init (outbound only) | `main_unix.cpp:931-934` (`--gui-connection`), `:991-992` (`rpc_init_client`) |
| RPC dispatch loop (exists, unused by emulator) | `rpc_unix.cpp:975-1015` (`rpc_dispatch`), `:1056` (`rpc_method_add_callbacks`) |
| Method enum to extend | `rpc.h:96-100` |
| **Guest-thread service point** (drain inbound here) | `emul_op.cpp:609`/`:618` (`OP_IDLE_TIME[_2]`), beside `e2e_check_host_shutdown()` `:149` |
| Free `status` data | `emul_op.cpp:80` `CurApName`, `:88` `WindowList`+windowKind modal probe |
| Window-creation gate (headless) | `main_unix.cpp:743` (`SDL_INIT_VIDEO`), `video_sdl3.cpp:759` (`SDL_CreateWindow`, unconditional) |
| `--nogui` ≠ headless | `main_unix.cpp:944-945`,`:1002` (suppresses the *settings dialog* only) |
| Headless precedent to copy | `main_unix.cpp:750-751` (`SDL_AUDIODRIVER=dummy` set before `SDL_Init` to avoid a headless hang) |
| VNC reads a surface, not the window | `vnc_server.h:7` (`VNCServerUpdate(SDL_Surface*)`), pixels `vnc_server.cpp:232-340` |

**Proposed RPC methods** (added to `rpc.h:96`; emulator registers callbacks + pumps `rpc_dispatch`
from the idle hook so handlers run on the emul thread): `STATUS` (read `CurApName`/`WindowList`,
cheap), `SHUTDOWN` (replaces the SIGUSR1 hack), `RELOAD_PREFS` (runtime-safe keys only),
`PAUSE`/`RESUME`, `SNAPSHOT` (must quiesce+flush before the host `clonefile`s the disk —
`vm.rs:309-353` already has the clone primitive). Transport already supports the needed arg types
(`rpc.h:50-69`).

**Phased build order:** A0 **bidirectional RPC bring-up** (M, med — prerequisite) → A1 `STATUS`
(S, low) → A2 `siliconsheep` CLI over the launcher endpoint (S–M, low) → A4 `SHUTDOWN` (S, low) →
A6 MCP server (M, low); then in parallel A3 **headless** (M, **high** — see below) and A7
AppleScript dict + Shortcuts (M, med — real Obj-C bridging in a Rust/Tauri app, *not* the "S" the
bullets imply); hard tail A5 `RELOAD_PREFS` (M), A8 `PAUSE/RESUME` (L, high — **`SIGUSR2` is already
the nanokernel interrupt**, `main_unix.cpp:1269` — collision risk), A9 running-VM `SNAPSHOT` (L,
**high correctness risk** — a mid-run `clonefile` can capture a torn FS; snapshot-while-stopped is
the safe S).

**Headless reality:** `--nogui` does *not* help. The realistic path mirrors the existing
audio-dummy trick — gate `setenv("SDL_VIDEODRIVER","dummy")` before `SDL_Init` and/or skip
`SDL_CreateWindow`. **But ROADMAP A5 notes macOS SDL needs a live WindowServer (no Xvfb equiv)** —
so even dummy-video may require a logged-in self-hosted Mac, and "render to VNC with no window" is a
*hypothesis to prove* (does a blittable `host_surface` survive under the dummy driver?), not a given.

**Open questions:** reentrancy contract (defer *all* inbound handlers to the idle hook vs answer
read-only `STATUS` on the reader thread?); which prefs are hot-reloadable (allowlist); one socket or
two (recommend a separate launcher control socket proxied to the emulator UDS, so the launcher stays
the single broker); carry a VM handle in the protocol from day one even though only one VM runs now.

##### Layer B — guest control bridge

Sorts entirely by the **three execution contexts** (see Shared architecture). Observation = any
thread (proven, `e2e_emit_idle_signals` `emul_op.cpp:76`); ADB input = buffers drained on the VBL
(proven, `adb.cpp:239-321`, already driven from both the VNC path and the idle hook); `AESend` =
emul-thread/EMUL_OP only (**no precedent in the tree**).

**Input API → ADB:** `move/click` → `ADBMouseMoved`+`ADBMouseDown/Up` (`adb.cpp:239/257/273`);
coords are **Mac logical pixels in *absolute* mode** (`vnc_server.cpp:207`) — the bridge **must
force `ADBSetRelMouseMode(false)`** (`adb.cpp:289`) because a grabbed window switches to relative
deltas (`video_sdl3.cpp:1137`). `key` → modifier-down, `ADBKeyDown/Up(code)`, modifier-up (raw Mac
keycodes; Return=0x24 as in `emul_op.cpp:176`). **Two hidden costs:** `type("text")` needs an
**ASCII→keycode table that does not exist** (`event2keycode`/`keycode_table` `video_sdl3.cpp:2141`
is keysym→Mac, not char→Mac); `menu(path)` is **not one ADB event** — either synthesize
mouse-drag from `MenuList` (0x0A1C) geometry (fragile) or call `MenuSelect` via `Execute68kTrap`
(cleaner but emul-thread-bound). Size both above `click`.

**Observation API (pure RAM reads, offsets per `HOST-GUEST-CHANNELS.md` §3):** `CurApName` 0x0910
(debounce — churns ~6/s); `WindowList` 0x09D6 (walk `+0x18` next, `+0x08` title, `+0x04` bounds,
`+0x6C` windowKind: 2=dialog); modal = front `windowKind==2` (the check at `emul_op.cpp:88-95`);
`MenuList` 0x0A1C; `EventQueue` 0x014C; `Ticks` 0x016A; `ScrnBase` 0x0824. Process list (8+) is not
a low-mem global — needs `GetNextProcess` via `Execute68kTrap`.

**AppleEvents bridge (design, unproven):** build descriptors in guest RAM with the proven idiom
(`NewPtrSysClear` via `Execute68kTrap(0xa71e,…)` + `WriteMacInt*`, as `disk.cpp:267-272`), then
`AECreateAppleEvent`/`AEPutParam*`/`AESend`. **The exact Apple Event Manager trap/`_Pack8` selector
is TBD — verify against *Inside Macintosh: IAC*; do not hardcode.** Drives scriptable apps with an
`'aete'` (Finder `odoc`/`quit`, AppleScript-aware apps); **cannot** do non-scriptable apps or block
on a reply (use `kAENoReply`, observe results via RAM polling).

**Phased:** B1 observation (S, low — half-built) → B2 command mailbox generalizing
`e2e_check_host_shutdown`, registered on **both** `OP_IDLE_TIME`/`_2` (S, low) → B3 key/click/move +
the new char table (S–M) → B4 `menu()` (M, med) → B5 AppleEvents (L, **high** — the moonshot).
**Risks:** idle-hook liveness gates all mailbox-routed commands (a busy/modal guest can starve them;
pure ADB/RAM are unaffected); low-mem offsets need a System 7→9.0.4 spot-check before Tier-3 chrome
relies on them; HiDPI coordinate-space assertion on the direct-host path.

##### Layer C — guest agent ("SiliconSheep Tools", managed images only)

**Channel choice — recommend (c) EmulOp RAM mailbox for control + (a) ExtFS for bulk; defer
(b) OT/TCP:**

| Channel | Mechanism (this codebase) | Verdict |
|---|---|---|
| (a) ExtFS "magic file" | single `extfs` root HLE'd to host file I/O (`extfs.cpp:438-443`, `extfs_unix.cpp:316/327`); poll a control file | **bulk payload** — coarse, weak as a doorbell |
| (b) Open Transport TCP | slirp guest→host `10.0.2.2` free; host→guest needs `redir` (`ether_unix.cpp:1191`, `slirp/ctl.h:6-7`); heavy OT guest code, net-stack-up dependency | **defer** (optional future fallback) |
| (c) **Custom EmulOp mailbox** | EMUL_OP is **opcode-gated, not address-gated** — a guest trap word dispatches to host `EmulOp()` from anywhere (`sheepshaver_glue.cpp:321`; JIT routes major-opcode-6 and refuses to chain into trampolines, `ppc-jit.cpp:3246`). Add one enum (`emul_op.h:41-54`) + one `case` (`emul_op.cpp:187`). | **best control channel** — synchronous, zero setup, no networking |

**Mechanism:** the Tools app `NewPtr`s a mailbox struct and registers its address once via a new
`OP_SST_REGISTER` selector; the host then reads/writes it directly (`WriteMacInt*`) and pumps it
from the **same 60 Hz idle hook** (`emul_op.cpp:76/609`). Bulk file *contents* go via ExtFS; only
the *filename* rides the mailbox. **Split-phase guest design (the load-bearing classic-Mac
constraint):** an interrupt-time VBL/Time-Manager task may only *read the mailbox flag* (no Memory
Manager / sync File Manager / `AESend` at interrupt time); the real work runs at **system-task
time** in the agent's own event loop. v1 commands: `PING`, `CLIPBOARD_PUT/GET`, `IMPORT_FILE`,
`OPEN_DOC`/`OPEN_URL`, `TIME_SYNC`, `RUN_SCRIPT`.

**Honest costs (flagged, not buried):** (1) **no classic-Mac toolchain exists in the repo** —
**Retro68** is net-new (free, scriptable, builds 68k+PPC with resource forks; CodeWarrior only if a
glue gap appears); a 68k agent is the pragmatic default. (2) **Writing the helper into an HFS `.dsk`
is unsolved + per-image + ongoing maintenance** — offline `libhfs`/`hfsutils` write vs ExtFS-copy +
first-boot mover vs boot-once installer (must preserve resource fork + type/creator). (3) The agent
is **ABI-coupled to the shipped emulator** (mailbox selector lives in `emul_op.h`), so guest image
and binary version together.

**Unlocks (Layer-C-only — pushing a command *into* the guest event loop):** scripted
`odoc`/`gurl` from a *safe* time; on-demand AppleScript/OSA; a stable command ABI we own (vs Layer
B's brittle RAM offsets); host-initiated clipboard. **Be honest:** time-sync (host can already write
`Time` 0x020C) and clipboard (`OP_GET/PUT_SCRAP`) are already partly host-side — C adds only the
*on-demand* form. **Does NOT solve** coherence / live resolution (need a display driver) or
arbitrary-PC interruption — those stay infeasible.

**Phased:** C0 mailbox spike (`OP_SST_REGISTER`+`PING`, trivial Retro68 stub) (S, low) → C1
toolchain + HFS-install mechanism + one pre-installed image (M, **med-high** — biggest risk) → C2
clipboard/import/time-sync (M, med) → C3 AppleEvents commands (M, med) → C4 wire to Layer A (S) →
C5 optional OT/TCP (L, high). **Open:** HFS-install mechanism (gates C1); ABI mismatch handling;
agent CPU when no front app yields (VBL vs `SystemTask`); `RUN_SCRIPT` = host→guest code-exec, gate it?

#### Sources (for future agents picking this up)

- UTM scripting (AppleScript dictionary + `utmctl` CLI + Shortcuts): https://docs.getutm.app/scripting/scripting/ · repo https://github.com/utmapp/UTM
- Lume (MIT; HTTP control server + MCP server for AI agents; VNC+OCR Setup-Assistant automation): https://cua.ai/docs/lume/guide/getting-started/comparison
- Tart (CI-focused macOS VMs on Apple Silicon, CLI-only): https://tart.run/
- VirtualBuddy + `VirtualBuddyGuest` (clipboard + shared-folder auto-mount guest agent): https://github.com/insidegui/VirtualBuddy
- Internal substrate: `docs/planning/HOST-GUEST-CHANNELS.md` (readable guest structures + channel limits), `rpc_unix.cpp` (UDS RPC), ROADMAP A5 / A5-V (E2E harness this feeds).

### Infeasible (no workaround for classic Mac OS)

> **Note (2026-06-05):** the *guest-agent* premise below is relaxed by **Tier 4 Layer C** for the
> VM images we build and ship (just as Parallels/VMware/VirtualBuddy ship guest tools). It stays
> infeasible for *retrofitting an arbitrary user's existing OS* without installing our helper, and
> the display-driver-dependent items (coherence, live resolution) remain infeasible regardless.

These features **require a guest agent, guest kernel extension, or modern guest OS** — none of
which exist or can be injected for Mac OS 8/9 *(without shipping our own guest tools — see Tier 4 Layer C)*:

- **Full Coherence/Unity mode** (continuous per-window pixel buffers). Parallels/VMware do this
  via a guest-side hook DLL + custom display driver. No equivalent for Mac OS 9.
- **Guest-initiated shared folder mounting** (auto-mounts from inside guest). Requires a guest
  daemon. Sharing must remain host-initiated via `extfs` HLE.
- **Dynamic memory ballooning** (adjust guest RAM live). Requires a guest kernel driver.
- **Guest-initiated resolution changes** (dragging the host window resizes the guest). Mac OS
  8/9 only changes resolution via the Monitors control panel; no programmatic API to push
  resolution changes into the guest without a display driver stub.

---

## JIT + GUI: Orthogonal but Complementary

The ARM64 JIT and this GUI frontend are independent efforts that together represent a
step-change improvement:

- **GUI frontend** removes the usability barrier for new users.
- **ARM64 JIT** delivers native Apple Silicon performance (1.88× over interpreter; boots
  Mac OS 8.6 to Finder in ~10s).

The JIT is fully operational — the launcher should default to the native ARM64 binary with JIT
enabled. No Rosetta 2 fallback is needed.

---

## What Other Emulators Teach Us

**Competitive teardown (2026-06-05):**

| Product | What's worth stealing | What's impossible for us |
|---------|----------------------|--------------------------|
| **Parallels** | VM gallery, linked clones, first-run wizard UX, settings sidebar, Dock integration | Coherence mode (requires `prl_hook.dll` guest agent), dynamic RAM, guest-initiated resolution |
| **VMware Fusion** | VM library (filter/tag/search/clone badges), linked clones, Unity mode UX | Unity mode (requires guest tools), balloon driver |
| **UTM** | Prebuilt VM gallery, Apple Virtualization integration, how they handle "no guest tools" case for older OSes — closest to our situation | SPICE agent features (clipboard, display resize) |
| **Infinite Mac** | Browser-based classic Mac UX — chronological OS picker, instant boot, drag-and-drop file import. **Best model for the "retro OS in modern frame" problem** | N/A (different architecture entirely) |
| **DOSBox-X** | 100-slot save states, CRT/scanline shaders, built-in capture, pixel-perfect scaling | Save states require full CPU/device serialization (very high effort) |

**Key insight:** APFS `clonefile(2)` gives us VMware's linked clones for free — duplicate a 4 GB
`.dsk` in milliseconds, zero extra disk space until divergence. "Duplicate VM" becomes instant.

**Conclusion**: SheepShaver's existing clipboard and volume-sharing implementations are already
at or above the state of the art for classic Mac OS emulation. The gap is purely UX. The
no-guest-agent constraint means we get the *launcher/management* features from
Parallels/VMware but not their *coherence/integration* features — and that's fine, because no
one else has solved coherence for classic Mac OS either.

---

## Developer Inspector (Snow-inspired) — 🟡 Tier 2 complete, P1 shipped

**Architecture: the Inspector is a separate window** — like Chrome DevTools detaching from the
browser, or Xcode Instruments being its own app. The settings panel is a control panel (trivial:
RAM, resolution, networking); the Inspector is a full development tool (complex: JIT stats,
execution tracing, memory inspection, profiling). Different audiences, different complexity,
different screen real estate.

The Inspector opens as its own Tauri `WebviewWindow` (same mechanism as the settings window),
not a tab in the detail pane. The current "Inspector" tab in settings is the minimum viable
stub; the real Inspector window replaces it.

**Build a live observability *inspector*, not a Snow-style step-debugger.** Our debugging is
*differential* (`SS_JIT_VERIFY`, the harness), not single-step. Borrow Snow's answer to *"which
surfaces matter"*, not its architecture. Full crosswalk: `SNOW-EVALUATION-PLAN.md` §S1.

### Live Observability — ✅ Tier 1 shipped

Heartbeat parsing, signal timeline, log viewer, stats gauges — all working in the Inspector
tab. Data flows: emulator stderr → Rust thread → AppState cache → Tauri command → web UI.

### Machine State — ✅ Tier 2 complete

- [x] **Register inspector** — GPR (r0-r31), SPR (LR/CTR/XER/CR), PC via `RPC_METHOD_DUMP_REGISTERS`.
  Change-highlighting between snapshots (Snow-style yellow).
- [x] **Memory hex viewer** — read N bytes at guest address via `RPC_METHOD_READ_MEMORY` (already
  implemented in emulator, capped at 64K). Classic hex+ASCII grid with navigable address input.
- [x] **Guest state panel** — CurApName, WindowList, Ticks, SysVersion, MBarHeight from
  low-memory globals (`HOST-GUEST-CHANNELS.md`). Periodic poll via C2.0 RPC.

### Profiler / Session Recording — 🟡 P1 shipped (Chrome DevTools-inspired)

**The vision:** a "Performance" panel like Chrome DevTools — click Record, run something in the
guest, click Stop, see a timeline of what happened. Which blocks were hot, where fallbacks
occurred, what was slow. This is the JIT equivalent of a flame chart.

**Inspiration:** Chrome DevTools Performance tab (recording + flame chart), Xcode Instruments
(timeline + detail), Firefox Profiler (web-based, shareable).

**Tier P1 — Session recording (existing data, new recording/playback UI):**
- [x] **Start/Stop recording** button in the Inspector window. While recording, capture all
  `[HB]` heartbeats, `[BOOT]`/`[APP]`/`[STALL]` signals, and j2i/fallback events to a session
  file (JSON or binary). Timestamped.
- [x] **Session timeline** — replay the recording as a scrollable waterfall. X-axis = time,
  Y-axis = events. Zoom in/out. Click an event to see its detail.
- [x] **Fallback trace** — log every interpreter fallback with the PC and opcode (needs a new
  emitter in `ppc-cpu.cpp`, guarded by an env var or RPC command to avoid overhead when not
  recording). This is the "why was this slow?" data.
- [x] **Export/share** — save the session as a `.sheepshaver-profile` file that can be reopened
  or shared for diagnosis (like Firefox Profiler's shareable URLs).

**Tier P2 — Per-block profiling (needs B1, the execution-weighted profiler):**
- [x] **Hot block table** — sorted by execution count. Shows PPC address, instruction count,
  native code size, hit count. Source: B1 profiler data via C2.0 RPC.
- [x] **Heat map** — visual representation of ROM/RAM regions by execution density.
  Bar chart with labeled regions (ROM/RAM/DR), execution counts, and percentage bars.
- [x] **Instruction mix** — breakdown of which PPC opcodes are executing most. Ranked list
  with execution counts, percentages, and bars. Identifies optimization targets.

**Tier P3 — Flame chart (needs block-level timing):**
- [x] **Block-level timing** — measure wall-clock time per JIT block execution. Needs
  `mach_absolute_time()` instrumentation in the dispatch loop (very low overhead with the
  rdtsc approach, but still a cost — guarded by recording mode only).
- [x] **Flame chart** — SVG visualization below Block Timing table. Each row = a JIT block,
  width = time spent, color-coded red→yellow by hotness. Hover for detail (time, count, avg).
  Top 40 blocks rendered.
- [x] **Comparison** — load two .sheepshaver-profile files side by side. Shows duration,
  event count, snapshot count, per-metric diff, and dual sparkline rate-over-time charts.

### Data paths

```
Live observability (Tier 1, shipped):
  Emulator stderr ──→ Rust thread ──→ parse [HB]/signals ──→ AppState cache ──→ web UI

C2.0 RPC (Tier 2, landed):
  SiliconSheep ──→ UDS socket ──→ emulator handler ──→ reply
  (sub-16ms, bidirectional, supports READ_MEMORY + DUMP_REGISTERS)

Profiler recording (Tier P1, planned):
  Emulator ──→ structured event stream (stderr or dedicated RPC channel)
  ──→ session file (.sheepshaver-profile) ──→ Inspector timeline UI

B1 execution profiler (Tier P2, planned — OPTIMIZATION-PLAN §P0):
  JIT dispatch loop ──→ per-block counters ──→ RPC_METHOD_GET_PROFILE
  ──→ hot block table / heat map
```

**Sequence:** ✅ Tier 1 (shipped) → Tier 2 machine state (C2.0 ready) → Tier P1 session
recording (UI + event capture) → B1 profiler (data) → Tier P2 hot blocks → Tier P3 flame
chart (only if earned).

**Cross-refs:** `SNOW-EVALUATION-PLAN.md` (crosswalk), `OPTIMIZATION-PLAN.md` §P0/B1,
`HOST-GUEST-CHANNELS.md`, C2.0 RPC above.

### Bug Report Bundle — ✅ implemented

One-click "Report Bug" button generates a `.zip` diagnostic bundle containing:
- SiliconSheep UI screenshot (captured from the web layer via `html2canvas`, no system permissions)
- Guest screen capture (from VNC, already implemented)
- Last run log (from `.sheepvm/logs/`)
- Inspector stats snapshot (from the live cache)
- VM profile (prefs, OS version, RAM, screen config — sanitized)
- Emulator build info (from stderr first line)
- Host environment info (macOS version, architecture, Tauri version)

All data sources already exist — no emulator changes needed. User can attach the zip to a GitHub
issue or share for diagnosis.

**Cross-refs:** `SNOW-EVALUATION-PLAN.md` (crosswalk + rationale), `HOST-GUEST-CHANNELS.md`,
`OPTIMIZATION-PLAN.md` §P0/B1, Tier 4 Layer A (the RPC transport once it exists).

---

## Implementation Notes

Research from codebase deep-dives. Reference this before writing any code.

### Cocoa Launcher Architecture

The existing launcher is clean and well-factored:

| File | Lines | Role |
|---|---|---|
| `AppController.mm` | ~70 | Entry point; re-shows window on Dock click |
| `VMListController.mm` | ~390 | VM table (NSUserDefaults `"vm_list"`), new/import/launch/delete actions |
| `VMSettingsController.mm` | ~490 | All preference UI; reads/writes text prefs file |
| `DiskType.m` | ~35 | Lightweight disk/CDROM model object |
| `prefs_macosx.mm` | ~130 | Injects "Preferences…" menu item into running emulator |

**Key facts:**
- The launcher and emulator are **separate processes**. `VMListController` spawns `NSTask` pointing at the `SheepShaver` Unix binary bundled as an auxiliary executable. No shared memory, no embedded emulator.
- Each VM is a `.sheepvm` bundle directory. Prefs live at `MyVM.sheepvm/prefs` as a plain-text key-value file (`KEYWORD value`, one per line).
- `NSUserDefaults` with key `"vm_list"` stores the array of VM bundle paths — not the prefs themselves.
- `VMSettingsController` uses deprecated `NSOpenPanel`/`NSFileManager` APIs (pre-10.6 style). Modernizing to URL-based APIs is a low-risk early win.
- Code comments in `VMListController.mm` call out three unfinished TODOs: drag-from-Finder import, ROM/keycode copy-into-bundle checkbox, and "copy path" feature — these align directly with Tier 1 roadmap items.

**Gaps vs. modern launcher (in priority order):**
1. No first-run wizard — new VM flow jumps directly to the settings dialog
2. No auto-discovery of `.sheepvm` bundles (e.g., scan ~/Documents)
3. No drag-and-drop of ROM/disk/keycode files into the launcher
4. No first-class disk image creation beyond raw `dd`
5. No validation or diagnostics on prefs values

### Prefs System

**Format:** Plain text, `KEYWORD value` per line, comments with `#` or `;`. Stored at `<VM.sheepvm>/prefs` or `~/.sheepshaver_prefs`.

**Runtime:** Parsed once at startup into an in-memory linked list. `PrefsFindString/Int32/Bool()` for reads; `PrefsReplaceString/Int32/Bool()` for writes; `SavePrefs()` on exit. No reload mechanism exists today.

**Hot-reload feasibility by pref key:**

| Setting | Hot-reloadable? | Notes |
|---|---|---|
| `frameskip` | ✅ Yes | Read each frame; just update the pref |
| `mousewheelmode` / `mousewheellines` | ✅ Yes | Input-only |
| `ignoresegv` / `ignoreillegal` | ✅ Yes | Signal handler booleans |
| `screen` | ✅ Mostly | SDL window resize; needs frame-boundary sync |
| `extfs` | ✅ Yes | Has `ExtFSInit()`/`ExtFSExit()` pair; re-entrant with minor refactor |
| `gfxaccel` | ⚠️ Maybe | Requires video layer re-init |
| `nosound` / `nonet` | ⚠️ Risky | Driver re-init; high crash risk |
| `ramsize` | ❌ No | Changes emulated RAM layout; impossible mid-run |
| `jit` / `jit68k` | ❌ No | CPU compilation mode |
| `disk` / `cdrom` | ❌ No | Block devices mapped at `DiskInit()` |

**Minimum viable hot-reload implementation:**
1. Add `PrefsRegisterReloadHandler(name, callback)` + `PrefsReloadFromDisk(name)` to `prefs.cpp` (~200 LOC, needs a `pthread_mutex_t`).
2. Register handlers in `video.cpp` (frameskip), `extfs.cpp` (path remount), and optionally input code (mousewheel).
3. Add a `SIGHUP` handler that calls `PrefsReloadFromDisk()`, OR extend the existing UDS RPC
   (`rpc_unix.cpp`, activated via `--gui-connection <path>`) with `RPC_METHOD_RELOAD_PREFS`.
   The RPC path is preferred — it allows targeted reload of specific keys.

**Existing IPC mechanism:** `rpc_unix.cpp` implements a Unix-domain-socket RPC layer with
`RPC_METHOD_EXIT`, `RPC_METHOD_ERROR_ALERT`, `RPC_METHOD_WARNING_ALERT`. Activated by the
`--gui-connection <socket-path>` CLI flag. Extensible for hot-reload, status queries, etc.
Tauri's Rust side connects to this socket for Phase 2 IPC.

### extfs Multi-Volume

**Current limit:** One root path (`static const char *RootPath` at `extfs.cpp:109`), one drive number, one media type.

**Why it's fixable:** The Mac FSM already handles multi-volume correctly — it routes each file operation to the right VCB automatically. The limitation is entirely in our `extfs.cpp`. The FSM calls `ExtFSHFS(uint32 vcb, ...)` with the correct VCB per operation; we just need to reverse-map VCB → RootPath.

**Required changes (estimated ~2–3 days):**

1. Replace three globals with a `VolumeConfig volumes[MAX_VOLUMES]` array (path, stat, drive_number per entry).
2. Load N paths from prefs in `ExtFSInit()` — e.g., iterate `"extfs"`, `"extfs2"`, `"extfs3"` keys.
3. Loop `AddDrive()` + `PBVolumeMount()` in `InstallExtFS()` once per volume.
4. Add `get_volume_config(uint32 vcb)` helper for VCB→path lookup.
5. Thread `vcb` parameter through `get_path_for_fsitem()` and its ~15 call sites.

**Biggest risk:** The FSItem CNID cache is currently global. If CNIDs must be per-volume, separate caches are needed; if globally unique (likely), the shared cache is fine.

**Prior art:** No existing fork has implemented this. The git log shows drag-and-drop volume support was added to the Windows GUI (`#135`) but `extfs.cpp` was never updated to match.

### Tauri Sidecar Architecture

**Project location:** `SiliconSheep/` at repo root — sibling to `SheepShaver/` and `BasiliskII/`.
Clean boundary: SiliconSheep never `#include`s emulator headers. It interacts with SheepShaver
only through the prefs file format, process lifecycle, and (future) UDS IPC.

**Sidecar launch:** `tauri-plugin-shell` with `externalBin`. The SheepShaver binary is copied to
`SiliconSheep/src-tauri/binaries/SheepShaver-aarch64-apple-darwin` by a prebuild script. Tauri
manages launch, stdout/stderr capture, and termination notification.

**IPC — layered by phase:**

| Need | Phase 1 (no C++ changes) | Phase 2 (extend emulator) |
|------|--------------------------|---------------------------|
| Status/heartbeat | Parse stderr for `[HB ...]` lines | — |
| Clean shutdown | `kill(pid, SIGUSR1)` | — |
| Screenshots | VNC to `localhost:{vncport}` or `ScrnBase` read | — |
| Error/warning | Parse stderr `ERROR:`/`WARNING:` | Extend `rpc_unix.cpp` |
| Hot-reload prefs | Write prefs + SIGTERM + relaunch | `SIGHUP` handler + `PrefsReloadFromDisk()` |
| Bidirectional RPC | Not needed | Extend `rpc_unix.cpp` (existing UDS RPC via `--gui-connection`) |

**Sidecar entitlements (open question):** The SheepShaver binary needs
`com.apple.security.cs.allow-jit` for `MAP_JIT` W^X on ARM64. Tauri's bundler may not
propagate entitlements to sidecar binaries — may need a post-build `codesign` step. The
existing `SheepShaver.entitlements` file in `src/MacOSX/` declares the right keys.

### Build System

**Emulator:** Autotools (unchanged). `SheepShaver/src/Unix/` → `make` → binary.

**Launcher:** Tauri v2 (Rust + pnpm). `SiliconSheep/` → `pnpm install && pnpm tauri build`.
Dependencies: Rust 1.70+, Node 18+, pnpm. No Xcode.app needed.

**Integration:** A `prebuild.sh` script builds the emulator via Autotools and copies the
binary to the Tauri binaries dir. `tauri.conf.json`'s `beforeBuildCommand` calls this.

The existing Xcode projects (`SheepShaverLauncher.xcodeproj`) and Cocoa launcher
(`Launcher/*.mm`) remain in the repo as upstream code — not modified or deleted.

### ROM Loading and Verification

**No hash verification exists.** ROM loading (`main_unix.cpp:581–613`) only checks:
1. File size is exactly 4 MB (`ROM_SIZE = 0x400000`)
2. Format auto-detection: plain 4MB image, CHRP/LZSS compressed, or macOS 9.2.x parcels format

**Search order:** User-configured `rom` pref → `./ROM` → `./Mac OS ROM`.

**Implication for first-run wizard:** The ROM picker is a neutral file picker — no legal guidance. On the "where do I find a ROM?" help text, link to community resources:
- [E-Maculation SheepShaver setup guide](https://www.emaculation.com/doku.php/sheepshaver_mac_os_x_setup)
- [E-Maculation forum](https://www.emaculation.com/forum/)
- [68k MLA (68kMLA.org)](https://68kmla.org/)

We can optionally add SHA-256 verification against known-good ROMs in the launcher (purely as a "this ROM is recognised" indicator, not a gate), since the emulator itself only checks file size. Three ROM formats to handle:
- Plain 4MB PCI PowerMac ROM
- CHRP compressed (NewWorld)
- Parcels-based (macOS 9.2.x)

---

## Open Questions

1. **Sidecar entitlements:** Does `tauri build` propagate entitlements to `externalBin`
   binaries, or is a manual `codesign` step required? Needs empirical testing.
2. **WindowList reliability:** Is `Mac2HostAddr()` + WindowList parsing reliable enough across
   OS versions (System 7 through 9.0.4) to build Tier 3 on? The e2e harness already reads
   `CurApName` and `WindowList` successfully — encouraging but needs broader testing.
3. **extfs multi-value vs numbered keys:** Use the multi-value pref pattern (multiple `extfs`
   lines, like `disk`) — path of least resistance, consistent with the rest of the prefs file.
   (Consensus from earlier research; treat as decided unless problems arise.)
4. ~~Should SiliconSheep live as a subdirectory or a separate repo?~~ **Decided:** sibling
   directory at `SiliconSheep/` in this repo. The hard-fork question is deferred but recognized
   as increasingly inevitable.

## Host-Guest Interaction Channels

Full reference in `docs/planning/HOST-GUEST-CHANNELS.md` — documents all existing and
achievable host↔guest channels, readable guest OS structures (low-memory globals, WindowList,
CurApName, MenuList, ScrnBase, etc.), and the hard limits (no arbitrary guest code execution
on demand, no guest-initiated async callbacks, no Toolbox calls outside EmulOp context).
