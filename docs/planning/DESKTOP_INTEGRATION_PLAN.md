# Silicon Sheep — Desktop Integration Feature Plan

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

### Decision: Extend the existing Cocoa/ObjC launcher

**Architecture:** Evolve the existing `AppController.mm` / `VMSettingsController.mm` / `VMListController.mm` Cocoa app in place. Add VM profile management, a first-run wizard, and deeper native integration directly in Objective-C++, calling the same `NSPasteboard`, `NSOpenPanel`, and `NSFileCoordinator` APIs the codebase already uses. No new language runtime, no new process model, no two-window coordination problem. The launcher is built entirely from code (programmatic `NSApplication` setup, no NIBs) so the build requires only Xcode Command Line Tools — no `xcodebuild`, no Xcode.app.

**Why this over alternatives:**

| Option | Verdict |
|---|---|
| **Extend existing Cocoa/ObjC launcher** | **Chosen.** Lowest risk; code already exists and builds. First-class macOS native integration (clipboard, dark mode, Gatekeeper, sandboxing, full-screen). Single window model — launcher and guest display are the same process. Buildable with CLT only via `xcodebuild` or programmatic `NSApplication` + CMake. |
| **Tauri v2** | CLI-buildable, cross-platform. Two-window model (Tauri launcher + SDL guest window) is a real coordination cost. IPC cannot use shared memory on macOS, so framebuffer data can't flow through it. Worthwhile if cross-platform (Linux/Windows) becomes a first-class goal. |
| **SDL3 + Dear ImGui** | Best for an in-process developer overlay (RetroArch style). No separate launcher window. Weakest for first-run wizard UX aimed at non-technical users. |
| **Qt6** | VirtualBox precedent; solid cross-platform. 60–80MB binary, Qt + SDL event loop bridging non-trivial. Only wins if Linux/Windows parity is a first-class goal from day one. |
| **Electron** | No advantage over Tauri; 150–200MB overhead. Skip. |
| **SwiftUI** | Xcode-dependent in practice; locks to Apple platforms. Skip. |

---

## Feature Roadmap

### Tier 1 — First-Run & VM Management (no core changes required)

Pure launcher/wrapper work. SheepShaver binary is a child process; Tauri manages it.

- [ ] **Guided first-run wizard**: ROM file picker with SHA hash verification against known-good ROM hashes, disk image creation UI (size picker + format), OS version selector.
- [ ] **VM profile library**: Named configurations stored as JSON/TOML alongside disk images. Browse, duplicate, delete, rename.
- [ ] **Disk image management**: Drag-and-drop to add images, eject, resize via `hdiutil`/`dd` wrapper, show used/free space.
- [ ] **Preference hot-reload**: Write prefs file and `SIGTERM` + relaunch SheepShaver child process automatically — eliminates the manual quit-relaunch cycle.
- [ ] **Fullscreen escape overlay**: Detect fullscreen state; show macOS-native escape affordance (e.g., hover-reveal top bar with "Exit Fullscreen" button).
- [ ] **Dark mode support**: Built-in prefs UI has no dark mode. Tauri web UI inherits macOS dark/light via CSS `prefers-color-scheme`.
- [ ] **Gatekeeper mitigation**: Bundle with proper signing/notarization workflow; document the copy-to-Desktop workaround in-app if entitlement is missing.

### Tier 2 — Enhanced Desktop Integration (some SheepShaver core changes)

Requires C++ changes to the emulator alongside Tauri frontend work.

- [ ] **Multi-folder shared volumes**: Extend `extfs.cpp` beyond its single `RootPath` to support N configured volumes, each appearing as a separate disk icon in the Finder. Tauri UI adds a "Shared Folders" list with add/remove/path pickers.
- [ ] **Live folder mount/unmount**: Hook into `extfs` volume lifecycle to allow adding/removing shared folders without full restart. Requires an IPC mechanism between the Tauri process and the running emulator (Unix domain socket or shared memory flag).
- [ ] **`utxt`/UT16 clipboard fix**: `clip_macosx64.mm` explicitly skips Unicode scrap types with a "sometime, it might be interesting" comment. Implement the UTF-16 ↔ Mac Roman converter — Carbon apps (AppleWorks, BBEdit) rely on this.
- [ ] **Clipboard status indicator**: Emit clipboard sync events over Tauri IPC so the UI can show "Last synced: text / image" — useful for diagnosing clipboard issues.
- [ ] **Network configuration assistant**: Guided UI for slirp vs. tap networking, port forwarding rules, and AppleTalk configuration — currently requires manual pref key editing.
- [ ] **Display scaling controls**: Expose `mag_rate` as a live slider in the Tauri UI sidebar; write pref + signal SheepShaver to re-scale without full restart.

### Tier 3 — Coherence Lite (significant research, novel work)

No prior art exists for classic Mac OS. Technically feasible but requires deep emulator internals work.

- [ ] **Guest window metadata extraction**: Parse the Mac OS Window Manager's `WindowList` low-memory global (`0x9D6`) via `Mac2HostAddr()` to extract window titles, bounds, and Z-order from guest RAM in real time. SheepShaver has full read access to guest RAM — this is the key insight that makes this tractable at all.
- [ ] **Host title bar overlay**: Using extracted window metadata, draw styled title-bar overlay panels on the host desktop that mirror guest window positions. Does not decompose the framebuffer — just adds a floating chrome layer above the SDL window for a more native feel.
- [ ] **Per-window taskbar/Dock entries**: Use extracted window titles to show individual classic Mac OS windows in the macOS Dock or a custom HUD panel — approximates Parallels-style task switching.
- [ ] **True coherence / seamless windows** *(research project)*: Decompose the guest framebuffer into per-window regions using guest WindowList bounds, composite each region into a separate host `NSWindow`. Requires: window-boundary detection, Z-order compositing, cursor-capture per window, and handling overlapping/occluded windows. Unprecedented in open-source classic Mac OS emulation. Parallels implements this via a guest agent + custom display driver — neither is available for Mac OS 9. Feasibility: low in the near term; high if guest RAM parsing proves reliable.

---

## JIT + GUI: Orthogonal but Complementary

The `macemu-jit` ARM64 JIT project (branch `macos-arm64`) and this GUI frontend are independent efforts that together represent a step-change improvement:

- **GUI frontend** removes the usability barrier for new users.
- **ARM64 JIT** closes the performance gap vs. x86_64 + Rosetta 2 (currently, native ARM64 without JIT is slower than Rosetta 2 with JIT).

Note: the x86_64 build running under Rosetta 2 with JIT enabled benchmarks faster than the native ARM64 build today. Until ARM64 JIT lands, the Tauri launcher should default to launching the x86_64 binary under Rosetta 2 on Apple Silicon with JIT enabled, with a UI toggle for "native ARM64 (no JIT)" for users who prefer it.

---

## What Other Emulators Teach Us

- **QEMU/UTM clipboard**: Uses SPICE `vdagent` over a virtio serial port — requires a modern guest agent. Inapplicable to Mac OS 9.
- **DOSBox-X clipboard**: Timer-based SDL clipboard polling + a virtual `CLIP$` device. SheepShaver's trap-interception approach is strictly superior.
- **Wine `winemac.drv`**: Creates real `NSWindow` objects per Win32 `HWND` — seamless by construction because Wine IS the runtime. Architecturally inapplicable here; SheepShaver is a hardware abstraction layer, not the Mac OS runtime.
- **Parallels Coherence**: Guest-side `prl_hook.dll` + host VMM trap. Requires a guest agent. Not possible for Mac OS 9.
- **Conclusion**: SheepShaver's existing clipboard and volume-sharing implementations are already at or above the state of the art for classic Mac OS emulation. The gap is UX, not core capability.

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
3. In `VMSettingsController.mm`, call `PrefsReloadFromDisk()` after saving instead of requiring a full relaunch.
4. Optional: add a Unix domain socket listener thread so the Cocoa launcher can poke the running emulator directly.

No `SIGHUP` handler exists today; no existing IPC mechanism to exploit.

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

### Build System

**No Xcode.app required.** The canonical build is Autotools:

```bash
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video --enable-sdl-audio --with-sdl2 \
            --disable-vosf --with-mon --enable-addressing=banks
make -j$(sysctl -n hw.logicalcpu)
```

Dependencies: `brew install sdl2 autoconf automake libtool` — that's it.

The existing Xcode projects (`SheepShaverLauncher.xcodeproj`) are being replaced by a programmatic Cocoa build. The launcher app bundle is assembled manually: compile the `.mm`/`.cpp` sources with `clang`, link against `Cocoa.framework`, and `cp` the Unix `SheepShaver` binary in as an auxiliary executable. No `xcodebuild` involved.

App bundle structure: `SheepShaver.app/Contents/MacOS/SheepShaver` + `Resources/SheepShaver.icns` + `Info.plist`.

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

1. For Tier 3 coherence work: is `Mac2HostAddr()` + WindowList parsing reliable enough across OS versions (System 7 through 9.0.4) to build on?
2. Should `extfs` multi-volume use the existing multi-value pref pattern (multiple `extfs` lines, same as how multiple `disk` entries work) or a new numbered key scheme (`extfs`, `extfs2`, …)? The multi-value pattern is the path of least resistance and consistent with the rest of the prefs file.
3. Should Silicon Sheep live as a subdirectory in this repo or as a separate repo that pulls in macemu as a submodule?
