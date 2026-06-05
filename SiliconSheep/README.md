# SiliconSheep

> **Status:** 🟡 Active · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** Developer guide for the Tauri v2 launcher/VM manager for SheepShaver.

A modern launcher and VM manager for [SheepShaver](../SheepShaver/), bringing a
Parallels-like experience to classic Mac OS (8.x–9.x) emulation on Apple Silicon.

## Architecture

SiliconSheep is a **Tauri v2** app — a Rust backend with a web frontend (HTML/CSS/TypeScript).
It manages VM profiles and launches SheepShaver as a **sidecar** child process. The emulator
owns its own SDL3 window for the guest display; the launcher handles everything else.

```
SiliconSheep/
├── src-tauri/          # Rust backend (Tauri commands, VM management, IPC)
│   ├── src/
│   │   ├── main.rs     # Tauri entry + commands (launch, stop, settings, backup)
│   │   ├── vm.rs       # VM profile CRUD, duplicate (clonefile), disk backup
│   │   └── prefs.rs    # SheepShaver prefs parser (round-trip, K/M/G, comments)
│   ├── Cargo.toml
│   ├── capabilities/   # Tauri v2 permission grants (shell, dialog)
│   ├── tauri.conf.json
│   └── build.rs
├── src/                # Web frontend (TypeScript + CSS)
│   ├── main.ts         # App: wizard, VM library, settings panel, toasts
│   └── styles.css      # Dark/light mode, responsive card grid
├── index.html          # Vite entry
├── package.json        # pnpm deps (Tauri CLI, Vite, TypeScript)
├── vite.config.ts
└── README.md           # this file
```

**Boundary rule:** SiliconSheep never `#include`s emulator headers. It interacts with
SheepShaver only through:
- The plain-text prefs file format (`KEYWORD value` per line)
- Process lifecycle (spawn, signal, wait)
- Future: Unix domain socket for IPC (hot-reload, status via existing `rpc_unix.cpp`)

## Prerequisites

- Rust 1.70+ (`rustup`)
- Node.js 18+ and pnpm (`brew install node`, `npm i -g pnpm`)
- A built SheepShaver binary at `../SheepShaver/src/Unix/SheepShaver`

## Development

```bash
cd SiliconSheep
pnpm install
pnpm dev           # starts Vite dev server + Tauri window with hot reload
```

## Build

```bash
pnpm build         # production → src-tauri/target/release/bundle/macos/SiliconSheep.app
```

## Test

```bash
pnpm check         # runs TypeScript type-check + Rust tests (10 prefs parser tests)
pnpm test          # Rust tests only
pnpm typecheck     # TypeScript only
```

## Current Features

- **First-run wizard** — 4 screens: Welcome → ROM picker (SHA-256 verification) → Disk
  creation/selection + optional CD → Review & Boot
- **VM library** — Card grid with Start/Stop/Settings/Duplicate/Reveal/Delete
- **Settings panel** — Sidebar sections (General, Display, Storage, Network, Advanced)
  with hot-reload badges and Save button
- **Prefs bridge** — Round-trip faithful parser for SheepShaver's text format (comments,
  K/M/G suffixes, multi-value keys)
- **VM lifecycle** — Launch with emulator discovery, SIGUSR1 clean shutdown, running
  status polling via `try_wait()`
- **Disk backup** — APFS `clonefile` copy-on-write snapshot of disk images
- **VM duplicate** — Instant APFS clone with path rewriting
- **Toast notifications** — Launch coach marks, save confirmation, error feedback
- **Dark/light mode** — CSS `prefers-color-scheme`
- **Accessibility** — `focus-visible` styles, semantic buttons, keyboard navigation

## Relation to SheepShaver

This is a sibling project, not a replacement. SheepShaver continues to work as a standalone
Unix binary with its text prefs file. SiliconSheep wraps it with a GUI — it does not modify
the emulator core. Emulator-side changes (IPC socket, new EmulOps) are contributed back to
`SheepShaver/` as normal patches, documented in the main CHANGELOG.md.

## Feature Plan

See [`docs/planning/DESKTOP_INTEGRATION_PLAN.md`](../docs/planning/DESKTOP_INTEGRATION_PLAN.md)
for the full tiered roadmap (Tier 1: wizard + VM library, Tier 2: hot-reload + multi-folder,
Tier 3: coherence-lite).
