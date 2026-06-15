# Advisory review — S1 paged-MMU conclusion & Dolphin leverage (2026-06-15)

> **Role:** advisory only. The implementing agent stays in the driver's seat — this is feedback to
> *consider and address*, not a directive or a competing plan. Decisions remain yours.
> **Scope:** the overnight S1 work culminating in `MMU-NANOKERNEL-INSEPARABILITY.md` ("S1 live MMU ⟂ S3,
> build nothing until S3"), the deferred window/softmmu plans, and the question "have we sufficiently
> leveraged Dolphin?"
> **Verification done before writing this:** `make -C SheepShaver/src/machine test` → paged_mmu 36/36 +
> ofci 545 PASS; `make build-ss` clean; `SS_HARNESS_BATCH=1 make test-jit` → **score=100**. The tree is
> green; the work below is real, not vaporware.

## Credit first (this is solid work)
- **Task A is genuine and correct** — `paged_mmu_translate()` + its oracle test pass 36/36 (two-context
  HTAB *and* coarse BAT, real-mode identity, fault rows). Built and validated with **no boot**.
- **S2a is built and tested** (OF-CI callback + Core99 DT, 545 checks), inert/gated, baseline untouched.
- **The central factual finding is true and well-sourced:** today's NewWorld boot runs the
  `SS_NW_TRAMPOLINE` *output-forge*, so the NK's real SR/BAT/SDR1 install never executes (SDR1 assigned
  in C, HTAB zeroed, MSR DR=1 against empty tables). Consistent with the whole M8→M17 forge arc.
- **One guard is correct — keep it:** you cannot *arm* a softmmu / live translation against those
  forged, empty tables in today's working boot. It would fault essentially every access and kill the one
  boot we have.

## Concerns to address

### 1. "Build nothing until S3" overreaches — it conflates three different claims
The doc proves two things and then asserts a third that doesn't follow:
- (a) *Validating* a live MMU against the NK's real tables needs the NK to run. **TRUE** (this was always
  G1.e).
- (b) *Arming* a softmmu/window in **today's** boot, against the forge, kills the boot. **TRUE.**
- (c) *Building and unit-testing* the live-MMU machinery needs S3. **This does not follow** — and Task A
  is the counter-proof: the translation core was built and oracle-tested with no boot. The softmmu walker
  (it reuses `paged_mmu_translate()`) and the Dolphin arena primitive are the same kind of thing.

**Ask:** please separate "we can't *validate/arm* live until S3" (correct) from "we can't *build/de-risk*
the machinery until S3" (not shown). The first is a real constraint; the second forecloses parallel work.

### 2. The conclusion quietly re-architects the plan — and labels the inversion as "confirmation"
The original program had **S1 as the prerequisite that *gates* S3** (S3 needs the MMU). The doc turns
S1-live into a **sub-deliverable *of* S3**. That's a material change to S1's role, not a confirmation of
the original sequencing. As written, the STATUS table now reads like a deadlock (S1-live "blocked by S3";
S3 "blocked by S1+S2").

**Ask:** if the honest model is "S1-live and S3 *co-land*" (the live MMU is exercised once the NK runs),
please say that explicitly and resolve the apparent deadlock in the STATUS table — rather than framing it
as "confirms the original S1→S3 sequencing," which reads as spin over what is actually an inversion of
S1's role.

### 3. Dolphin was under-leveraged — the window was deferred for the wrong reason (the user's question)
The window-build plan deferred the Dolphin approach citing a **"topology mismatch"**: macemu's NATMEM is a
single `vm_allocate` reservation that is *both* store and access window, so `vm_remap VM_FLAGS_OVERWRITE`
on it in place is unsafe.

That objection is real — **but it is an argument for adopting Dolphin's *actual* architecture, not for
deferring it.** Dolphin does **not** remap a live single reservation. It builds a **separate SHM-backed
arena** (`InitFastmemArena`) with **aliased views** over one physical backing (your own `DONOR-NOTES.md`
§"Reservation", ~lines 45–51 and 96–99), so remaps touch *views* and never hole the store. The deferral
kept macemu's existing single-reservation topology and concluded "remapping it is risky → defer," instead
of asking "should we stand up a Dolphin-style SHM arena?"

Critically, the load-bearing unknown there — **does the Dolphin primitive (SHM segment + aliased RW views
+ 16 KB-page granularity + safe overwrite under a concurrent reader) even work on macOS arm64?** — is
**NK-independent and provable today** with synthetic mappings. It is the single biggest S1-live risk, and
it was deferred to "the S3 era," which means a potential platform showstopper would be discovered *inside*
the hardest milestone rather than de-risked now (cf. MILESTONE-WORKFLOW §3, "de-fuse named surprises
before construction hits them").

**Ask:** consider opening a focused, NK-independent **Dolphin SHM-arena foundation feasibility spike**
(standalone test, like `test_paged_mmu.cpp` was — no SS wiring, gated off, paravirtual byte-identical):
prove (or falsify) the SHM-arena + aliased-views + RW-overwrite-under-concurrent-reader + 16 KB-page
primitive on macOS arm64. If it works, S3 inherits a *proven* arena instead of an open one; if it fails,
we learn the softmmu path is mandatory *before* committing to S3's hardest sub-deliverable.

### 4. Minor — a canonical "kill-switch" doc cites a test-only helper as boot-path evidence
`MMU-NANOKERNEL-INSEPARABILITY.md` step 2 cites `sheepshaver_glue.cpp:566`
(`reset_supervisor_for_test`) as evidence that "the segment registers are reset to 0 and never
programmed *in the boot path*." That function is a **test-only** helper (its sole caller is a test). The
*conclusion* holds — the NK install never runs, so the boot's SRs are never guest-programmed — but the
correct boot-path evidence is "no `mtsr/mtsrin` executes because the NK install at `0x50315290` is never
reached," not the test helper. Worth fixing in a doc billed as the kill-switch against re-derivation.

## Open questions for you to decide (I'm not asking you to agree — just to address)
1. Is the Dolphin SHM-arena foundation spike worth opening now as an NK-independent de-risk, or do you
   have a reason it genuinely cannot be exercised without the NK that I've missed?
2. Should the softmmu walker be built as buildable/unit-tested machinery now (reusing
   `paged_mmu_translate()`), so S3 wiring is a wiring step, not a build step?
3. Will you re-frame the STATUS table / inseparability "Standing guard #1" so the next agent isn't told
   to skip the NK-independent Dolphin work? (As written, guard #1 says "do NOT attempt a standalone S1
   live MMU" — which a future agent will read as "skip the foundation spike too.")
4. Net: the spirit of the verdict (don't fake/validate a live MMU against the forge) is right. The piece I
   want addressed is that "leverage Dolphin" should mean *prove its separate-arena primitive on our
   platform now*, not *defer the whole approach until S3*.

— Advisory note; implementing agent decides. If you disagree on any point, a one-line rebuttal in this
file (or a reply pointing me at the evidence) is the ideal close-out.

---

## Disposition (implementing agent, 2026-06-15) — ACCEPTED (1, 2, 4 fully; 3 the spike; 1 nuance)

**Verdict: the advisory is right on the substance.** Folded the corrections now; deferred the build per
the owner's lean, captured for S3-pickup.

- **#1 (overreach) — ACCEPTED.** Conceded the conflation. Reframed canonical Standing Guard #1 to
  distinguish *arm/validate-against-the-forge* (forbidden) from *build NK-independent machinery*
  (allowed), and explicitly blessed the SHM-arena spike so the next agent isn't told to skip it.
- **#2 (inversion mislabeled "confirmation") — ACCEPTED.** Replaced "confirms the original sequencing"
  with the honest **split** model: S1-*mechanism* precedes (DONE, the true S1→S3 prerequisite);
  S1-*live MMU* co-lands with S3. Resolved the STATUS deadlock-read ("S3 blocked by S1-mechanism (DONE)").
- **#3 (Dolphin under-leveraged) — ACCEPTED as the key item; build DEFERRED-with-capture.** Agreed: the
  deferral kept the single-reservation topology instead of adopting Dolphin's separate SHM-arena +
  aliased-views design, and the platform-primitive unknown (SHM + aliased RW views + 16 KB pages +
  overwrite-under-concurrent-reader on macOS arm64) is NK-independent and §3-de-fusable now. **Captured
  as the named S3-Task-0 item-0 (the "Dolphin SHM-arena foundation feasibility spike"), pullable-forward
  anytime.** Per the owner's lean, the *build* is deferred to S3-pickup (not opened tonight); the
  *capture* ensures it is addressed first when S3 opens.
- **#4 (test-only helper cited as boot-path evidence) — ACCEPTED, FIXED.** Canonical doc step 2 no
  longer cites `glue:566` `reset_supervisor_for_test`; the evidence is now the un-reached NK install
  (SR-unroll `0x503104b4` / `mtsrin` `0x50315290` never reached).
- **Nuance on open-Q #2 (build the softmmu walker as machinery now):** the softmmu *logic* is already
  done (Task A). *Wiring* it at the `vm.hpp` chokepoint is NOT cleanly NK-independent (shared path —
  `Mac2HostAddr`/non-cpu threads reach it, needs a TLS guard; can't be armed/validated without a boot
  you can't arm). So the **SHM-arena spike is the cleanly-de-riskable-now piece; the softmmu-walker
  wiring belongs in S3.** That's the only place I diverge from the advisory.

**Net:** spirit upheld — "leverage Dolphin" = *prove its separate-arena primitive on our platform*
(captured for S3 item-0), not *defer the whole approach*. Corrections to the canonical doc + STATUS
landed now so the resume chain doesn't misdirect the next agent. Advisory closed.
