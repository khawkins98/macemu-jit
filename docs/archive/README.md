# `docs/archive/` — retired one-off artifacts

> **Status:** 📖 Archive · **Created:** 2026-06-04 · **Updated:** 2026-06-12 (Reku pass)
> **Why this doc exists:** Index of point-in-time session/PR artifacts kept for the record.
> _These are **not** living docs — their conclusions already live in `LEARNINGS.md` /
> `CHANGELOG.md` / the trackers. Don't update them; cite the canonical doc instead._

Retired here (rather than deleted) so the original investigation survives. Each was a
one-off output of a single session or PR, superseded by the durable record.

The first three describe the **same resolved bug** — the 68K DR-emulator SCSI/boot hang,
root-caused to the `subfe`/`adde` carry-out miscompile and fixed via the single-`ADCS` codegen
(`ppc-jit.cpp` cases 136/138; OPTIMIZATION-PLAN §0b "DONE"). SheepShaver now boots Mac OS 8.6 to
Finder with the full DR region JIT-compiled. The last is a drifting coverage snapshot.

| File | What it was | Status / durable record |
|---|---|---|
| [`0x50467E00-CODEGEN-BUG-ANALYSIS.md`](0x50467E00-CODEGEN-BUG-ANALYSIS.md) | **Superseded** investigation of the DR-emulator SCSI/boot hang (2026-06-03) — hypothesized `rlwimi`/`bcctr`/`lhau`; none confirmed | ✅ Resolved, but by a *different* cause: the real culprit (`subfe`/`adde`) was found later — see the rows below + `LEARNINGS.md` |
| [`PR-SUBFE-FIX-DRAFT.md`](PR-SUBFE-FIX-DRAFT.md) | Draft upstream PR body for the actual fix — `subfe`/`adde` carry-out | ✅ Shipped — `ppc-jit.cpp` cases 136/138 (ADCS), OPTIMIZATION-PLAN §0b, `CHANGELOG.md` |
| [`SUBFE-CARRY-BUG-REPORT.md`](SUBFE-CARRY-BUG-REPORT.md) | Detailed root-cause report for the `subfe`/`adde` carry-out bug | ✅ Resolved — the durable root-cause record for the fix above |
| [`JIT-OPCODE-TABLE.md`](JIT-OPCODE-TABLE.md) | Generated opcode-coverage snapshot (2026-04-21, "1603/1605") | Drifting snapshot — live count is `make harness-count`, never a hardcoded table |

## 2026-06 Reku pass additions

Archived 2026-06-12 during the Reku archive & accelerate pass. Each file has an archive
banner. The full journal is in `2026-06/LEARNINGS-2026-06.md`.

| Directory | Contents |
|-----------|----------|
| `2026-05/BasiliskII-AARCH64_JIT_BRINGUP.md` | Full 68K JIT bringup history (BasiliskII deferred) |
| `2026-06/machine/` | Completed milestone recon docs (M3–M8, VIA-IFR) |
| `2026-06/superpowers/plans/` | All shipped milestone plans (M0–M8 + supporting) |
| `2026-06/superpowers/specs/` | All shipped design specs |
| `2026-06/planning/` | Superseded planning docs (Path A, Path B, MMU deferral, etc.) |
| `2026-06/sheepshaver-research-research/` | Historical research leads (except IMPLEMENTATION-BACKLOG.md) |
| `2026-06/LEARNINGS-2026-06.md` | Full pre-Reku session journal (4175 lines) |
