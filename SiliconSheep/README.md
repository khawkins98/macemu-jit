# Silicon Sheep

> **Status:** 🟡 Active · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** Developer guide for the Tauri v2 launcher/VM manager for SheepShaver.

A modern launcher and VM manager for [SheepShaver](../SheepShaver/), bringing a
Parallels-like experience to classic Mac OS (8.x–9.x) emulation on Apple Silicon.

## Architecture

Silicon Sheep is a **Tauri v2** app — a Rust backend with a web frontend (HTML/CSS/TypeScript).
It manages VM profiles and launches SheepShaver as a **sidecar** child process. The emulator
owns its own SDL3 window for the guest display; the launcher handles everything else.

```
SiliconSheep/
├── src-tauri/          # Rust backend (Tauri commands, VM management, IPC)
│   ├── src/
│   │   ├── main.rs     # Tauri entry point + command registrations
│   │   └── vm.rs       # VM profile CRUD (JSON manifest + .sheepvm bundles)
│   ├── Cargo.toml
│   ├── tauri.conf.json # Tauri config (window, sidecar, plugins)
│   └── build.rs
├── src/                # Web frontend (TypeScript + CSS)
│   ├── main.ts         # App entry point, VM library view
│   └── styles.css      # Dark/light mode, card grid, buttons
├── index.html          # Vite entry
├── package.json        # npm deps (Tauri CLI, Vite)
├── vite.config.ts
└── README.md           # this file
```

**Boundary rule:** SiliconSheep never `#include`s emulator headers. It interacts with
SheepShaver only through:
- The plain-text prefs file format (`KEYWORD value` per line)
- Process lifecycle (spawn, signal, wait)
- Future: Unix domain socket for IPC (hot-reload, status)

## Prerequisites

- Rust 1.70+ (`rustup`)
- Node.js 18+ (`brew install node`)
- A built SheepShaver binary at `../SheepShaver/src/Unix/SheepShaver`

## Development

```bash
cd SiliconSheep
npm install
npm run dev        # starts Vite dev server + Tauri window
```

## Build

```bash
npm run build      # production build → src-tauri/target/release/
```

## Relation to SheepShaver

This is a sibling project, not a replacement. SheepShaver continues to work as a standalone
Unix binary with its text prefs file. Silicon Sheep wraps it with a GUI — it does not modify
the emulator core. Emulator-side changes (IPC socket, new EmulOps) are contributed back to
`SheepShaver/` as normal patches, documented in the main CHANGELOG.md.

## Feature Plan

See [`docs/planning/DESKTOP_INTEGRATION_PLAN.md`](../docs/planning/DESKTOP_INTEGRATION_PLAN.md)
for the full tiered roadmap (Tier 1: wizard + VM library, Tier 2: hot-reload + multi-folder,
Tier 3: coherence-lite).
