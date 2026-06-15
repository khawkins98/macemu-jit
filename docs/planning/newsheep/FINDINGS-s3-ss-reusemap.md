# NewSheep — S3 recon: the SheepShaver-side reuse/collision map (two-supervisor reconciliation)

**Status:** COMPLETE (2026-06-15) · READ-ONLY recon feeding S3 Task-0. Maps what S3 must KEEP / TOGGLE /
RETIRE / YIELD on the SS side when the real NK owns the live supervisor regime, + the collision points.
All file:line are re-verify-before-use (drift ±1–5). Pairs with `FINDINGS-s3-nk-ownership.md` (which
proved co-residency is statically impossible → the disposition below is REPLACE/YIELD, not CO-OWN).

## Disposition map

| File:line | Surface | Disposition | Why |
|---|---|---|---|
| `machine/exc_core.cpp:12` `ExcEnter()` | **KEEP** | Pure state transition (SRR0/SRR1 capture, MSR entry-mask, PC dispatch). Hardware-vectoring mechanism — PEM masks = LAW. The NK REUSES these semantics; it owns the *entry decision*, not the mechanism. |
| `machine/exc_core.cpp:104` `ExcRfi()` | **KEEP** | OEA-compliant `rfi` restore. Both SS and NK use it. |
| `machine/exc_core.cpp:58` `ExcDeliveryDecision()` | **KEEP** | Pure stateless gate (pending→depth→EE→native), law-tested. NK consumes it for its own delivery decision. |
| `machine/exc_core.cpp:89/94` `ExcDeferredEdgeLatch/Fire()` | **KEEP math, maybe RETIRE windows** | Riser-stub EE-edge suppression. S3 may retire SS's windows (NK has own atom boundaries) but keep the math. |
| `kpx_cpu/sheepshaver_glue.cpp:142` `g_exc_entry_table` | **RETIRE / GATE** | Static `{interrupt=0x50412b1c, syscall=0, external=0x50314880}`. SS decides the route. **Collision 1**: NK installs the real vectors; under the gate this table must be nulled/bypassed. |
| `kpx_cpu/sheepshaver_glue.cpp:1122` `deliver_pending_dec_exception()` | **RETIRE** | The body (`:1122-1458`) IS SS's synthetic supervisor: owns the DEC latch, calls ExcEnter twice (EXT `:1254`, DEC `:1342`), applies SRR0/SRR1/MSR/PC to the live CPU. **Collision 3.** The whole regime changes — can't be patched. |
| `kpx_cpu/sheepshaver_glue.cpp:1002` `interrupt()` | **RETIRE/MUTE** | Legacy KDP-shim trampoline. Under newworld the 2-SPR shim already bypasses it; the gate can mute it. |
| `kpx_cpu/sheepshaver_glue.cpp:1461` `execute_68k()` | **RETIRE (Q-S3.1)** | Reads the Execute68k pair `[KDP+0x1074]/[KDP+0x1078]` (`:1504-1505`) = SS's OWN JIT emulator (`0x50480000/0x50460000`), NOT the ROM's `0x50360000`. `FINDINGS-s3-nk-ownership.md`: the parcel has NO ref to either → the pair is an SS synthetic; the NK reaches 68k via its own ECB/KDP dispatch. **Collision 2.** |
| `kpx_cpu/sheepshaver_glue.cpp:1683` `ppc_cpu` (static) | **KEEP** | Host-side CPU register container. |
| `kpx_cpu/sheepshaver_glue.cpp:2806/2805/2918` forge (SDR1/HTAB/MSR) | **RETIRE** | The forged supervisor state (SDR1 in C, zeroed HTAB, MSR 0x7072). The NK install replaces it. |
| `virt_clock.cpp:108` `VirtClockDECPending()` | **YIELD** | The DEC latch SS consults for delivery. Under NK (which owns DEC `0x50313200`, re-arms its own decrementer) this becomes advisory; the delivery gate must NOT consult it. Stays readable for paravirtual/diagnostics. |
| `virt_clock.cpp:71` `VirtClockWriteDEC()` | **YIELD/SUPPRESS** | Host DEC write + `on_dec_write` callback. NK's guest `mtspr DEC` contests it; suppress the host callback under the gate. |
| `event_sched.cpp:66` `process_timers()` | **YIELD** | Host scheduler pump. When the NK owns DEC, host scheduling is advisory; SS scheduler goes quiescent (callbacks fire but ignored). Falsifiable: SS-scheduler-ticks=0 while NK live (G3.a). |
| `ppc-cpu.cpp:1986` `check_spcflags()` newworld arm | **TOGGLE (B4)** | The EXISTING gated arm — S3 EXTENDS it (good precedent). The call site `SheepExcDeliverPending()` (`:1687`, called `~:2027`) is the S3 hook to retire/replace with the NK-supervisor check. |
| `rom_patches.cpp:716` `SS_NW_TRAMPOLINE` reg-fixup forge | **RETIRE** | Synthesizes 68k vectors + KDP/Hnfo records host-side (`:729-900+`): writes guest `[0x04]` reset-PC, `[0x10/0x28/0x2C]` stubs, `[KDP+0xfd0]` Hnfo, the MM save pool. The "producer never ran" symptom — Route A makes these DEAD; the host forge must stop so the NK's real writes stick. |

## The 3 load-bearing collisions S3's architecture must resolve

**Collision 1 — Exception-entry-table ownership (CRITICAL).** SS's `g_exc_entry_table` (`glue:142`)
decides the delivery route (`:1179-1180`, `:1254`, `:1342`); the NK installs its own handlers at the real
vectors. Both can't decide simultaneously. → under `SS_M18_NK_SUPERVISOR`, null/bypass the SS table; the
NK owns EXT `0x50314880`/SC `0x50314ac0`/DEC `0x50313200`/PROGRAM `0x50314700`.

**Collision 2 — 68k-dispatch ambiguity (Q-S3.1).** SS `execute_68k` reads `[KDP+0x1074/0x1078]`; the NK
reaches 68k via its own ECB/KDP dispatch (no static `0x50360000` ref; `FINDINGS-s3-nk-ownership.md`). →
the SS pair is an SS synthetic; under the gate `execute_68k` is dead (orthogonal door), pending the impl
probe that confirms it's provably dead under a resident NK.

**Collision 3 — DEC/scheduler delivery authority (LOAD-BEARING).** SS owns the DEC pending latch
(`VirtClockDECPending`) and decides delivery (`SheepExcDeliverPending` @ `ppc-cpu.cpp:~2027`); the NK owns
its reschedule loop `0x50313200` (re-arms DEC itself). → **FULL YIELD** (recommended): under the gate, SS
doesn't call `deliver_pending_dec_exception()` at all; the NK's DEC handler runs; SS scheduler becomes
advisory/quiescent.

## Env-gate census

| Gate | Location | S3 disposition |
|---|---|---|
| `SS_M18_NK_SUPERVISOR` | **NOT YET DEFINED — CREATE** | The master S3 gate (default OFF ∧ `MachineProfileIsNewWorld()`). Gates `deliver_pending_dec_exception()`, the forge writes, the table bypass. |
| `SS_NW_DEC_PUBLISHED` | `glue:199` | EXTEND — S3 gates the entire delivery call. |
| `SS_NW_TRAMPOLINE` | `rom_patches.cpp:729` | GATE — DEAD when `SS_M18_NK_SUPERVISOR=ON` (real producer runs). |
| `SS_NW_MM_SWITCH`/`MM_POOL` | `rom_patches.cpp:867` | INHERIT — keep if the NK respects the same pool (`0x68ff5800`); conflict only if relocated. |
| `SS_NW_EE_RISER` | `rom_patches.cpp:2576` | INHERIT/RETIRE the stub (NK has own atom boundaries). |

## Conclusion

S3 is architecturally feasible: the infrastructure is already gated (B4 — toggle an existing arm), and
`exc_core.cpp`'s vectoring math SURVIVES. The hard part is reconciling delivery authority + 68k dispatch
**without both paths active** — and `FINDINGS-s3-nk-ownership.md` removes CO-OWN from the menu (co-residency
is impossible). So the architecture is **REPLACE** (or thin-shim): retire SS's synthetic supervisor
(`deliver_pending_dec_exception`, the table, the forge) under the gate; YIELD the scheduler; KEEP the
ExcEnter/ExcRfi vectoring + the bare core + device models beneath the resident NK. S3-Task-0's Q-S3.3
picks REPLACE vs thin-shim; the collisions above are the contracts it must satisfy.
