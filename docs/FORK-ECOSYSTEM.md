# macemu Fork Ecosystem Survey

_Last surveyed: 2026-06-09. Covers the kanjitalk755/macemu and cebix/macemu fork networks,
focusing on changes that may be relevant to this macOS arm64 port._

---

## Attribution policy

When we integrate code from a fork, the commit message must include:
1. A plain-text credit line: `Source: user/repo branch-or-description`
2. The **full GitHub URL** to the originating commit or branch
3. If the change was also merged upstream into kanjitalk755/macemu, a second line:
   `Upstream merge: kanjitalk755/macemu@<sha>` with the full URL

Example (from `b3d61f1d`):
```
Source: robxnano/macemu keyboard-grab branch
  https://github.com/robxnano/macemu/tree/keyboard-grab
Upstream merge: kanjitalk755/macemu@e2a210ef
  https://github.com/kanjitalk755/macemu/commit/e2a210ef3d7e6bf8d78323570f8c3b3ba4f8c037
```

### Integrated so far

| Our commit | Source | Upstream |
|------------|--------|----------|
| [`c661ee90`](https://github.com/khawkins98/macemu-jit/commit/c661ee90c7bec1c14b4ce7df8e7af87e77a1e01c) — keyboard grab tracks mouse grab state | [robxnano keyboard-grab branch](https://github.com/robxnano/macemu/tree/keyboard-grab) | [kanjitalk755@e2a210ef](https://github.com/kanjitalk755/macemu/commit/e2a210ef3d7e6bf8d78323570f8c3b3ba4f8c037) |

---

## Fork lineage

```text
cebix/macemu
├─ kanjitalk755/macemu
│  ├─ rcarmo/macemu-jit
│  │  └─ khawkins98/macemu-jit  ← this repo
│  ├─ MatthiasWM/kanjitalk_macemu
│  ├─ AndrewNile/macemu
│  ├─ amcchord/macemu
│  ├─ eyeonpower/macemu
│  ├─ quentinmit/macemu
│  ├─ sirmick/macemu (= realhidden/macemu)
│  ├─ mihaip/macemu
│  ├─ vaccinemedia/macemu
│  ├─ Cronocide/macemu
│  ├─ siddhartha77/macemu
│  ├─ audiocontrol-org/macemu
│  ├─ mstevenson/macemu
│  ├─ DMJC/macemu
│  └─ many plain mirrors / catch-up forks
├─ zydeco/macemu
├─ jsdf/macemu
├─ DavidLudwig/macemu
├─ uyjulian/macemu
├─ ucosty/macemu
├─ seanmadawala/macemu
├─ kwhr0/macemu
└─ vasi/macemu
```

**Reading the tree:** most currently relevant active work lives in the **kanjitalk755** fork network.
The **cebix-direct** forks are more heterogeneous: some are older macOS/iOS/browser experiments,
some are Linux portability branches, and a few are still useful as idea mines.

---

## How to read this

Priority ratings below are subjective relevance to this repo's goals: **macOS arm64,
JIT, SDL3, networking, launcher/UI integration, and portability work that may carry over**.
The summary table is the quick answer to _"what is each fork really trying to do?"_; the
sections below it contain commit-level detail.

### Status legend

| Symbol | Meaning |
|--------|---------|
| ❓ | **Pending** — spotted, not yet investigated or decided |
| ✅ | **Integrated** — cherry-picked into this repo (see [Integrated so far](#integrated-so-far)) |
| ❌ | **Rejected** — reviewed and decided not to integrate (reason noted inline) |

### At-a-glance: what each fork is trying to do

| Fork | Primary goal / theme | What it's really trying to do | Status |
|------|----------------------|-------------------------------|--------|
| [MatthiasWM/kanjitalk_macemu](https://github.com/MatthiasWM/kanjitalk_macemu) | macOS linker fix | Keep kanjitalk's Unix build working on Darwin by avoiding `--export-dynamic` on macOS. | ❓ |
| [eyeonpower/macemu](https://github.com/eyeonpower/macemu) / [amcchord/macemu](https://github.com/amcchord/macemu) | memory layout + SDL3 fixes | Stabilize BasiliskII/SDL paths on newer hosts with contiguous allocation and SDL3 fixes. | ❓ |
| [AndrewNile/macemu](https://github.com/AndrewNile/macemu) | larger VM allocation refactor | Rework memory acquisition more broadly than eyeonpower's BasiliskII-only patch. | ❓ |
| [Cronocide/macemu](https://github.com/Cronocide/macemu) | parallel SheepShaver ARM64 JIT bring-up | Add an AArch64 JIT backend and patch resulting freezes/crashes in upstream SheepShaver. | ❓ |
| [zydeco/macemu](https://github.com/zydeco/macemu) | macOS-native rootless/Xcode build | Make BasiliskII feel like a native macOS app: rootless desktop windows, Xcode/Homebrew fixes, static deps. | ❓ |
| [quentinmit/macemu](https://github.com/quentinmit/macemu) | VDE networking | Make SheepShaver networking usable with VDE and persistent pref-driven configuration. | ❓ |
| [audiocontrol-org/macemu](https://github.com/audiocontrol-org/macemu) | networked SCSI bridge lab | Drive external hardware / OS 9 workflows via a `scsi2pi` backend, Docker automation, and deep device-manager tracing. | ❓ |
| [sirmick/macemu](https://github.com/sirmick/macemu) | Apple Silicon headless/browser streaming | Replace SDL-centric UI assumptions with IPC video and built-in web streaming. | ❓ |
| [mihaip/macemu](https://github.com/mihaip/macemu) | browser/Emscripten port (Infinite Mac) | Turn BasiliskII/SheepShaver into browser-delivered emulators and polish web-specific UX/media paths. | ❓ |
| [mstevenson/macemu](https://github.com/mstevenson/macemu) | macOS window/fullscreen polish | Improve the macOS window model around fullscreen, resizing, magnification, and default disk discovery. | ❓ |
| [robxnano/macemu](https://github.com/robxnano/macemu) | build-system + prefs-dir modernization | Prototype Meson/macOS builds, a Qt 6 SheepShaver prefs dialog, and more native config-file locations (XDG/Application Support/AppData). | ✅ (keyboard-grab) |
| [SegHaxx/macemu-flatpak](https://github.com/SegHaxx/macemu-flatpak) | Flatpak/distribution modernization | Package macemu for Flatpak and strip away legacy desktop assumptions with XDG support, 64-bit cleanup, and old-backend removals. | ❓ |
| [andyvand/macemu](https://github.com/andyvand/macemu) | Android host port | Carry BasiliskII toward Android/guisan while also testing a few macOS/SDL3 display tweaks. | ❓ |
| [timothy-fuchs/macemu](https://github.com/timothy-fuchs/macemu) | DPI LCD rotation tweak | Add a small-screen/Waveshare rotation pref in BasiliskII's SDL2 video path. | ❓ |
| [vaccinemedia/macemu](https://github.com/vaccinemedia/macemu) | GTK/SDL UX polish | Improve Linux prefs UX with scaling controls and better drag-and-drop CD-ROM behavior. | ❓ |
| [seanmadawala/macemu](https://github.com/seanmadawala/macemu) | GTK4 + PulseAudio modernization | Update the old Linux desktop stack to current GUI/audio APIs. | ❓ |
| [jrepp/macemu](https://github.com/jrepp/macemu) / [vasi/macemu](https://github.com/vasi/macemu) | portability hygiene | Carry small but valuable build, linker, Wayland, and signal-handler fixes across modern hosts. | ❓ |
| [siddhartha77/macemu](https://github.com/siddhartha77/macemu) | guest/host clock decoupling | Reduce host-time coupling after startup, mainly for deterministic runtime behavior. | ❓ |
| [jsdf/macemu](https://github.com/jsdf/macemu) | original browser BasiliskII port | Early Emscripten proof that later fed into the Infinite Mac line of work. | ❓ |
| [DavidLudwig/macemu](https://github.com/DavidLudwig/macemu) | macOS SDL2 performance tuning | Tune pixel formats, rendering, and VOSF behavior for OSX-era SDL2 builds. | ❓ |
| [kwhr0/macemu](https://github.com/kwhr0/macemu) | alternative interpreter cores | Experiment with TinyPPC/Tiny68020 and maintain Xcode-native macOS builds. | ❓ |
| [uyjulian/macemu](https://github.com/uyjulian/macemu) | macOS-only BasiliskII simplification | Strip BasiliskII down to a macOS-only/CMake target and try an ARAnyM-derived 68K JIT path. | ❓ |
| [ucosty/macemu](https://github.com/ucosty/macemu) | guest-side debug hooks | Add tiny debug conveniences like an emulated `putchar` instruction. | ❓ |
| [DMJC/macemu](https://github.com/DMJC/macemu) | joystick → ADB input | Add gamepad support for BasiliskII by mapping SDL joystick input onto ADB. | ❓ |

---

## 🔴 High Priority — macOS arm64 / build / memory / JIT

### [MatthiasWM/kanjitalk_macemu](https://github.com/MatthiasWM/kanjitalk_macemu) — `--export-dynamic` macOS linker fix
- **Status**: ❓
- **Commit**: [`964e8127d6c1`](https://github.com/MatthiasWM/kanjitalk_macemu/commit/964e8127d6c1d584edf4d19fa4ad248b6ebe6d7d) (2026-03-25)
- **Merged**: into kanjitalk755 as [PR #296](https://github.com/kanjitalk755/macemu/pull/296) on 2026-03-26
- Fix: conditionally adds `--export-dynamic` only on non-Darwin hosts (macOS linker rejects it)
- Applies to both `BasiliskII/src/Unix/configure.ac` and `SheepShaver/src/Unix/configure.ac`

### [eyeonpower/macemu](https://github.com/eyeonpower/macemu) / [amcchord/macemu](https://github.com/amcchord/macemu) — contiguous RAM/ROM/scratch allocation (BasiliskII)
- **Status**: ❓
- **Commit**: [`2d388f768246`](https://github.com/eyeonpower/macemu/commit/2d388f768246180aa4263c5c50d511fa13488f55) (2025-12-27)
  - Also present in [amcchord @ `2d388f768246`](https://github.com/amcchord/macemu/commit/2d388f768246180aa4263c5c50d511fa13488f55)
- Allocates RAM, ROM, and scratch memory as one contiguous block in `BasiliskII/src/Unix/main_unix.cpp`
- Relevant to NATMEM / DIRECT_ADDRESSING on arm64 where virtual address layout matters
- Small, surgical patch; easy to audit

### [AndrewNile/macemu](https://github.com/AndrewNile/macemu) — bulk memory acquisition (SheepShaver + `vm_alloc`)
- **Status**: ❓
- **Commit**: [`94a9f6cc229f`](https://github.com/AndrewNile/macemu/commit/94a9f6cc229f6436b60850484e681f770c0f0080) (2025-07-19)
- "Acquire memory in bulk" — touches `BasiliskII/src/CrossPlatform/vm_alloc.cpp`,
  `SheepShaver/src/Unix/main_unix.cpp`, `SheepShaver/src/Unix/sysdeps.h`, Windows glue,
  and kpx_cpu `vm.hpp`
- Broader refactor than eyeonpower's BasiliskII-only version; worth comparing for memory ownership and fallback behavior
- AndrewNile is actively rebasing/merging from kanjitalk755, so this fork is a decent "maintained delta" reference

### [Cronocide/macemu](https://github.com/Cronocide/macemu) — parallel SheepShaver AArch64 JIT bring-up
- **Status**: ❓
- **Commits**:
  - [`e110c93db22b`](https://github.com/Cronocide/macemu/commit/e110c93db22b7e8b06a469a15376bc4d78adeee5) (2026-03-16) — add AArch64 JIT backend support to SheepShaver
  - [`ad80ac805bf0`](https://github.com/Cronocide/macemu/commit/ad80ac805bf0935e156684f69ed123a85af44ea2) (2026-03-19) — fix freezes and crashes
- This is the most directly relevant "someone else also tried ARM64 SheepShaver JIT" fork in the kanjitalk tree
- Files touched include `ppc-jit.cpp`, `ppc-cpu.cpp`, `vm.hpp`, AArch64 dyngen headers, `jit-cache.*`, and `configure.ac`
- Likely useful as a **parallel portability/design reference**, even if this repo's hand-written ARM64 JIT has moved further

### [amcchord/macemu](https://github.com/amcchord/macemu) — SDL3 `UnlockTexture` fix
- **Status**: ❓
- **Commit**: [`e596e21583e4`](https://github.com/amcchord/macemu/commit/e596e21583e488d8997f71e3b8509eb51792977e) (2026-01-31)
- "SDL3: Fixed so that blit is not required in `SDL_UnlockTexture()`"
- We use SDL3 by default for SheepShaver, so this is still worth checking against our SDL3 path

### [zydeco/macemu](https://github.com/zydeco/macemu) — macOS aarch64 static link + Xcode build
- **Status**: ❓
- **Branch**: [`rootless`](https://github.com/zydeco/macemu/tree/rootless) (last notable macOS activity 2024-03)
- Notable commits:
  - [`841a7cc04820`](https://github.com/zydeco/macemu/commit/841a7cc048203d52ee84b63eaeb15b59e73f1d68) — rootless: only redraw mask once on show-desktop
  - [`7024200a4970`](https://github.com/zydeco/macemu/commit/7024200a49707090f8e21ba842ab0347de9b28c3) — **macOS/aarch64: statically link gmp and mpfr**
  - [`dbf51450817b`](https://github.com/zydeco/macemu/commit/dbf51450817b88c1d1a31a7bd8ca751637eca263) — xcode: add Homebrew library and header search paths
- Strongest macOS-native fork outside the kanjitalk tree: rootless desktop windows, Xcode project work, and macOS packaging instincts
- zydeco also maintains [`sheepshaver_ios`](https://github.com/zydeco/macemu/tree/sheepshaver_ios)

---

## 🟠 Medium-High Priority — networking / alternative host-guest plumbing

### [quentinmit/macemu](https://github.com/quentinmit/macemu) — VDE support for SheepShaver
- **Status**: ❓
- **Commit**: [`06d8bc02631b`](https://github.com/quentinmit/macemu/commit/06d8bc02631b14a023ca29b1bd85bb1c129755db) (2026-05-06)
- Fixes and extends VDE support in SheepShaver:
  - packets now have correct length (no trailing garbage)
  - adds VDE to SheepShaver's `configure.ac`
  - VDE destination link configurable as a pref (survives saves)
  - example: `sheepshaver --ether 'vde:cmd://ssh root@server vde_plug tap://tap0'`
- We already ship VDE networking — this is directly relevant polish
- Also has an [`ss-vde` branch](https://github.com/quentinmit/macemu/tree/ss-vde) with further VDE work
- **[sirmick/macemu](https://github.com/sirmick/macemu/tree/ss-vde)** also has an `ss-vde` branch — likely related or derived

### [audiocontrol-org/macemu](https://github.com/audiocontrol-org/macemu) — network SCSI bridge + automated OS 9 lab
- **Status**: ❓
- **Branches checked**:
  - [`feature/scsi-network-bridge`](https://github.com/audiocontrol-org/macemu/tree/feature/scsi-network-bridge) — 7 commits ahead of `master`
  - [`os9-minimal`](https://github.com/audiocontrol-org/macemu/tree/os9-minimal) — 68 commits ahead of `master`
- Key commits on `feature/scsi-network-bridge`:
  - [`8161a6b6ce9b`](https://github.com/audiocontrol-org/macemu/commit/8161a6b6ce9b6f47e9d4c38fd0b4203bb06a7b47) — `scsi2pi` network SCSI backend for SheepShaver
  - [`b42d0fe2b3a8`](https://github.com/audiocontrol-org/macemu/commit/b42d0fe2b3a850fd76b5e3303ade797a838ef50b) — add `--enable-scsi-s2p`
  - [`441b6e103d63`](https://github.com/audiocontrol-org/macemu/commit/441b6e103d63746ee79f4fcc7fbd41c01f12cb79) / [`a87648371cef`](https://github.com/audiocontrol-org/macemu/commit/a87648371ceff45fae29663b7b7290c8e5d806a7) — Docker automation for booting/testing OS 9 with the bridge
- `os9-minimal` goes much further: Device Manager / Mixed Mode / ROM patch investigation, MESA II probing, and documentation-heavy tracing
- Not directly VDE-related, but very interesting as an **alternative host↔guest I/O strategy** and as an example of emulator-assisted automation work

---

## 🟡 Medium Priority — display / input / UI / browser-facing work

### [sirmick/macemu](https://github.com/sirmick/macemu) (= [realhidden/macemu](https://github.com/realhidden/macemu)) — IPC video + web streaming for Apple Silicon
- **Status**: ❓
- **Branches**: [`master`](https://github.com/sirmick/macemu/tree/master) / [`tutorial-m1-mac`](https://github.com/sirmick/macemu/tree/tutorial-m1-mac) (2025-12 to 2026-01)
- Adds a full **web-streaming video backend** for SheepShaver:
  - IPC-based video capture piped to a built-in HTTP server
  - codecs: AV1 (SVT-AV1), WebP, unreliable DataChannel/WebRTC-style low-latency path
  - JSON config, ROM/disk management UI, mouse mode persistence
- Key commits:
  - [`30e7e01e6017`](https://github.com/sirmick/macemu/commit/30e7e01e60172a58f9e15e6e3c8c898a22f42b46) — fix IPC video crash on macOS with `DIRECT_ADDRESSING`
  - [`d9317e12a933`](https://github.com/sirmick/macemu/commit/d9317e12a933dee7e10b9148a5f6e3d7f5ff2bf6) — fix duplicate keystrokes
  - [`b237a2f3b686`](https://github.com/sirmick/macemu/commit/b237a2f3b6862702c7fd5eb184b83dda727d8280) — fix mouse input + codec reload
- Most relevant fork if we ever want a browser-accessible/headless display path for Silicon Sheep

### [mihaip/macemu](https://github.com/mihaip/macemu) — Infinite Mac / Emscripten line, plus UX/media work
- **Status**: ❓
- **Default branch**: [`infinite-mac-kanjitalk755`](https://github.com/mihaip/macemu/tree/infinite-mac-kanjitalk755)
- **Older branches checked**: `bas-emscripten-release`, `bas-emscripten-mainthread`, `bas-singlethread-release`, `infinite-mac-jsdf`, `master`
- This is the most important **browser-delivery** fork in the kanjitalk tree
- Notable commits on the modern line:
  - [`7aabc9801a04`](https://github.com/mihaip/macemu/commit/7aabc9801a0483c10cc6976858250c1feda1d783) — relative mouse delta input
  - [`260cab139958`](https://github.com/mihaip/macemu/commit/260cab139958b79b403f112bd1ae0e8b53572396) — removable non-CD media
  - [`1736f17f141e`](https://github.com/mihaip/macemu/commit/1736f17f141eaef339b3791055eb110c197204a7) / [`b7b5cb4a102c`](https://github.com/mihaip/macemu/commit/b7b5cb4a102c3cd9054f2667a6e4e71194a569d3) — MacJapanese clipboard / Script Manager-aware ExtFS conversions
  - [`683bc641a20e`](https://github.com/mihaip/macemu/commit/683bc641a20ece452e2dd45900a43655925cbb11) — report emulated instruction count to JS side
- The older `bas-emscripten-*` branches are clearly descended from the earlier jsdf browser work, but the current branch is the one to care about

### [mstevenson/macemu](https://github.com/mstevenson/macemu) — macOS window/fullscreen UX polish
- **Status**: ❓
- **Branches**:
  - [`standardize-fullscreen-shortcut`](https://github.com/mstevenson/macemu/tree/standardize-fullscreen-shortcut) — 8 commits ahead
  - [`close-button-exit`](https://github.com/mstevenson/macemu/tree/close-button-exit) — 1 commit ahead
  - [`default-disk-images`](https://github.com/mstevenson/macemu/tree/default-disk-images) — 1 commit ahead
- Key commits:
  - [`0f76097fece6`](https://github.com/mstevenson/macemu/commit/0f76097fece6cd52eb675cf3177dba5ae1ca9d84) — Fn+F fullscreen shortcut on macOS
  - [`fb0ad3e99ad8`](https://github.com/mstevenson/macemu/commit/fb0ad3e99ad8852b1bdb7eca45582d058942a29d) — enable fullscreen button + resize
  - [`f5feff34c362`](https://github.com/mstevenson/macemu/commit/f5feff34c362a34c5bcf25be48acc67ac0663d88) — magnification menu options
  - [`4cf1022e6f0b`](https://github.com/mstevenson/macemu/commit/4cf1022e6f0b0e59577d34b83629d01a1709c543) — search current directory for disk images
- Touches mostly `BasiliskII/src/MacOSX/utils_macosx.*` and SDL video backends
- Relevant if we want more native-feeling window behavior on macOS, even though this is BasiliskII-focused

### [vaccinemedia/macemu](https://github.com/vaccinemedia/macemu) — GTK UI scaling + placeholder CD-ROM
- **Status**: ❓
- **Branches checked**: only `master` exists as of 2026-06-09
- Commits:
  - [`666797fdee43`](https://github.com/vaccinemedia/macemu/commit/666797fdee43eb681131959685af3e64a2a30a49) — GTK3 SDL scaling settings
  - [`678fffa5f51c`](https://github.com/vaccinemedia/macemu/commit/678fffa5f51c688ac27fa9eb76a38cb678b28b6f) — GTK2 SDL scaling settings
  - [`aa77d0269fa6`](https://github.com/vaccinemedia/macemu/commit/aa77d0269fa6b55f8d36ded48fe26564d0cda398) — placeholder CD-ROM for drag-and-drop
  - [`3f46296a36af`](https://github.com/vaccinemedia/macemu/commit/3f46296a36af20ed4083bfd846e4fb4d3639e5c9) — port BasiliskII GTK2 scaling work into SheepShaver
- Linux/GTK focused, but still useful UI polish reference

### [seanmadawala/macemu](https://github.com/seanmadawala/macemu) — GTK4 + PulseAudio port
- **Status**: ❓
- **Commit**: [`67ce9a297e5a`](https://github.com/seanmadawala/macemu/commit/67ce9a297e5aec805ec6e5103542a8f3a5b7d934) (2026-03-05)
- Ports SheepShaver prefs editor to GTK4 and adds a PulseAudio Simple API backend
- Modernizes Linux desktop integration rather than macOS, but the prefs-editor cleanup is conceptually useful

### [DavidLudwig/macemu](https://github.com/DavidLudwig/macemu) (cebix fork) — macOS SDL2 performance + render knobs
- **Status**: ❓
- Notable commits:
  - [`50986dcf467f`](https://github.com/DavidLudwig/macemu/commit/50986dcf467f793aec7e3ec2005d1f0c7b64758e) — use ARGB8888 texture to avoid pixel-format conversion on OSX GPUs/drivers
  - [`ef26204e6d6b`](https://github.com/DavidLudwig/macemu/commit/ef26204e6d6b19378d27ef0baf91adeac729d671) — reduce pixel updates in SDL2 backend
  - [`449936e461ac`](https://github.com/DavidLudwig/macemu/commit/449936e461ac15325453cf09c892d41c8e0fec24) — add `--sdlrender` option in SheepShaver Unix prefs path
  - [`4e5e3377f1cf`](https://github.com/DavidLudwig/macemu/commit/4e5e3377f1cfead70e31170a644f1158fe7d35ce) — re-enable VOSF on Xcode-made OSX builds
- Old (2017), but still one of the clearest macOS/SDL-specific optimization forks in the cebix tree

### [jsdf/macemu](https://github.com/jsdf/macemu) (cebix fork) — original browser BasiliskII port
- **Status**: ❓
- **Default branch**: [`bas-emscripten-release`](https://github.com/jsdf/macemu/tree/bas-emscripten-release)
- Key branch set: `bas-emscripten-release`, `bas-emscripten-mainthread`, `bas-singlethread-release`
- Foundational commit: [`1730d17db9e8`](https://github.com/jsdf/macemu/commit/1730d17db9e8f3832397e91ce2a43a43aaccfeae) — Emscripten port of BasiliskII
- This is mostly historical now, but it is the clearest **ancestor branch** for the later Infinite Mac line

---

## 🔵 Build / portability / runtime behavior

### [jrepp/macemu](https://github.com/jrepp/macemu) (merged from [vasi/archpower](https://github.com/vasi/macemu/tree/archpower))
- **Status**: ❓
- **Merged via**: [kanjitalk755 PR #302](https://github.com/kanjitalk755/macemu/pull/302) (2026-05)
- Commits:
  - [`aff612c41b5f`](https://github.com/jrepp/macemu/commit/aff612c41b5f17faad3391ad045f6cb17666e135) — fix incorrect `printf` / `snprintf` use
  - [`7d8a9fe19f77`](https://github.com/jrepp/macemu/commit/7d8a9fe19f779dcef096888d9d42849feb0adf92) — disable stack protection for PPC signal handlers
  - [`26fffb6a0eaf`](https://github.com/jrepp/macemu/commit/26fffb6a0eafd621b4b056218d8efcfd7b75d292) — only use `SDL_VERSION_ATLEAST` if building with SDL
  - [`533cf6faa3eb`](https://github.com/jrepp/macemu/commit/533cf6faa3eb71596f1748d4cec03ce3dd154020) — adjust reads to be kernel-relative
  - [`f95dc6559337`](https://github.com/jrepp/macemu/commit/f95dc6559337f97fc2798c633317e3c192df6cc2) — fix `loff_t` detection
  - [`6787dce83205`](https://github.com/jrepp/macemu/commit/6787dce832056b39c1d52f4754d4709ca834c744) — add `linux/sched.h` for `CLONE_VM`
  - [`b4c6e9139599`](https://github.com/jrepp/macemu/commit/b4c6e9139599d3973be24f475082886783db472c) — fix linker script check (BII)
- Good source of small modern-host hygiene fixes; `vasi/archpower` remains the broader branch family behind these

### [siddhartha77/macemu](https://github.com/siddhartha77/macemu) — decouple clock from host after startup
- **Status**: ❓
- Diverges from kanjitalk by only two commits, but both are conceptually interesting:
  - [`7f367cf8eb62`](https://github.com/siddhartha77/macemu/commit/7f367cf8eb62a3d981bc03012e58eb08531656f8) — decouple clock from host after startup
  - [`ea2057210ccd`](https://github.com/siddhartha77/macemu/commit/ea2057210ccd90dd8867134a74636a7daae3c47a) — decouple clock from host for BII
- Touches `rom_patches.cpp`, `emul_op.cpp`, and Windows main paths
- Not macOS-specific, but worth remembering if guest-time coupling becomes a problem

### [robxnano/macemu](https://github.com/robxnano/macemu) — Meson/Qt6/prefs-dir modernization
- **Status**: ✅ `keyboard-grab` branch integrated as [`c661ee90`](https://github.com/khawkins98/macemu-jit/commit/c661ee90c7bec1c14b4ce7df8e7af87e77a1e01c); remaining branches (`meson`, `qt6`, `prefs-dir`) still ❓
- **Interesting branches**:
  - [`qt6`](https://github.com/robxnano/macemu/tree/qt6) — 8 commits ahead of kanjitalk `master`
  - [`meson`](https://github.com/robxnano/macemu/tree/meson) — 6 commits ahead
  - [`prefs-dir`](https://github.com/robxnano/macemu/tree/prefs-dir) — 10 commits ahead
  - [`keyboard-grab`](https://github.com/robxnano/macemu/tree/keyboard-grab) — 1 commit ahead
  - [`x11-no-threads`](https://github.com/robxnano/macemu/tree/x11-no-threads) exists, but is now fully upstreamed / empty relative to current kanjitalk `master`
- Key commits:
  - [`bd54ccd8`](https://github.com/robxnano/macemu/commit/bd54ccd8) — initial Meson build support
  - [`3564d7a6`](https://github.com/robxnano/macemu/commit/3564d7a6) — add macOS support to the Meson line
  - [`92737114`](https://github.com/robxnano/macemu/commit/92737114) — add a Qt 6 SheepShaver prefs dialog
  - [`27616fd3`](https://github.com/robxnano/macemu/commit/27616fd3) / [`d2c414da`](https://github.com/robxnano/macemu/commit/d2c414da) / [`658a93fb`](https://github.com/robxnano/macemu/commit/658a93fb) — move prefs/XPRAM locations toward XDG, macOS Application Support, and Windows AppData
  - [`e2a210ef`](https://github.com/robxnano/macemu/commit/e2a210ef) — suppress host keyboard shortcuts while mouse grab is active
- Best fork in this batch for **desktop-plumbing modernization**: alternative build-system work, more native per-OS config locations, and a non-GTK prefs UI path

### [SegHaxx/macemu-flatpak](https://github.com/SegHaxx/macemu-flatpak) (cebix fork) — Flatpak packaging + de-legacy-ing the desktop stack
- **Status**: ❓
- **Branches checked**: [`master`](https://github.com/SegHaxx/macemu-flatpak/tree/master), [`flatpak`](https://github.com/SegHaxx/macemu-flatpak/tree/flatpak)
- Key commits:
  - [`617c4041`](https://github.com/SegHaxx/macemu-flatpak/commit/617c4041) — implement XDG Base Directory Specification
  - [`4a7cb4b3`](https://github.com/SegHaxx/macemu-flatpak/commit/4a7cb4b3) — consolidate m68k memory map allocation
  - [`b46e3703`](https://github.com/SegHaxx/macemu-flatpak/commit/b46e3703) — make Basilisk II mostly 64-bit clean
  - [`655e8664`](https://github.com/SegHaxx/macemu-flatpak/commit/655e8664) / [`fcea98c9`](https://github.com/SegHaxx/macemu-flatpak/commit/fcea98c9) / [`e3c1bbb4`](https://github.com/SegHaxx/macemu-flatpak/commit/e3c1bbb4) — remove SDL1, GTK1, and other legacy APIs
  - [`7a6f82f7`](https://github.com/SegHaxx/macemu-flatpak/commit/7a6f82f7) — derive version info from git tags
- This branch family is **huge** (hundreds of commits ahead of cebix), so it is better treated as an idea mine than a merge target
- Most relevant themes here are packaging/distribution, config-path cleanup, and aggressively dropping old host-backend assumptions

---

## 🟢 Niche / specific-interest forks

### [timothy-fuchs/macemu](https://github.com/timothy-fuchs/macemu) — Waveshare / small-LCD rotation parameter
- **Status**: ❓
- **Branches**: [`master`](https://github.com/timothy-fuchs/macemu/tree/master), [`screen_rotation_param_for_waveshare_dpi_lcd_support`](https://github.com/timothy-fuchs/macemu/tree/screen_rotation_param_for_waveshare_dpi_lcd_support), [`waveshare_2.8_inch_dpi_lcd_support`](https://github.com/timothy-fuchs/macemu/tree/waveshare_2.8_inch_dpi_lcd_support)
- Unique commit on top of kanjitalk `master`:
  - [`254ef18c`](https://github.com/timothy-fuchs/macemu/commit/254ef18c) — add a rotation parameter for Waveshare DPI LCD panels
- Touches only `BasiliskII/src/SDL/video_sdl2.cpp` and `BasiliskII/src/prefs_items.cpp`
- Very hardware-specific, but worth noting if handheld / tiny-display BasiliskII deployments ever matter

### [andyvand/macemu](https://github.com/andyvand/macemu) — Android + guisan host port experiment
- **Status**: ❓
- Diverges from kanjitalk by five commits, but they are substantial:
  - [`9c4802ce`](https://github.com/andyvand/macemu/commit/9c4802ce) — add support for guisan
  - [`4dfb7bdd`](https://github.com/andyvand/macemu/commit/4dfb7bdd) — add Android support
  - [`07a0c446`](https://github.com/andyvand/macemu/commit/07a0c446) — fix Android build
  - [`33cacbc1`](https://github.com/andyvand/macemu/commit/33cacbc1) — update Android project
  - plus upstream-derived SDL3/macOS display work such as [`bc5544e5`](https://github.com/andyvand/macemu/commit/bc5544e5) (`VIDEO_CHROMAKEY`)
- The diff is noisy because it checks in an Android project and bundled app/framework artifacts, but it is the clearest **Android-oriented** fork in this batch
- Relevant mainly as a host-port/reference curiosity rather than a direct input to this macOS arm64 fork

### [kwhr0/macemu](https://github.com/kwhr0/macemu) — TinyPPC + Tiny68020 alternative interpreters (macOS native)
- **Status**: ❓
- Commits:
  - [`bfb142928ea6`](https://github.com/kwhr0/macemu/commit/bfb142928ea65a05497fd2a58c007169a72514d5) — Sonoma compatibility fixes
  - [`98682169c6f5`](https://github.com/kwhr0/macemu/commit/98682169c6f5b2d8135cf407ec23b5ad65426bfc) — Xcode10 build fix
  - [`a8f99a75414e`](https://github.com/kwhr0/macemu/commit/a8f99a75414eb357291ba875edd3df1f9e78bd30) — refactor flags
- Also carries TinyPPC / Tiny68020 alternative CPU backends
- Best viewed as an **alternative interpreter/reference-core** fork, not a JIT fork

### [uyjulian/macemu](https://github.com/uyjulian/macemu) (cebix fork) — macOS-only BasiliskII + ARAnyM JIT experiment
- **Status**: ❓
- Interesting branches:
  - [`core_cleanup`](https://github.com/uyjulian/macemu/tree/core_cleanup) — remove non-macOS targets, convert BasiliskII to CMake, drop SheepShaver/cxmon from focus
  - [`kanjitalk755_aranym_wip`](https://github.com/uyjulian/macemu/tree/kanjitalk755_aranym_wip) — very large experimental line with ARAnyM-derived JIT/compiler work
- Key commits:
  - [`76d285a6f2e0`](https://github.com/uyjulian/macemu/commit/76d285a6f2e0080082c09d4c670c6d85401d71e4) — convert buildsystem to CMake
  - [`77e20bda2abe`](https://github.com/uyjulian/macemu/commit/77e20bda2abeebc62646c0674daddaa8192eeb50) — back to BasiliskII `uae_cpu` but with ARAnyM JIT
- Very experimental and stale, but potentially interesting for future BasiliskII JIT archaeology on macOS

### [ucosty/macemu](https://github.com/ucosty/macemu) (cebix fork) — debug `putchar` instruction
- **Status**: ❓
- Compare view: [cebix master vs ucosty master](https://github.com/cebix/macemu/compare/master...ucosty:macemu:master)
- Unique commits:
  - [`d4ab5504ba62`](https://github.com/ucosty/macemu/commit/d4ab5504ba62723239cc569bc0017dd8dc28959a) — add `putchar` emulated instruction for debugging
  - [`4ec8d1022f58`](https://github.com/ucosty/macemu/commit/4ec8d1022f58c6c5c733955c1fae7eb4c166421e) — include `stdlib.h`
- Minimal delta, but the debug op is genuinely handy

### [DMJC/macemu](https://github.com/DMJC/macemu) — SDL joystick mapped to ADB (BasiliskII)
- **Status**: ❓
- **Branch**: [`codex/update-basilisk-2-for-adb-joystick-support`](https://github.com/DMJC/macemu/tree/codex/update-basilisk-2-for-adb-joystick-support)
- Key commits:
  - [`5d0f131edbd5`](https://github.com/DMJC/macemu/commit/5d0f131edbd5693b8f14ef56de41300107b9b612) — add SDL2 joystick support mapped to ADB
  - [`5cedcccfe1e7`](https://github.com/DMJC/macemu/commit/5cedcccfe1e71824fbc310bdab3db3daddf140c6) — GTK3 prefs controls for ADB joystick prefs
- Niche, but useful if game-controller support ever matters

---

## Forks checked and found low-relevance / mostly mirror-like

| Fork / branch | Result | Why it stayed out of the main sections |
|---------------|--------|----------------------------------------|
| [thetrung/macemu](https://github.com/thetrung/macemu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master` |
| [ToTsRETRO/macemu](https://github.com/ToTsRETRO/macemu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master` |
| [zCubed3/macemu](https://github.com/zCubed3/macemu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master` |
| [Link4Electronics/macemu](https://github.com/Link4Electronics/macemu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master`; just tracks the 2025–2026 kanjitalk line |
| [ldgarcia/macemu](https://github.com/ldgarcia/macemu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master`; same recent commit set as Link4Electronics |
| [xp5-org/macemu_xp5 `freebsd-timer-thread-fix`](https://github.com/xp5-org/macemu_xp5/tree/freebsd-timer-thread-fix) | upstreamed staging branch | The FreeBSD timer-thread fix was merged into kanjitalk; no remaining delta against current `master` |
| [clementgallet/macemu](https://github.com/clementgallet/macemu) | behind-only kanjitalk tracker | Interesting owner, but no unique commits ahead of kanjitalk `master`; `always-update` is just a catch-up branch |
| [PegLeg-Computer/macemu-rootless](https://github.com/PegLeg-Computer/macemu-rootless) | rootless maintenance copy | Carries the older zydeco/mabam/emendelson rootless line, but no clearly new 2024+ direction beyond that |
| [pfhore-github/macemu](https://github.com/pfhore-github/macemu) | historical branch archive | Has old `DavidLudwig_fix`, `address_translation`, `mmu`, and `newcpu` branches, but `master` itself only adds merge commits |
| [WarlockD/macemu](https://github.com/WarlockD/macemu) | compile experiment fork | Only three unique commits on `master` (`ok giving up...` / revert / `Ok we compile!`) plus a few test branches |
| [MatthewCroughan/macemu](https://github.com/MatthewCroughan/macemu) | upstreamed staging fork | Their `module_init` kernel-module fix landed in kanjitalk; no remaining independent delta |
| [gbraad-apple/macemu-fork](https://github.com/gbraad-apple/macemu-fork) | historical Apple Silicon staging fork | Unique AArch64/Mach-exception work is mostly 2020–2022-era cebix/kanjitalk history, not a fresh 2025–2026 lead |
| [NearlyTRex/MacEmu](https://github.com/NearlyTRex/MacEmu) | plain kanjitalk catch-up fork | No unique commits ahead of kanjitalk `master` |
| [emendelson/macemu](https://github.com/emendelson/macemu) | small Windows-only prefs tweak | Only one unique commit ahead: [`6c8d58d5`](https://github.com/emendelson/macemu/commit/6c8d58d5) adds a `.txt` extension to Windows prefs files |
| [jonassvatos/macemu `claude/sheepshaver-sdl-wayland-*`](https://github.com/jonassvatos/macemu/tree/claude/sheepshaver-sdl-wayland-01VvdLXPFwQEmLQGd6XPWVPB) | Wayland diagnostics branch | Unique and recent, but narrowly focused on SDL logging / software-renderer fallbacks / `BUILD_WAYLAND.md` for Linux waypipe debugging |
| [Tevo45/macemu](https://github.com/Tevo45/macemu) | upstreamed portability staging fork | POSIX shell-function syntax fix was merged into kanjitalk; no remaining independent delta |
| [whimbrels/macemu](https://github.com/whimbrels/macemu) | mirror of jrepp | Same portability-PR line, no independent work |
| [lubert/macemu](https://github.com/lubert/macemu) | historical branch collection | Has old `ios`, `DavidLudwig_fix`, and `address_translation` branches, but no clearly new 2025–2026 direction beyond merged upstream work |
| [mabam/macemu](https://github.com/mabam/macemu) | rootless maintenance fork | Mostly a stale updater for zydeco rootless plus older keyboard / ROM-base experiments |
| [uliwitness/macemu](https://github.com/uliwitness/macemu) | historical staging fork | Mostly carries old `zydeco_rootless`, `DavidLudwig_fix`, and `address_translation` branches; only unique modern branch is a one-line Xcode SDK bump |
| [vaccinemedia/macemu branches](https://github.com/vaccinemedia/macemu/branches) | no extra branches | `master` is the whole story as of this survey |
| [agranlund/macemu](https://github.com/agranlund/macemu) | unrelated platform fork | Atari ST work, not a Mac emulator improvement line |
| [tjpadula/macemu](https://github.com/tjpadula/macemu) | older Linux arm64 notes | Historically interesting, but lower current value than the newer ARM64/macOS forks above |
| [jvernet/macemu](https://github.com/jvernet/macemu) | identical cebix mirror | No unique commits ahead of cebix `master` |
| [MRPROGRAMMA/DeiroMACemu](https://github.com/MRPROGRAMMA/DeiroMACemu) | translated README fork | Only one unique commit ahead of cebix `master`: a Russian README addition |
| [RobotRoom/macemu](https://github.com/RobotRoom/macemu) | identical cebix mirror | No unique commits ahead of cebix `master`; `patch-1` does not open a new line of work |
| [boingball/macemu](https://github.com/boingball/macemu) | AmigaOS maintenance fork | Two unique 2025 commits, but they are narrowly about GTLayout / asm-support / header fixes on the AmigaOS host path |
| [erique/macemu `amiga-gcc-650`](https://github.com/erique/macemu/tree/amiga-gcc-650) | Amiga GCC/VASM compatibility branch | One unique 2025 commit for building with GCC 6.5.0 + VASM; useful only for that host niche |
| [polluks/macemu](https://github.com/polluks/macemu) | older cebix Apple Silicon snapshot | Behind-only fork with no unique commits beyond already-covered 2022 Apple Silicon work |
| [zydeco/macemu `ios`](https://github.com/zydeco/macemu/tree/ios) | stale branch | Old iOS-specific Xcode project, superseded by other macOS-native work |
| [hghpublic/kanjitalk755-macemu](https://github.com/hghpublic/kanjitalk755-macemu) | automated mirror | Exact mirror of kanjitalk755 master; no unique commits |
| [Lockl00p/macemu-WiiU](https://github.com/Lockl00p/macemu-WiiU) | named-only fork | Despite the "WiiU" in the name, no unique commits; identical to cebix master |
| [coderjimbo/macemu-pi](https://github.com/coderjimbo/macemu-pi) | plain cebix mirror | Despite the "pi" in the name, no unique commits ahead of cebix master |
| [cartheur-forks/macemu755](https://github.com/cartheur-forks/macemu755) | kanjitalk catch-up mirror | No unique commits; just tracks kanjitalk755 upstream |
| [seanpm2001/Cebix_MacEmu](https://github.com/seanpm2001/Cebix_MacEmu) | mass-fork archive | seanpm2001 auto-forks; only commit is a README update, no code changes |

---

## Quick-reference: best second-look forks

| Fork | Why re-open it |
|------|-----------------|
| [Cronocide/macemu](https://github.com/Cronocide/macemu) | Parallel AArch64 SheepShaver JIT bring-up — closest conceptual neighbor to this repo's core work |
| [zydeco/macemu](https://github.com/zydeco/macemu) | Best macOS-native/rootless/Xcode branch family outside the kanjitalk tree |
| [quentinmit/macemu](https://github.com/quentinmit/macemu) | Directly relevant VDE quality-of-life fixes |
| [audiocontrol-org/macemu](https://github.com/audiocontrol-org/macemu) | Most interesting alternative host↔guest I/O / automation direction |
| [sirmick/macemu](https://github.com/sirmick/macemu) | Headless/browser-display architecture for Apple Silicon |
| [mihaip/macemu](https://github.com/mihaip/macemu) | Browser delivery / Infinite Mac / removable media / JS-side metrics |
| [mstevenson/macemu](https://github.com/mstevenson/macemu) | macOS window/fullscreen UX ideas worth stealing selectively |
| [robxnano/macemu](https://github.com/robxnano/macemu) | Best newly-checked fork for build-system, prefs-location, and non-GTK desktop-plumbing ideas |
| [AndrewNile/macemu](https://github.com/AndrewNile/macemu) | Broader memory-allocation refactor than the small BasiliskII-only patch |
| [SegHaxx/macemu-flatpak](https://github.com/SegHaxx/macemu-flatpak) | Best packaging/distribution idea mine in the cebix tree: XDG, Flatpak, 64-bit cleanup, legacy-backend removal |
| [jsdf/macemu](https://github.com/jsdf/macemu) | Historical root of the browser/Emscripten line |
| [uyjulian/macemu](https://github.com/uyjulian/macemu) | Experimental BasiliskII/macOS simplification + ARAnyM JIT archaeology |
