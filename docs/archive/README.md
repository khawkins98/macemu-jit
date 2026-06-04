# `docs/archive/` — retired one-off artifacts

> **Status:** 📖 Archive · **Created:** 2026-06-04 · **Updated:** 2026-06-04
> **Why this doc exists:** Index of point-in-time session/PR artifacts kept for the record.
> _These are **not** living docs — their conclusions already live in `LEARNINGS.md` /
> `CHANGELOG.md` / the trackers. Don't update them; cite the canonical doc instead._

Retired here (rather than deleted) so the original investigation survives. Each was a
one-off output of a single session or PR, superseded by the durable record.

All three describe **resolved** issues (or a drifting snapshot). The first two are two angles on
the *same* now-fixed bug — the 68K DR-emulator SCSI/boot hang, root-caused to the `subfe`/`adde`
carry-out miscompile and fixed via the single-`ADCS` codegen (`ppc-jit.cpp` cases 136/138;
OPTIMIZATION-PLAN §0b "DONE"). SheepShaver now boots Mac OS 8.6 to Finder with the full DR region
JIT-compiled.

| File | What it was | Status / durable record |
|---|---|---|
| [`0x50467E00-CODEGEN-BUG-ANALYSIS.md`](0x50467E00-CODEGEN-BUG-ANALYSIS.md) | **Superseded** investigation of the DR-emulator SCSI/boot hang (2026-06-03) — hypothesized `rlwimi`/`bcctr`/`lhau`; none confirmed | ✅ Resolved, but by a *different* cause: the real culprit (`subfe`/`adde`) was found later — see the next row + `LEARNINGS.md` |
| [`PR-SUBFE-FIX-DRAFT.md`](PR-SUBFE-FIX-DRAFT.md) | Draft upstream PR body for the actual fix — `subfe`/`adde` carry-out | ✅ Shipped — `ppc-jit.cpp` cases 136/138 (ADCS), OPTIMIZATION-PLAN §0b, `CHANGELOG.md`; root-cause detail in `docs/SUBFE-CARRY-BUG-REPORT.md` |
| [`JIT-OPCODE-TABLE.md`](JIT-OPCODE-TABLE.md) | Generated opcode-coverage snapshot (2026-04-21, "1603/1605") | Drifting snapshot — live count is `make harness-count`, never a hardcoded table |
