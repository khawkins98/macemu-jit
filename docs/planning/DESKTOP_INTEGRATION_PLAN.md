# Silicon Sheep — Desktop Integration Feature Plan

> **Status:** 🟡 Active — scaffolded · **Created:** 2026-06-02 · **Updated:** 2026-06-05
> **Why this doc exists:** "Silicon Sheep" — a Tauri v2 launcher/VM manager for SheepShaver (first-run wizard, VM library, hot-reload, coherence-lite). Framework pivoted from Cocoa to Tauri (2026-06-05). Scaffold at `SiliconSheep/`.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._


> *Do they dream of OS X?*

A modern Cocoa launcher and desktop integration layer for SheepShaver, targeting Apple Silicon + macOS. Research synthesis from lateral-thinking agent review, June 2026.

---

## Repository Strategy

### Hard fork, not a submodule

Silicon Sheep will be a **hard fork of kanjitalk755/macemu** (the most actively maintained upstream), not a submodule or thin wrapper. Reasons:

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
| `SheepShaver/src/Unix/prefs_editor_gtk.cpp` | GTK prefs editor; replaced by Silicon Sheep launcher |
| `SheepShaver/src/MacOSX/SheepShaver.xcodeproj` | Replaced by programmatic Cocoa build |
| `SheepShaver/src/MacOSX/SheepShaver_Xcode8.xcodeproj` | Same |
| `SheepShaver/src/MacOSX/Launcher/SheepShaverLauncher.xcodeproj` | Replaced by Silicon Sheep |
| Old 32-bit / i386 JIT backends | Targeting ARM64 + x86_64 only |

**What stays:**

- `BasiliskII/` core — shares `extfs.cpp`, `clip_macosx64.mm`, and other components with SheepShaver. Keep it; it also fills the 68k gap (System 6/7) that SheepShaver can't.
- `cxmon/` — the built-in debugger; useful for development.
- All `src/Unix/` and `src/MacOSX/` code not covered above.
- SDL2 video/audio backends.

### Silicon Sheep launcher location

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

- [ ] **Screen 1 — Welcome.** Illustration of a classic Mac desktop inside a modern macOS window
  frame. "Silicon Sheep — Classic Mac OS on Apple Silicon." Single button: "Get Started."
- [ ] **Screen 2 — ROM.** "You need a Macintosh ROM file." Large drop target + browse button.
  SHA-256 check against known-good hashes: recognized → green check + ROM name; unrecognized
  but 4 MB → amber "accepted (unverified)"; wrong size → red error. SHA is an *indicator*,
  never a gate. Help disclosure links to E-Maculation and 68kMLA (no hosted downloads).
- [ ] **Screen 3 — Disk.** Two paths: (A) "I have a disk image" — file picker. (B) "Create a
  new disk" — size slider (500 MB / 1 / 2 / 4 GB, default 2 GB), auto-creates raw image.
  Optional: "Do you have a Mac OS install CD image?" drop target for ISO/toast.
- [ ] **Screen 4 — Review & Boot.** Summary card: ROM, disk, RAM (256 MB default), display
  (windowed 1024×768), networking (slirp on). All editable inline. Single "Start" button
  creates `.sheepvm` bundle, writes prefs, launches SheepShaver.

#### VM Library

- [ ] **Card grid**, responsive (1 col narrow → 2–3 wide). Per card: name (editable),
  screenshot thumbnail (captured on clean shutdown, placeholder if never run), OS version,
  disk size, last-launched date. Status pill: Running (green) / Stopped (grey).
- [ ] **Actions per card:** Play, Settings (gear), Duplicate (APFS `clonefile` — instant,
  zero-copy on APFS), Delete (confirmation, option to keep disk image).
- [ ] **Top-level "+" button** opens the wizard flow (minus welcome screen).
- [ ] **Import:** drag `.sheepvm` folder onto library. **Export:** right-click → Reveal in Finder.
- [ ] **Search/filter bar** appears once library exceeds 6 VMs.

#### Settings Panel

- [ ] **Sidebar categories:** General (name, RAM, ROM), Display (resolution, scale), Storage
  (disks, shared folders), Network (slirp/vde), Advanced (JIT toggle, debug env vars).
- [ ] **Hot-reload indicators per setting:** "Applies instantly" (frameskip, mouse) vs
  "Applies on next boot" (writes prefs, auto-restarts child) vs "Requires shutdown" (RAM,
  JIT — greyed out while running).

#### Other Tier 1 Items

- [ ] **Auto-restart on pref change**: Write prefs, SIGTERM child, relaunch. Eliminates the
  manual quit/relaunch cycle for non-hot-reloadable settings.
- [ ] **Fullscreen escape overlay**: Hover-reveal bar at top edge: "Press Ctrl-Return to exit
  fullscreen" with clickable button. Auto-hides after 3s, reappears on mouse-to-top-edge.
- [ ] **Dark mode**: CSS `prefers-color-scheme` — free with web UI. Guest display is always
  the guest's own palette; don't try to tint it.
- [ ] **Gatekeeper mitigation**: Detect blocked launch (exit code / `xattr` check). Dialog:
  "macOS blocked SheepShaver." One-click `xattr -cr` "Fix Now" button (admin password prompt).
- [ ] **Disk backup ("snapshots")**: Available only when VM is stopped. Copies `.dsk` with
  timestamp suffix. Restore = swap file back. Labelled honestly as "Disk Backup", not
  "snapshot" (no saved CPU/RAM state). Uses APFS `clonefile` when possible (instant).
- [ ] **Screenshot/recording capture**: Grab the SDL framebuffer from host side (no guest
  involvement).
- [ ] **Drag-and-drop file import**: Drop host files onto the launcher → write to the `extfs`
  shared folder. Pure launcher plumbing, no emulator change needed.
- [ ] **CRT/scanline shaders + integer scaling** (stretch): SDL render pipeline or Metal
  post-process pass. Pixel-perfect integer scaling by default (retro aesthetic); optional
  "Smooth scaling" toggle for bilinear.
- [ ] **Coach marks** for first-time tasks: "Your install CD is mounted…", "Click inside the
  classic desktop to capture the mouse. Press Ctrl-F5 to release.", "Networking is on. Open
  TCP/IP in Control Panels and set Configure to 'Using DHCP Server'."

#### Error States

- [ ] ROM not found: red banner + "Locate ROM" file picker.
- [ ] Disk missing/corrupt: card warning badge → "Locate" / "Remove from VM".
- [ ] Emulator crashed: card flips to "Crashed" (red pill), shows stderr tail. "Relaunch" +
  "View Full Log" buttons.
- [ ] Mouse capture toast on first launch.

### Tier 2 — Enhanced Desktop Integration (emulator IPC/hooks needed)

Requires C++ changes to the emulator alongside Tauri frontend work. Key enabler: SheepShaver
already has a Unix-domain-socket RPC layer (`rpc_unix.cpp`, activated via `--gui-connection
<path>`) with `RPC_METHOD_EXIT`, `RPC_METHOD_ERROR_ALERT`, `RPC_METHOD_WARNING_ALERT`. Extend
this with new methods rather than inventing a new protocol.

- [ ] **True hot-reload** for frameskip/mouse/display: Add `SIGHUP` handler +
  `PrefsReloadFromDisk()` (~200 LOC in `prefs.cpp`), or extend the UDS RPC with
  `RPC_METHOD_RELOAD_PREFS`.
- [ ] **Multi-folder shared volumes**: Extend `extfs.cpp` beyond its single `RootPath` to
  support N configured volumes. Use the multi-value pref pattern (multiple `extfs` lines, like
  `disk`). ~2–3 days. See Implementation Notes §extfs.
- [ ] **Live folder mount/unmount**: Hook `ExtFSInit()`/`ExtFSExit()` for hot-add/remove via
  UDS RPC.
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
- [ ] **Full execution-state save/restore**: Serialize all CPU/JIT/device state for
  DOSBox-X-style save slots. Very high effort.

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

#### Layer C — "Silicon Sheep Tools" guest agent (managed images only)

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
4. ~~Should Silicon Sheep live as a subdirectory or a separate repo?~~ **Decided:** sibling
   directory at `SiliconSheep/` in this repo. The hard-fork question is deferred but recognized
   as increasingly inevitable.

## Host-Guest Interaction Channels

Full reference in `docs/planning/HOST-GUEST-CHANNELS.md` — documents all existing and
achievable host↔guest channels, readable guest OS structures (low-memory globals, WindowList,
CurApName, MenuList, ScrnBase, etc.), and the hard limits (no arbitrary guest code execution
on demand, no guest-initiated async callbacks, no Toolbox calls outside EmulOp context).
