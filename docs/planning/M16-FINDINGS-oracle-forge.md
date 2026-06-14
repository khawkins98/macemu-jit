# M16 — Oracle-First NK Routing-Struct Forge: Findings

**Status:** Task 0 IN PROGRESS · 2026-06-14
**Plan:** `docs/superpowers/plans/2026-06-14-m16-oracle-forge.md` · **Spec:** `docs/superpowers/specs/2026-06-14-m16-oracle-forge-design.md`
**Predecessor:** `docs/planning/M15-FINDINGS-consumption-recon.md` "Addendum — misroute-why diagnostic".

## Task 0 — blocking-answer table

| # | Question | Answer | Status |
|---|----------|--------|--------|
| Q1 | Live `hnfo` base (NOT hardcoded `0x68ff4f00`) | **`0x68ff4f00`** — probe-confirmed `[0x68ffefd0]=0x68ff4f00`; host-asserted by our NW-trampoline (`[KDP+0xfd0]`) | ✅ |
| Q2 | Does paravirtual populate the NK structs? (R1) | not yet probed — but moot (see refinement: our trampoline already host-asserts the structs) | ⏸ |
| Q3a | hnfo address transfer QEMU↔us: LITERAL-OK \| SYNTHESIZE | **likely SYNTHESIZE** — QEMU rig is 512MB/9.2.1; literal pointer transfer unlikely. NOT yet run (strategic fork open) | ⏸ |
| Q3b | SS_SEED_MEM accepts the 3 targets (not MMIO-refused)? | **YES** — `0x68ff4f14`, `0x68ff4f28`, `0x68fff674` all `[SEED] … applied` | ✅ |
| Q4 | `r11` bit `0x8000` — already set on EXT frame, or must be forced? | **already set** — prologue `0x50313d40` does `mfspr r11,0x1b` (r11=SRR1); our EXT frame `srr1=0x9040`, so SRR1 & 0x8000 ≠ 0 → `50314888 beq` NOT taken; dispatcher proceeds to the struct read. NOT the blocker. | ✅ |
| Q5 | the real onward-route decider | **`*(r8-0x338)+0x20 ≥ 2`** (`r8` = SPRG0-derived per-CPU ptr from the prologue). This is the field that gates `50314898 blt 0x50314660`, NOT `hnfo+0x14/+0x28/KDP+0x674` directly. `r8-0x338` target not yet resolved. | 🔶 partial |
| — | Per-field GO/NO-GO + viability fork | **open** — pending the QEMU-oracle vs static-RE-first decision | ⏸ |

## Evidence

### Step 1 — live hnfo base (RUNDIR 20260614-153442.63803)
`[PROBE 0x50314880 visit=1] [0x68ffefd0]=0x68ff4f00`. The probe DID fire at the vector-entry PC (the M15 Task-1 "vector dispatch bypasses block-entry hook" caveat did not bite here). Trampoline log confirms our code host-asserts the Hnfo record: `[NW-TRAMP] W2 'Hnfo' record @68ff4f00 ([KDP+0xfd0]), id=0x3035, scratch=68ff5000`; `[NW-TRAMP] PIC-rail level source staged: [[KDP-0x20]+0xf18]=68ff4f18=0xF3040000 (NK-held PIC base) [0x3f3f]=1 (vector 0x3f -> 68k level 1)`.

### Step 3b — seed-MMIO safety (RUNDIR 20260614-153615.64107)
All three M14 §7 targets accept `SS_SEED_MEM` writes (`applied`, none `REFUSED (MMIO range)`). So the seed can be expressed via `SS_SEED_MEM` for the recon; production seed still gates behind `SS_M16_FORGE`.

### Step 4 — `r11`/SRR1 gate + the real decider (static RE of `rom901.bin`)
Prologue `0x50313d40` (called by `0x50314880 bl`): saves regs, then `mfspr r10,0x1a`/`mfspr r11,0x1b` → **r10=SRR0, r11=SRR1**, `mfcr r13`, `mr r8,r1` (r8 = `mfspr 0x110` = SPRG0-derived per-CPU base), `blr`. Back in the dispatcher:
```
50314884  rlwinm. r9, r11, 0, 16, 16   ; SRR1 & 0x8000  → SET on our frame (srr1=0x9040)
50314888  beq     0x50313ab0           ; NOT taken (bit set)
5031488c  lwz     r9, -0x338(r8)       ; r9 = *(SPRG0_base - 0x338)  ← routing struct ptr
50314890  lwz     r9, 0x20(r9)         ; r9 = struct->[0x20]         ← THE deciding field
50314894  cmpwi   r9, 2
50314898  blt     0x50314660           ; field < 2 → early-return stub (unserviced)
5031489c  bl      0x503238ac           ; field >= 2 → service path (sets err code 9 @ -0x238(r8))
```
**Refinement vs M14 §7:** the onward route is gated by `*(r8-0x338)+0x20 ≥ 2`, not the M14 forge triplet. **Service-path RE (resolves the success-vs-error question):** `0x503238ac` is a context-SAVE helper (stores r14–r31 to context block `r6`, returns SPRG0). So `5031489c` IS the real servicing path (saves full context, sets dispatch state `=9` at `[r8-0x238]`, calls `0x503148e0`), and `blt 0x50314660` is the "nothing to service" early-return stub. **⇒ the forge wants `field ≥ 2` (take the service path).** A zero/uninitialized field → `field<2` → early-return → never reaches `0x5000ec50`, matching M15's 0/510.

**SPRG0 (`r8`) chain (static RE):** at `0x50314620`: `lwz r8,0x5a0(r1)` (r1 = prior SPRG0) → `mtspr 0x110,r8` (new SPRG0) and `mtspr 0x113, r8+0x360` (SPRG3 = the `[KDP+0x360]` vector table). So SPRG0 = a per-CPU/KDP base loaded from `*(prior_SPRG0+0x5a0)`. The deciding pointer `*(SPRG0-0x338)` is itself loaded through this chain — its **runtime value + the `+0x20` field can only be pinned with a live register-snapshot probe** at the dispatch (capture SPRG0 and `*(SPRG0-0x338)+0x20` via `SS_JIT_TRACE_RING` regs-at, or a probe on a block where `r8`=SPRG0). That + RE of what `0x503148e0`/the service path expects in the indexed structures is the remaining Task-0 GO/NO-GO work.

**Next RE step (RE-first path, chosen 2026-06-14):** boot with `SS_JIT_TRACE_RING=1`, find the dispatch record near `0x50314880`/`0x5031488c`, `ring-walk --regs-at` to read runtime SPRG0 and walk `*(SPRG0-0x338)+0x20`. Determine (a) its current value (expect <2, i.e. 0/1), (b) what the service path indexes off it to reach the 68k L1 handler, (c) therefore the minimal correct forge: the value/structure to write so `field≥2` AND the service path finds a valid source→handler mapping. QEMU only as a tiebreaker if our ROM RE is ambiguous.

## Strategic fork (open — needs decision before spending QEMU budget)

The red-team weakened the QEMU oracle (512MB/9.2.1 ≠ our 256MB/9.0.1). Meanwhile Step 4 shows the deciding field is local (`*(r8-0x338)+0x20`), our trampoline already stages NK/PIC structs, and the seed path works. Two routes:
- **(A) QEMU-oracle-first** (plan as written): boot QEMU, read format/CR-mask, synthesize. Cost: ~3 QEMU boots; weak transfer confidence.
- **(B) Static-RE-first** (emergent): resolve `r8-0x338` + the `0x503238ac` service path by RE of `rom901.bin` + targeted live probes; determine what the NK expects in that struct directly from OUR ROM. QEMU only as a tiebreaker. Aligns with "proper RE / solid foundations." Likely higher-confidence since it reads the exact 9.0.1 code that runs.
