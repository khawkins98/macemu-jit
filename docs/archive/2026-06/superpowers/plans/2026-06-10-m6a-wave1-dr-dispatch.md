> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# M6a Wave 1 — DR-Emulator dispatch repair (the three-constant fix + the user-MSR handoff)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land rung 1 of `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md`: repair the DR-Emulator dispatch-table contract so the 68k boot world survives its first opcode — the mirror-world dispatch base becomes the dump-verified table at `0x50480000` everywhere the scaffolding seeds it, and the handoff performs the dropped UserModeMSR (`0x0000D032`, EE=1) transition that the jump68k redirect lost. Acceptance = the memo's PROBE-3 criteria: zero visits at the console help-text printer `0x504277d0`, first-compiles in region `0x50480000`, `jDR > 8`, the compile counter unfrozen — and capture the next 68k stall (it names the first shim for Wave 2). Paravirtual byte-identical (every touched site is newworld/`g_rom_904_lenient` scaffolding).

**Architecture:** No new components — this wave only corrects existing scaffolding constants and adds one MSR write at the handoff. The memo (read it FIRST) is the spec; this plan sequences it. **(rev 2 finding 1)** The UserModeMSR write is **env-gated `SS_M6A_USER_MSR=1`, default OFF**: the red team traced that EE=1 during 68k execution routes the first DEC delivery through UNVERIFIED save/restore plumbing (the NK handler's full-context save through r6 — the ctx+0xfc/+0x1ec offsets are its *interrupt save area*, dynamically overwritten — plus the always-cold table[0] re-entry that resets guest[0]/[4]); the first round-trip plausibly destroys 68k state and would mask PROBE-3's stall capture. Acceptance therefore splits: **Boot A (MSR off)** = the clean stall capture; **Boot B (MSR on)** = the recorded A/B of the first live EE-enabled delivery (rung-2 recon, failure expected and informative).

**Rev 2 corrections (red-team round, verdict GO):** geometry/table/sites/gates/0xD032 all independently re-verified (incl. slot 0x4efa → `b 0x367c64` decoded from the dump; table 131072/131072 populated; the bad-r29 landing = the Thud help printer confirmed). Folded: (1) the MSR env gate above; (2) acceptance criterion fixed — first-compile logging is per-64KB chunk, so assert "first-compiles in `0x50480000–0x504fffff`" (op 0x4efa's slot is in chunk 0x504a0000) + jDR/comp; (3) paravirtual e2e smoke added to Task 1 gates (the memo's rung-1 row requires it); (4) attribution hygiene — land all four constants but bisect r29-first if PROBE-3 misbehaves; comment that ctx+0xfc/+0x1ec are cold-boot seeds the NK overwrites at its first interrupt save; (5) the MSR-write site is the implementer's documented choice — trampoline-side `lwz rX,-0x964(r1); mtmsr rX` is preferred (rides the existing execute_mtmsr EE-edge re-raise); (6) note the dormant cross-world constants ([KDP+0x5a4]=0x5036d218, [KDP+0x5f0/4]=primary EMUL_RETURN) so rung 2 doesn't trip on them; (7) [KDP+0x648] is architecturally the entry-VECTOR table (NK writer computes LA_EmulatorCode+0x84) — fix per the memo, it is wrong on both identity and world today.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md` (THE spec — read fully) | Table geometry (`handler = (r29 & 0xFFF80007) \| (opcode<<3)`), the three inconsistent constants + their exact sites, the 0x50480000 dump verification, the 0x310000+8 skip coupling, PROBE-1..5 |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` (+ Task 7 + carry-forwards) | The live facts + forensics this wave builds on |
| `docs/planning/PATCH-68K-SHIM-INVENTORY.md`, HANDOFF §2.8 | Wave-2 context only (the shim desert) — NOT this wave's scope |

## Codebase facts (from the memo + forensics; implementer re-verifies sites before editing)

- **The fixup trampoline** (`rom_patches.cpp` ~:741 region, the NW-trampoline DR-entry redirect): seeds `r29 = 0x5046e000` — WRONG (no table there; mirror NK code). Correct: `0x50480000` (`LA_DispatchTable`, ConfigInfo-consistent, dump-verified fully populated).
- **The glue ECB seeds** (`[NW-TRAMP]` block, sheepshaver_glue.cpp): the ECB/context fields the memo names (ctx+0x1ec / ctx+0xfc family — the memo's environment-needs table has the exact offsets and current-vs-required values) and the `[KDP+0x648]` dispatch seed (currently `0x50380000` = the PRIMARY world's table; the memo says which value the MIRROR world path needs — follow it, do not guess).
- **The user-MSR transition**: the unpatched ROM's jump68k does `mtsrr0; mtsrr1; rfi` with UserModeMSR `0x0000D032`; the Path-A redirect dropped the `mtsrr1` — post-M3a (real MSR), the 68k world must enter with `msr = 0x0000D032`. The memo names where the transition belongs (the fixup-trampoline handoff site — guest-side patch instructions or host-side seed; follow the memo's recommendation).
- **The first-opcode death**: `0x4efa` at `0x5000002a` currently dispatches to `0x504277d0` (console help-text printer) — the PROBE-3 "gone" marker.
- **io_poll_beq NOPs + patch_68k shims are Wave 2+** — explicitly out of scope here.
- **Probe limitations**: chained blocks are probe-invisible — acceptance probes use `SS_JIT_NO_CHAIN=1` where attribution matters (memo PROBE-1/2 notes).
- **Telemetry follow-up bundled here (small)**: add the MMIO region counters to the `[HB]` heartbeat (same NULL-suffix idiom as `exc=`), closing the alarm-kill blind spot PROBE-5 is blocked on.

## Tasks

### Task 1: The three-constant repair + user-MSR write
- [ ] Read the memo's contract section; re-verify each site in code (`rom_patches.cpp` fixup trampoline; glue `[NW-TRAMP]` ECB/KDP seeds) and the 0x50480000 table in the dump (16-word spot check).
- [ ] Apply: `r29` seed → `0x50480000`; the glue ctx/KDP seeds per the memo's required-values column; the UserModeMSR `0x0000D032` transition at the handoff per the memo's recommendation. Every edit cites the memo section in a comment. All inside existing newworld/lenient gating — confirm each site's gate before editing.
- [ ] Gates: `make build-ss`; batch + legacy `make test-jit` 353/353; `make -C src/machine test` 9/9.
- [ ] Commit.

### Task 2: MMIO counters in the heartbeat (telemetry follow-up)
- [ ] Extend the heartbeat `extra` suffix (the `exc=` idiom, jit-heartbeat `extra` param) with compact MMIO region read counts (newworld only; paravirtual lines byte-identical). Unit-test-free (formatting only); gates as Task 1.
- [ ] Commit.

### Task 3: Acceptance — PROBE-3 (boots authorized)
- [ ] Boot the diagnostic config; assert: zero visits at `0x504277d0` (probe), first-compiles in region `0x50480000`, `jDR > 8` and climbing, comp unfrozen past 781, `[EXC]` deliveries plausibly > 0 once EE rises with the 68k world (record the count — first live EE-enabled delivery on the boot path). Record the NEXT stall signature (PC region, heartbeat shape, MMIO counters now visible) — it names Wave 2's first shim.
- [ ] If the dispatch still lands wrong: PROBE-1/PROBE-2 from the memo (`SS_JIT_NO_CHAIN=1`), iterate on the constants via the memo's geometry — do not guess beyond one iteration; record and stop (the memo's honest-residue section anticipates the re-entry contract may bite immediately — that is Wave 2's "ongoing-entry" item, not a Wave-1 failure).
- [ ] Commit instrumentation if any; record results in the memo (§ "Wave 1 results").

### Task 4: Docs
- [ ] Memo results section; MACHINE-LAYER-PLAN M6 row note (M6a Wave 1 landed; Wave 2 = ongoing-entry + io_poll retirement + boot-path shims); CHANGELOG; LEARNINGS if non-obvious. Commit.

## Self-review record
Scope is deliberately minimal: constants + one MSR write + one telemetry line. The big-ticket M6a items (ongoing-entry contract, io_poll retirement, the shim wave) are Wave 2+, scoped AFTER PROBE-3 names the real next wall. Stop-rule: one constants-iteration maximum in Task 3.

## Red-team record
_To be filled._
