# NewSheep — Trampoline RE (Task-0) Findings

**Status:** IN PROGRESS · spec `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Method:** two instruments (static tbxi+capstone / dynamic QEMU gdbstub), gated on agreement; both on 9.2.

## Blocking-answer table
| # | Question | Static finding | Dynamic finding | Agree? | Status |
|---|----------|----------------|-----------------|--------|--------|
| Q0-D | Canonical 9.2.x ROM + internal version (the "9.0.1=9.2.2" reframe) | **CLOSED** — `2001-12-19 Mac OS ROM 9.0.1` (md5 `66210b4f…`, == active project ROM) chosen as canonical RE binary; NanoKernel **v02.27**, ROM-file ver only (no embedded OS ver) | n/a | n/a | ✅ |
| Q0-A | OF client-interface call set the Trampoline makes; bounded-and-stubbable vs open-ended | _pending_ | _pending_ | — | — |
| Q0-B | Interrupt-setup writes: constants/relocations vs computed-from-OF-tree | _pending_ | _pending_ | — | — |
| Q0-C | Can Route B be honest re-binding (relocation), not value-hardcoding? | _pending_ | _pending_ | — | — |
| Q0-F | **Who builds CGRP — the Trampoline directly, or the NanoKernel from the device tree the Trampoline produces?** (decides whether the producer is Trampoline-alone or Trampoline+NanoKernel) | _pending_ | _pending_ | — | — |
| Q0-E | Route decision (A run / B patch / C reproduce) + SS-integration sketch | _pending_ | _pending_ | — | — |

## Evidence

### Q0-D — ROM identity + version reframe (T0.0) [STATIC]

**Method:** `tbxi 0.13` (venv `/tmp/newsheep/venv`) `dump -o` of the two 9.2-era candidates.

- **Dump shape (both 9.0.1 and 8.4, identical top-level layout):** `Bootscript` (CHRP ASCII),
  `MacOS.elf` (the Trampoline — top-level ELF), `Parcels` / `Parcels.src/` (PEF device drivers +
  `MacROM.src/`). No parcels-layout break between 8.4 and 9.0.1 — both tbxi-dump cleanly with the
  same structure. (R4's "9.1+ different layout" concern does not bite the 9.0.1 binary we target.)
- **The ROM file has NO embedded Mac OS *system* version string.** The "9.0.1" in the filename is
  the **ROM-file version**; the Mac OS system version (9.2.x) is supplied by the System file on disk,
  not the ROM. So the reframe is confirmed by *date + components*, not an OS-version string:
  - `2001-12-19 Mac OS ROM 9.0.1` — md5 `66210b4f71df8a580eb175f52b9d0f88`, **byte-identical to our
    active project ROM**. NanoKernel **v02.27** (`Configfile-1: KernelCodeOffset=…=NanoKernel-v02.27`).
    Dec-2001 = the Mac OS 9.2.2 era. `BootstrapVersion='NewWorld v1.0   '`.
  - `2001-07-30 Mac OS ROM 8.4` — md5 `f97d4382…`, NanoKernel **v02.24**. Jul-2001 = 9.2/9.2.1 era.
- **Configfile-1 structural map (9.0.1)** — the ROM component table the NanoKernel/Trampoline use:
  `ROMImageSize=0x00400000`; `Mac68KROM` @BASE+0x0 (0x300000 B); `ExceptionTable` @BASE+0x300000;
  `HWInitCode` @BASE+0x320000; **`NanoKernel-v02.27` @BASE+0x310000 (KernelCode, 0x10000 B)**;
  `EmulatorCode` @BASE+0x360000; `OpcodeTable` @BASE+0x380000.
- **Two NanoKernel parcels present in 9.0.1 dump** (`MacROM.src/`): `NanoKernel-v02.27` (active per
  Configfile) + `NanoKernel-v02.24` (the alternate/fallback). The CGRP-builder disasm target (Q0-F)
  is **`NanoKernel-v02.27`**, 105280 B.

**Verdict:** canonical RE binary = the `66210b4f…` 9.0.1 ROM (== active project ROM; both instruments
analyze this one binary). The "9.0.1 file = 9.2.2-era ROM" reframe is **confirmed** (Dec-2001 date +
NanoKernel v02.27 + structural identity to the active ROM). Remaining 9.2 gap is system software, off
Task-0's path.
