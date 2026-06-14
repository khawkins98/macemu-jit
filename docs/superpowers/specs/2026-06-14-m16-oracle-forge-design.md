# M16 — Oracle-First NK Routing-Struct Forge: Design Spec

**Status:** DRAFT (rev 1) · 2026-06-14
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`
**Predecessors:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 (forge-class collapse + resume experiment) · `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" + "Addendum — misroute-why diagnostic" (the structural confirmation that sends us here).

---

## §1 — Problem & Definition of Done

**Problem.** NewWorld 9.x interrupt routing dead-ends because the guest's Interrupt Manager (IM) init — which populates the NK routing structures (`hnfo+0x14` source table, `hnfo+0x28` pending bits, `KDP+0x674` CR mask) — runs downstream of Cuda init and never completes. The M15 misroute-why diagnostic pinned the exact branch: the NK EXT dispatcher at `0x50314880` receives the (correctly-delivered) EXT exception but declines to route onward at `50314898 blt 0x50314660`, because the deciding fields — saved-`r11` bit `0x8000` and `*(r8-0x338)+0x20` — read uninitialized (frozen-zero, M15 pillar 3, 0 writes to obs=1e9). No edit to our delivery path changes this; the structs must be populated. The verdict is FORGE: host-seed the NK routing infrastructure.

**This milestone tests one hypothesis:** that we can seed a *correct, well-informed* set of NK routing-struct values (sourced from a boot where IM init genuinely runs) such that the NK EXT dispatcher routes onward to the 68k level-1 handler `0x5000ec50`, advancing the NewWorld 9.x boot — without the 0xDEADBEEF-class crash that killed the M10 blind forge.

**Definition of Done (any ONE of these is a valid milestone outcome — partial findings beat stalling):**
1. **Forge succeeds:** seeded values route the EXT edge through `0x50314660`/the service path to `0x5000ec50`; boot advances past the current park with no crash. (Best case.)
2. **Forge attempted, values wrong:** seeds applied, gate fails, and we have ring/probe evidence of *which* field is wrong and what the dispatcher actually read — handed to a follow-on.
3. **Oracle infeasible (viability fork):** Task 0 proves neither paravirtual nor QEMU mac99 yields transferable values (paravirtual doesn't run the NK; QEMU addresses/format don't transfer), and the milestone re-scopes to structural synthesis or parks with the infeasibility documented.

In all three, the standing gate (`make test-jit` score=100) stays green and paravirtual `make e2e` is unaffected (all new code env-gated behind `SS_M16_*` + `MachineProfileIsNewWorld()`).

---

## §2 — Scope

**IN:**
- Task 0 (BINDING): oracle viability + value extraction. Resolve the live `hnfo` base (never hardcode `0x68ff4f00`), confirm whether paravirtual runs the NK at all, stand up QEMU mac99 (256MB, 9.0.1 ROM) as the value oracle, and characterize the three struct fields + the deciding `r11` bit / `*(r8-0x338)+0x20` source-count field. Emit a blocking-answer table with a GO/NO-GO on each field.
- A single env-gated seed mechanism (`SS_M16_FORGE=1`) applying the resolved values at NW-trampoline-end (the `SS_SEED_MEM` injection point), guarded by `MachineProfileIsNewWorld()`.
- Un-fencing the MODE_68K CR injection for NewWorld (M14 §7 step 2) — **only if** Task 0 shows it is required and only behind the same gate.
- Falsifiable acceptance: ring-confirmed reach of `0x50314660`/service path and `0x5000ec50`, with no SIGSEGV/0xDEADBEEF.

**OUT:**
- Any non-gated change to the delivery path (M15 proved our delivery is already correct).
- Pursuing the full IM-init root cause (why it runs downstream of Cuda init) — that is a separate, larger effort; this milestone forges around it.
- Performance work, paravirtual-path changes, anything touching the 8.6–9.0.4 booting configs.

---

## §3 — Method (the boots, in order)

1. **Oracle-viability boot(s) — Task 0.** (a) Probe our own paravirtual boot for any NK struct presence (expected: absent — confirms paravirtual is NOT the oracle). (b) QEMU mac99 9.0.1/256MB boot via `qemu-rig.sh`; read `hnfo`/`KDP+0x674`/source-table region post-IM-init via the monitor. Caveat (load-bearing): QEMU is a *value* oracle for NK lowmem only — never an MMIO-address oracle; confirm the guest-virtual addresses match ours (same ROM + RAM size ⇒ deterministic NK allocation, to be verified, not assumed).
2. **Forge smoke boot.** Seed the three locations (+ un-fence if required), `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1`, ring-walk for `0x50314660`→`0x5000ec50` reach and crash-absence.
3. **Reflight / bisect boot(s).** If the smoke fails, vary one field at a time (which seed flips the `blt`) — bounded.

**Budget:** ≤~8 slot boots + ≤~3 QEMU boots. Stop-when: forge routes to `0x5000ec50` (DoD 1), OR the wrong field is isolated with evidence (DoD 2), OR oracle proven infeasible (DoD 3).

---

## §4 — Key risks (carried into the plan's Task 0)

- **R1 (foundational): paravirtual is not an oracle.** SheepShaver's normal boot HLEs the ROM; the 9.0.1 NK structs almost certainly don't exist there. Task 0 must confirm and pivot to QEMU.
- **R2: QEMU addresses don't transfer.** `hnfo` is NK-allocated; if its guest address differs from ours, only the *format/relative-layout* transfers, not literal pointer values — the seed must then synthesize a host-owned table and point `hnfo+0x14` at it.
- **R3: crash risk (the M10 lesson).** Writing NK structures the host doesn't own can desync the NK. Mitigate: seed minimal, gate everything, ring-confirm, revert-on-red.
- **R4: the `r11` bit `0x8000` gate.** `50314884`/`50314888` branches away *before* the struct read if that bit is clear — the seed may be necessary but not sufficient. Task 0 must capture what sets it on a working frame.
