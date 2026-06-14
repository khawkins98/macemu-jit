# M16 — NewWorld CGRP Handler-Table Synthesis: Design Spec

**Status:** rev 2 (RE-SCOPED) · 2026-06-14
**Parent strategy:** `docs/planning/MACHINE-LAYER-PLAN.md`
**Predecessors:** `docs/planning/M14-FINDINGS-cuda-delivery.md` §7 · `docs/planning/M15-FINDINGS-consumption-recon.md` "Verdict (Task 4)" + "Addendum — misroute-why diagnostic" · `docs/planning/M16-FINDINGS-oracle-forge.md` Q5/Q6 (the Task-0 RE that re-scopes this milestone).

> **REV-2 RE-SCOPE BANNER (2026-06-14).** Rev-1's hypothesis — that seeding the M14 routing
> triplet (`hnfo+0x14`/`hnfo+0x28`/`KDP+0x674`) plus the `r11` bit would route the EXT edge
> onward — is **SUPERSEDED by the M16 Task-0 RE**. That RE proved: (1) the `r11`/SRR1 `0x8000`
> gate is **already satisfied** (srr1=0x9040), not a blocker; (2) the real onward-route decider
> is a single CGRP-descriptor field `*(KDP-0x338)+0x20 = [0x68ffc1e0] = 1`, needing `≥2`
> (`50314894 cmpwi 2; blt 0x50314660`); and (3) forging that field to ≥2 is **SAFE but INERT** —
> the CGRP handler table it gates is **empty** (`+0x38`=0 guard, `+0x3c`=0 base, `+0x44`=0 count),
> so the service routine `0x503148e0` `beqlr`s without dispatching. **The real fix is to
> synthesize the full CGRP handler table**, which is what this rev specifies. The rev-1
> "oracle-first routing-struct forge" content below is replaced wholesale.

---

## §1 — Problem & Definition of Done

**Problem.** NewWorld 9.x interrupt routing dead-ends inside the NK EXT dispatcher's service
routine. EXT is delivered correctly to the published NK entry `0x50314880` (M15 settled). The
dispatcher reads the **"CGRP" interrupt-group descriptor** at `*(KDP-0x338) = [0x68ffdcc8] =
0x68ffc1c0` (base dynamic; `[+0x04]="CGRP"`, `[+0x00]=0x00010001`) and:
- gates on `[+0x20] ≥ 2` (`50314894 cmpwi 2; blt 0x50314660`) — currently `1` → early-return; and
- even past that gate, the service routine `0x503148e0` **self-guards on an empty handler table**:
  `if [CGRP+0x38]==0: beqlr` (no table), `if idx >= [CGRP+0x44]: bgelr` (out of range), else load
  a handler descriptor from base `[CGRP+0x3c]` and `rfi` to it. **Entry layout (unambiguous from
  disasm):** `[entry+0]` → `SRR0` (handler PC, `503149bc mtspr 0x1a,r18`); `[entry+4]` → `r2`/TOC
  (`503149c8 lwz r2,4(r20)`), **NOT SRR1** — SRR1 is sourced separately from `r19`
  (`mtspr 0x1b,r19`), and `r1`/stack from the `[CGRP+0x40]` stack array. So the synthesized entry
  must set `[entry+0]`=handler PC and `[entry+4]`=the correct r2/TOC; getting `+4` wrong mis-sets the
  handler's globals pointer. All of `+0x38/+0x3c/+0x40/+0x44` read **0** because guest IM init — which
  registers CGRP sources and builds this table — never runs here (M15 pillar 3: structs
  frozen-zero across obs=1e9).

So the fix is **not** a one-word forge (proven inert) but **synthesis of a host-owned CGRP
handler table** whose descriptor entries `rfi` along the path that eventually reaches the 68k
level-1 handler `0x5000ec50`, wired into the live CGRP descriptor (guard/base/stack-base/count
+ the gate field), all resolved from the live `[0x68ffdcc8]` base — never hardcoded.

**This milestone tests one hypothesis:** that a correctly-formatted, host-synthesized CGRP
handler table — populated with the descriptor format the service routine `0x503148e0` consumes,
and a source→handler mapping that chains to `0x5000ec50` — makes the NK EXT dispatcher actually
dispatch the EXT edge, advancing the NewWorld 9.x boot, without the M10-class crash.

**Definition of Done (any ONE is a valid outcome — partial findings beat stalling):**
1. **Synthesis succeeds:** the seeded CGRP table makes `0x503148e0` dispatch; ring confirms the
   EXT edge reaches `0x5000ec50` (previously 0/510) with no SIGSEGV/0xDEADBEEF; boot advances. (Best case.)
2. **Synthesis attempted, format/mapping wrong:** table seeded, dispatch fails, and we have
   ring/probe evidence of *which* descriptor field the service routine read wrong (it self-guards,
   so the failure mode is a clean `beqlr`/`bgelr` or a wrong-target `rfi`, not a crash) — handed to a follow-on.
3. **Format unobtainable (viability fork):** Task 0 proves neither IM-init RE of `rom901.bin`
   nor the QEMU format oracle yields the descriptor layout + source→handler mapping within budget;
   the milestone parks with the infeasibility documented and the gate-field probe (safe/inert,
   confirms the RE) as the residual artifact.

In all three, the standing gate (`make test-jit` score=100) stays green and paravirtual `make e2e`
is unaffected (all new code env-gated behind `SS_M16_FORGE=1` + `MachineProfileIsNewWorld()`).

---

## §2 — Scope

**IN:**
- Task 0 (BINDING): obtain (a) the **CGRP handler-descriptor format** the service routine
  `0x503148e0` consumes — exact layout of `[CGRP+0x38]` guard, `[CGRP+0x3c]` descriptor-array
  base, `[CGRP+0x40]` stack-array base, `[CGRP+0x44]` count, the per-entry stride, and the
  `[entry+0]`=SRR0(handler PC) / `[entry+4]`=r2/TOC fields (SRR1 from `r19` separately, not the
  entry) — via static RE of `rom901.bin`; and (b) the **source→handler chain** that lands the EXT
  source's `rfi` on a path that *eventually* reaches `0x5000ec50` — the `rfi` target `[entry+0]` is
  an NK/PPC-space handler, **not** `0x5000ec50` directly; the chain is `entry.SRR0 → NK EXT handler
  → (crosses into the DR/68k emulator) → 68k L1 handler 0x5000ec50`. Task 0 must trace the FULL
  chain (which source #/index the EXT edge dispatches, and what SRR0 the entry needs so the chain
  terminates at `0x5000ec50`), not assume a direct target.
  Static RE of our exact 9.0.1 ROM is primary; the QEMU mac99 rig is a **format/semantics
  tiebreaker only** (512MB/9.2.1 — read structure, never literal addresses; read at ~5s
  pre-System-customization). Emit a blocking-answer table with a GO/NO-GO on each field.
- A single env-gated synthesis (`SS_M16_FORGE=1` + `MachineProfileIsNewWorld()`): at
  NW-trampoline-end, resolve the live CGRP base from `[0x68ffdcc8]`, write a host-owned handler
  descriptor + stack into NK-owned scratch, then set `[CGRP+0x3c]`/`[CGRP+0x40]`/`[CGRP+0x44]`/
  `[CGRP+0x38]` guard and the `[CGRP+0x20]` gate field accordingly.
- Falsifiable acceptance: ring-confirmed reach of `0x5000ec50`, no SIGSEGV/0xDEADBEEF.

**OUT:**
- Any non-gated change to the delivery path (M15 proved delivery is already correct).
- Pursuing the full IM-init root cause (why it runs downstream of Cuda init) — separate, larger effort.
- Hardcoding the CGRP base or any literal QEMU address.
- Performance work, paravirtual-path changes, anything touching the 8.6–9.0.4 booting configs.

---

## §3 — Method (the work, in order)

1. **Format RE — Task 0 (primary).** Statically RE `0x503148e0` end-to-end against `rom901.bin`
   (capstone, BE): pin the descriptor stride, the index source (which source # the EXT edge uses),
   the exact SRR0/SRR1/stack fields it loads, and where the `rfi` lands; then RE the onward chain
   to confirm it can reach `0x5000ec50`. Grep the decompressed ROM for stores to `[base+0x38/+0x3c/+0x44]`
   off the CGRP/`KDP-0x338` base to find the guest IM-init code that *would* build the table (its
   construction code is the ground-truth format spec).
2. **Format oracle (tiebreaker, only if RE ambiguous) — Task 0.** QEMU mac99 via `qemu-rig.sh`;
   read a populated CGRP descriptor + handler descriptor *structure* at ~5s. Format/semantics only.
3. **Synthesis smoke boot — Task 2.** Seed the host-owned table behind `SS_M16_FORGE=1`,
   `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1`; ring-walk for `0x5000ec50` reach and crash-absence.
4. **Reflight / bisect boot(s) — Task 3.** If smoke fails, vary one descriptor field at a time
   (guard vs base vs count vs entry-SRR0) to find which the service routine read wrong — bounded.

**Budget:** ≤~8 slot boots + ≤~3 QEMU boots (tiebreaker only). Stop-when: dispatch reaches
`0x5000ec50` (DoD 1), OR the wrong descriptor field is isolated with evidence (DoD 2), OR the
format is proven unobtainable within budget (DoD 3).

---

## §4 — Key risks (carried into the plan's Task 0)

- **R1 (foundational): the descriptor format is RE-intractable.** `0x503148e0` may index through
  multiple indirections whose layout can't be pinned from static RE alone within budget. Mitigate:
  grep for the IM-init *construction* code (stores to `+0x38/+0x3c/+0x44`) — building code is the
  clearest format spec; QEMU as a structural tiebreaker.
- **R2: source→handler mapping wrong → inert or mis-targeted `rfi`.** Even a correctly-formatted
  table dispatches nowhere useful if the entry's SRR0 doesn't chain to `0x5000ec50`. The service
  routine self-guards, so the failure is clean (`beqlr`/`bgelr` or a wrong-PC `rfi`), not a crash —
  ring-walk `--r24-flow` + `--find-pc` isolates it. Task 0 must pin the SRR0 chain, not just the layout.
- **R3: crash risk (the M10 lesson).** Writing NK structures the host doesn't own can desync the NK.
  Mitigate: the service routine self-guards (M16 Q6 — the empty-table case proved safe); seed minimal,
  resolve the base live, gate everything, ring-confirm, revert-on-red.
- **R4: scratch ownership.** The synthesized descriptor + stack must live in NK-owned memory the NK
  won't reclaim/overwrite. Task 0 must identify a safe scratch region (candidate: the trampoline's
  `scratch=68ff5000` already host-asserted in `[NW-TRAMP] W2 'Hnfo'`).
- **R5: QEMU transfer (demoted).** QEMU rig is 512MB/9.2.1 — format/semantics only, never literal
  addresses or values. Used only as a tiebreaker; the primary source is RE of our exact ROM.
