# Deprecated Scaffolding Inventory

> **Created:** 2026-06-10 · **Purpose:** catalog env-gated code from Path A (NewWorld ROM
> port) and Path B (Upgrade Card) that becomes removable once the Machine Layer approach
> (`MACHINE-LAYER-PLAN.md`) is operational. **Do not remove any of this code yet** — it's
> all env-gated (default off) and harmless. This doc exists so the cleanup is mechanical
> when the time comes.

---

## Context

Three approaches to Mac OS 9.2.x were tried in sequence:

| Path | Status | Key env vars |
|------|--------|-------------|
| **A — NewWorld ROM port** | Parked 2026-06-09 | `SS_NW_TRAMPOLINE`, `SS_NW_MODEL`, `SS_NW_SYNTH_ENTRY` |
| **B — Upgrade Card** (9.2.1 on 1.1 ROM) | Dead end 2026-06-10 | `SS_COMPAT_92X`, `SS_NW_MODEL` |
| **Machine Layer** (9.0.1 ROM + device models) | Active | New code, mostly in worktree |

Paths A and B left env-gated scaffolding across ~5 files. None of it runs unless you
set the env vars. The Machine Layer is a clean-sheet approach that doesn't depend on
any of it.

---

## 1. Remove when Machine Layer lands

These are Path A/B-specific and have no value once proper NewWorld support exists.

### SS_NW_TRAMPOLINE (12 sites — largest block)

The nanokernel trampoline path: seeds supervisor state, allocates sub-KDP/HTAB/page
descriptors, installs register-fixup trampoline, enables tick-count interrupt injection.

| File | Lines | What it does |
|------|-------|-------------|
| `rom_patches.cpp` | ~681 | Synthetic entry stub at ROM+0x310000 (paired with SS_NW_SYNTH_ENTRY) |
| `rom_patches.cpp` | ~714 | Register-fixup trampoline at ROM+0x429b40 |
| `rom_patches.cpp` | ~814 | Seed r13/r14/r15 for Init.s kernel memory layout |
| `rom_patches.cpp` | ~1098 | Comment: don't NOP stwu for parcels descriptor-create |
| `rom_patches.cpp` | ~1117 | SegMap/PMDT stub init (also gated on g_rom_904_lenient) |
| `sheepshaver_glue.cpp` | ~1302-1399 | Memory setup: sub-KDP pool, HTAB, page descriptors, SDR1 |
| `sheepshaver_glue.cpp` | ~1677-1704 | Tick-count interrupt injection for nanokernel idle loop |
| `main_unix.cpp` | ~990-1003 | Memory validation: includes trampoline regions in valid-address check |

### SS_NW_MODEL (2 sites)

Device-tree and UniversalInfo identity injection. Proven insufficient for 9.2.1.

| File | Lines | What it does |
|------|-------|-------------|
| `rom_patches.cpp` | ~1847-1856 | UniversalInfo: BoxFlag=0x96, gestaltMachineType=406 |
| `name_registry.cpp` | ~94-120 | Device tree: model="PowerMac3,1", compatible list |

### SS_NW_SYNTH_ENTRY (2 sites)

Path B diagnostic: skip nanokernel, enter DR Emulator directly. Dead end (hits
Mixed-Mode F-line trap).

| File | Lines | What it does |
|------|-------|-------------|
| `rom_patches.cpp` | ~676-695 | Cold-start dispatch stub at ROM+0x310000 |
| `sheepshaver_glue.cpp` | ~1521-1531 | Conditional logging |

### SS_ROM_SKIP_JUMP68K (1 site)

Diagnostic fallback: skip jump68k patch when pattern-find fails.

| File | Lines | What it does |
|------|-------|-------------|
| `rom_patches.cpp` | ~1293-1298 | Allow boot to proceed unwedged |

### SS_COMPAT_92X (0 sites — already removed)

SCC serial monitor patches. Code removed; only a comment remains at `rom_patches.cpp:~2137`.

### Path A/B comments

Scattered "Path A" / "Path B" / "Upgrade Card" references in comments at:
- `rom_patches.cpp`: ~654, ~679, ~1275, ~1289
- `name_registry.cpp`: ~97, ~105
- `sheepshaver_glue.cpp`: ~1470

---

## 2. Retain (general-purpose)

These are useful beyond any specific path and should survive cleanup.

| Env var | Why keep |
|---------|----------|
| `SS_ROM_LENIENT` | Multi-ROM version support; useful for any new ROM |
| `SS_ROM_PATCH_TRACE` | General pattern-search tracing diagnostic |
| `SS_FORCE_ALTIVEC` | Validated AltiVec feature enabler |
| `SS_PROBE_PC` | General-purpose PC-targeted register/memory dump |
| `SS_JIT_RING_68K_MONITOR` | Boot diagnostic; documented $28 corruption root cause |
| `ROMTYPE_NEWWORLD` | ROM type discrimination; needed for any multi-ROM support |

---

## 3. Cleanup procedure (when ready)

1. Confirm Machine Layer boots 9.2.x on the 9.0.1 ROM
2. Remove all §1 env-var checks and their gated code blocks
3. Remove Path A/B comments (§1 comment list)
4. Run `make test-jit` + boot 8.6 to confirm no regression
5. Delete this doc
