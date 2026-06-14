# NewSheep — Trampoline RE (Task-0) Findings

**Status:** IN PROGRESS · spec `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Method:** two instruments (static tbxi+capstone / dynamic QEMU gdbstub), gated on agreement; both on 9.2.

## Blocking-answer table
| # | Question | Static finding | Dynamic finding | Agree? | Status |
|---|----------|----------------|-----------------|--------|--------|
| Q0-D | Canonical 9.2.x ROM + internal version (the "9.0.1=9.2.2" reframe) | _pending_ | n/a | — | — |
| Q0-A | OF client-interface call set the Trampoline makes; bounded-and-stubbable vs open-ended | _pending_ | _pending_ | — | — |
| Q0-B | Interrupt-setup writes: constants/relocations vs computed-from-OF-tree | _pending_ | _pending_ | — | — |
| Q0-C | Can Route B be honest re-binding (relocation), not value-hardcoding? | _pending_ | _pending_ | — | — |
| Q0-F | **Who builds CGRP — the Trampoline directly, or the NanoKernel from the device tree the Trampoline produces?** (decides whether the producer is Trampoline-alone or Trampoline+NanoKernel) | _pending_ | _pending_ | — | — |
| Q0-E | Route decision (A run / B patch / C reproduce) + SS-integration sketch | _pending_ | _pending_ | — | — |

## Evidence
_(filled per task)_
