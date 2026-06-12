# Reku Pass — Archive & Accelerate (2026-06)

> **Status:** PROPOSED 2026-06-12 · **Owner:** khawkins · **Effort:** 1 sprint day
> **Goal:** Archive archaeology, slim active docs, switch to light-gear sprint mode
> **Outcome:** Stop spinning, start shipping

---

## Why This Exists

The project is **spinning**. We have:
- **249 markdown files** cluttering the active workspace
- **~60% of recent commits are documentation**, not code
- **LEARNINGS.md at 4,175 lines** — a journal, not a reference
- **VIA-IFR blocked for 3+ sessions** — analysis paralysis
- **Process overhead > implementation** — multi-agent workflow for solo work

We've built something remarkable (native ARM64 JIT, Mac OS 8.6 boots to Finder), but we're **optimizing for the wrong thing**. Time to cull, archive, and accelerate.

---

## Phase 0: Pre-flight Audit (Non-negotiable)

**Before moving any file, update every reference to it.** Moving docs without updating
references is worse than not moving them — a broken `CLAUDE.md` path misdirects every
future agent session.

### CLAUDE.md lines that reference archived files

| Line(s) | File referenced | Action |
|---------|----------------|--------|
| 467, 606 | `docs/planning/machine/VIA-IFR-RECON.md` | Extract §7d → HANDOFF.md, then archive; update both refs to HANDOFF.md |
| 565, 597 | `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` | Update path to `docs/archive/2026-05/` |
| 615 | `docs/planning/UPGRADE-CARD-PATH.md` | Update path to `docs/archive/2026-06/` |
| 617 | `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` | Update path to `docs/archive/2026-06/` |

### VIA-IFR-RECON.md — Option B (extract, then archive)

`VIA-IFR-RECON.md` is referenced **8 times** in `HANDOFF.md` and **3 times** in
`AGENT-CONTEXT.md`. Rather than holding the whole file indefinitely, extract the
active pickup recipe into HANDOFF.md, then archive RECON and update all refs to HANDOFF.

**Extract this into HANDOFF.md** (§7d from VIA-IFR-RECON.md):

> **Boot 1 — baseline PROBE_68K (no patch):** confirm the 68k level-1 handler at 0x5000ed08
> fires at all, without any via_nw901_int modification:
> ```bash
> SS_NW_IRQ_CONSUME=1 SS_NW_PIC=1 \
>   SS_PROBE_68K=0x5000ed08:3 \
>   SheepShaver/tools/ss-slot-boot.sh --label via-ifr-probe-baseline --timeout 20
> ```
> If PROBE_68K fires → DR executes 0xed08 in normal operation; patch is broken or breaks
> something upstream. If PROBE_68K does NOT fire → different problem: handler isn't reaching
> 0xed08 at all.
>
> **Boot 2 — PPC world depth check (with patch):** confirm how far PPC world reaches:
> ```bash
> SS_NW_VIA_IFR=1 SS_NW_IRQ_CONSUME=1 SS_NW_PIC=1 \
>   SS_PROBE_PC=0x50314880:1 \
>   SheepShaver/tools/ss-slot-boot.sh --label via-ifr-ppc-depth --timeout 25
> ```
> 0x50314880 = NK EXT handler entry. If fires: PPC world IS running, Start68k was reached.
> If not: stall is before NK interrupt handling (trampoline or other change broke boot path).

After inserting this block into HANDOFF.md, archive VIA-IFR-RECON.md and update all
references in HANDOFF.md and AGENT-CONTEXT.md to point to HANDOFF.md instead.

---

## Phase 1: Archive Pass (2-3 hours)

### Archive structure

`docs/archive/` already exists with early JIT analysis docs. Extend it:

```
docs/archive/
├── README.md                  (already exists — update to mention 2026-06/ additions)
├── 2026-05/
│   └── BasiliskII-AARCH64_JIT_BRINGUP.md
└── 2026-06/
    ├── machine/               (completed milestone recon)
    ├── superpowers/plans/     (all shipped plans)
    ├── superpowers/specs/     (all shipped specs)
    ├── superpowers/research/  (historical research)
    └── planning/              (superseded planning docs)
```

### Archive header banner

Every archived file **must** start with:
```markdown
> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** [milestone complete / superseded by X / historical reference]
>
```

### Files to archive

#### Machine recon — completed milestones

| Source | Destination | Reason |
|--------|-------------|--------|
| `docs/planning/machine/DISK-PATH-RECON.md` | `docs/archive/2026-06/machine/` | M5 complete |
| `docs/planning/machine/DSAT-WALL-RECON.md` | `docs/archive/2026-06/machine/` | M6a shipped |
| `docs/planning/machine/EE-CHAIN-RECON.md` | `docs/archive/2026-06/machine/` | M7 shipped |
| `docs/planning/machine/TRAP-TABLE-RECON.md` | `docs/archive/2026-06/machine/` | SysError-12 resolved |
| `docs/planning/machine/SLIDE-WALL-RECON.md` | `docs/archive/2026-06/machine/` | M6a rung 2 shipped |
| `docs/planning/machine/M6A-ONGOING-ENTRY-DESIGN.md` | `docs/archive/2026-06/machine/` | Superseded by shipped design |
| `docs/planning/machine/M6A-DR-HANDOFF-ANALYSIS.md` | `docs/archive/2026-06/machine/` | M6a shipped |
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` | `docs/archive/2026-06/machine/` | M6a shipped |
| `docs/planning/machine/M3-INTERRUPT-RECON.md` | `docs/archive/2026-06/machine/` | M3 shipped |
| `docs/planning/machine/M3-PIC-CUDA-DONOR-STUDY.md` | `docs/archive/2026-06/machine/` | M3 shipped |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | `docs/archive/2026-06/machine/` | M3a shipped |
| `docs/planning/machine/M5-MMU-SR-WALL-ANALYSIS.md` | `docs/archive/2026-06/machine/` | M5 shipped |
| `docs/planning/machine/INTERRUPT-INJECTION-RECON.md` | `docs/archive/2026-06/machine/` | M8 shipped |
| `docs/planning/machine/FRAMEBUFFER-RECON.md` | HOLD | Verify M5 framebuffer status first |

**Keep active:** `CORE99-MACHINE-DESCRIPTION.md`, `M1-DEVICE-CONFORMANCE.md`, `ROM-PATCH-AUDIT.md` — still reference material for ongoing work.

#### Superpowers plans — all shipped milestones

| Source | Destination | Reason |
|--------|-------------|--------|
| `docs/superpowers/plans/2026-06-01-phase1-macos-baseline.md` | `docs/archive/2026-06/superpowers/plans/` | milestone complete |
| `docs/superpowers/plans/2026-06-01-phase2-wx-and-addressing.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-03-per-instance-diag-log.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-04-e2e-vnc-harness-p1.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-05-benchmark-result-export.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-06-guest-ui-introspection-p1-skeleton.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-06-guest-ui-introspection-p2a-dialog-items.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-06-guest-ui-introspection-p2b-control-state.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-06-guest-ui-introspection-p2c-menus.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-machine-layer-m0.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-machine-layer-m1.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-machine-layer-m2.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-machine-layer-m3a.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-m6a-wave1-dr-dispatch.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-10-mmu-sr-wall-wave0.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-68k-pc-desync.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-fe1f-service-surface.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-m6a-rung2-mixedmode-switch.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-machine-layer-m3b.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-nk-syscall-surface.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-11-wave2-interrupt-chain.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-12-interrupt-injection.md` | same | milestone complete |
| `docs/superpowers/plans/2026-06-12-slot4-consumption.md` | same | milestone complete |
| `docs/superpowers/plans/m1-task6-recon.md` | same | milestone complete |
| `docs/superpowers/plans/m1-task9-thunk-encodings.md` | same | milestone complete |

#### Superpowers specs and research (all shipped)

All of `docs/superpowers/specs/` → `docs/archive/2026-06/superpowers/specs/`
All of `docs/superpowers/research/` → `docs/archive/2026-06/superpowers/research/`

#### Superseded planning docs

| Source | Destination | Reason |
|--------|-------------|--------|
| `docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` | `docs/archive/2026-06/planning/` | Superseded approach (Path A) |
| `docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md` | `docs/archive/2026-06/planning/` | Superseded by Machine Layer |
| `docs/planning/UPGRADE-CARD-PATH.md` | `docs/archive/2026-06/planning/` | Superseded (Path B) |
| `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` | `docs/archive/2026-05/` | BasiliskII deferred |

**Also archive now** (status confirmed from file headers; none referenced in active docs):

| File | Verdict |
|------|---------|
| `docs/planning/CHAINING-VERIFICATION-PLAN.md` | Archive — status header: "✅ Resolved / superseded" |
| `docs/planning/JIT-APPROACH-RESET.md` | Archive — status header: "✅ Decided — standing guidance" (the guidance is baked into CLAUDE.md already) |
| `docs/planning/MMU-DEFERRAL-REDTEAM.md` | Archive — decision memo, no pending actions |
| `docs/planning/MMU-NANOKERNEL-MP-PLAN.md` | Archive — "⏸ Deferred — never, unless specific trigger" |
| `docs/planning/sheepshaver-research/research/` (all except `IMPLEMENTATION-BACKLOG.md`) | Archive — historical research, not referenced |
| `docs/planning/autoresearch/` (all) | Archive — auto-generated reports, not referenced |

**Keep:** `docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` — referenced in ROADMAP.md and CLAUDE.md.

---

## Phase 2: Slim Active Docs (1-2 hours)

Target sizes are ceilings, not goals. Don't trim content that prevents re-deriving
the same lesson.

### HANDOFF.md → ≤80 lines

**Keep:**
- Current frontier paragraph (the VIA-IFR boot stall)
- The two-boot recon recipe (§7d from VIA-IFR-RECON.md — copy it here, since RECON is
  being held but HANDOFF is the pickup point)
- The three open questions table (what sets `$0d94`; handler at 0x64; `$6e4` chain)
- Verification criteria

**Remove:** Reading-order tables, idea queues, operational notes. Those belong in
CONTRIBUTING.md or the specific planning doc.

### ROADMAP.md → ≤300 lines

**Keep:** Active milestones only (next 3-5). Short status header.
**Remove:** All completed milestone narratives (they're in the archived plans).
**Move:** Idea queue / future backlog → `docs/planning/BACKLOG.md` (new, minimal).

### AGENT-CONTEXT.md — trim history, keep instruments

**Keep intact:**
- "Current frontier" section
- "Boot recipes" section
- "Constants" table (ECB/MMCB/KDP/XLM/mirror bases) — ~40 lines, prevents 20min re-derivation
- "Probe-ready instrument" quick reference

**Archive:** History-of-decisions prose, "why we chose X" rationale blocks. Those are in
the individual milestone plans (now archived).

### LEARNINGS.md → ≤400 lines

1. Copy full current content to `docs/archive/2026-06/LEARNINGS-2026-06.md`
2. **Keep in active `LEARNINGS.md`** only entries that prevent repeating an expensive mistake:
   - Session 5 RETRACTION (HOT-PC / STUCK are sampling artifacts — not hangs)
   - VBL timer death from repeated lldb attach/detach
   - AltiVec bottom line (⭐ section)
   - QEMU rig pitfalls (5 bullets from "2026-06-12" entry)
   - "Rule out boring causes first" (wrong coordinates / noisy signals)
3. Add header: `> For historical session logs, see docs/archive/2026-06/LEARNINGS-2026-06.md`

---

## Phase 3: CLAUDE.md Cleanup (do this last, atomically with archive commit)

Update these specific lines to reflect moved paths:

```
Line 565/597: BasiliskII/docs/AARCH64_JIT_BRINGUP.md
  → docs/archive/2026-05/BasiliskII-AARCH64_JIT_BRINGUP.md

Line 606: docs/planning/machine/VIA-IFR-RECON.md  (in QEMU rig table entry)
  → update AFTER VIA-IFR fix ships

Line 615: docs/planning/UPGRADE-CARD-PATH.md
  → docs/archive/2026-06/planning/UPGRADE-CARD-PATH.md

Line 617: docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md
  → docs/archive/2026-06/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md
```

Also trim the Key Documentation table: remove entries for archived docs (they're no
longer daily-use references), and update the row for `BasiliskII/docs/AARCH64_JIT_BRINGUP.md`
to note it's archived.

---

## Phase 4: Light-Gear Sprint Rules (Ongoing)

### Rule 0: No new recon docs

- **Allowed:** Session notes in `/tmp/session-YYYY-MM-DD.md` (never committed)
- **At end of session:** One finding moves into an active doc; the rest is deleted
- **Never:** Commit session logs

### Rule 1: Milestones are 3 bullets max

```markdown
## M9: VIA-IFR → 68k ticks

- [ ] Fix: Two-boot recon (§7d recipe) → if probe fires, add EMUL_OP_IRQ at 0xed08 + rte
- [ ] Verify: SS_PROBE_68K=0x5000ed08 fires, boot advances past PROGRAM#5 park
- [ ] Ship: One-line CHANGELOG entry
```

### Rule 2: 5-line fix discipline

Before any investigation:
1. Can this be solved with a ≤5-line code change? If yes → do it
2. Can this be bracketed with a ≤5-line probe/gate? If yes → do it
3. Only if both "no" → deeper investigation allowed, max 30 minutes before booting a VM

### Rule 3: When in doubt, boot it

Stop reasoning from static docs. Start reasoning from live data.

**If you've spent 30 minutes on a problem without new data, you must launch a VM.**

Cost of a boot: ~30 seconds. Cost of a wrong assumption: ~3 hours.

#### Tooling cheat sheet

| Need | Command |
|------|---------|
| Quick headless test | `ss-slot-boot.sh --label test1 --timeout 30` |
| Probe at a PC | `SS_PROBE_PC=0x503xxxxx:r1,r8 ss-slot-boot.sh ...` |
| Probe 68k handler | `SS_PROBE_68K=0x5000xxxx:N ss-slot-boot.sh ...` |
| Behavioral oracle | `SheepShaver/tools/qemu-rig.sh --timeout 50` then `qemu-mon.py` |
| Parallel A/B | Two terminals, two slot labels — concurrent boots are proven safe |
| Cleanup strays | `SheepShaver/tools/ss-reap.sh` |

#### QEMU rig is your first tool, not your last resort

When asking "what does the guest do here?":
1. **QEMU rig** — ground truth in 5-10s
2. Static disassembly — when QEMU can't reach the site
3. Reasoning from first principles — only if the above fail

#### Parallel testing strategy

```bash
# Run these concurrently — 4 data points in the time of 1 serial boot:
ss-slot-boot.sh --label baseline --timeout 20          # control
SS_NW_VIA_IFR=1 ss-slot-boot.sh --label gate --timeout 20    # treatment
SS_PROBE_68K=0x5000ed08:5 ss-slot-boot.sh --label probe --timeout 20  # instrument
SheepShaver/tools/qemu-rig.sh --timeout 50             # oracle
```

### Rule 4: Commit attribution

Normal commits: no attribution lines.
Do not add "Co-Authored-By" for AI tools — it implies the AI has authorship in git history.

---

## Execution Script

Run from repo root. Does Phase 1 only — Phase 2 (slim docs) is manual.

```bash
#!/bin/bash
set -e
cd "$(git rev-parse --show-toplevel)"

# Create archive subdirs
mkdir -p docs/archive/2026-06/machine
mkdir -p docs/archive/2026-06/superpowers/plans
mkdir -p docs/archive/2026-06/superpowers/specs
mkdir -p docs/archive/2026-06/superpowers/research
mkdir -p docs/archive/2026-06/planning
mkdir -p docs/archive/2026-05

add_archive_header() {
  local file="$1" reason="$2"
  { printf '> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.\n'
    printf '> May contain outdated assumptions, resolved questions, or superseded plans.\n'
    printf '> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`\n'
    printf '> **Reason:** %s\n>\n\n' "$reason"
    cat "$file"
  } > "$file.tmp" && mv "$file.tmp" "$file"
}

move_if_exists() {
  local src="$1" dst="$2" reason="$3"
  if [ -f "$src" ]; then
    git mv "$src" "$dst"
    add_archive_header "$dst/$(basename "$src")" "$reason"
    echo "  archived: $src"
  else
    echo "  SKIP (not found): $src"
  fi
}

echo "=== Machine recon ==="
for f in DISK-PATH-RECON DSAT-WALL-RECON EE-CHAIN-RECON TRAP-TABLE-RECON \
          SLIDE-WALL-RECON M6A-ONGOING-ENTRY-DESIGN M6A-DR-HANDOFF-ANALYSIS \
          M6A-WAVE2-SHIM-RECON M3-INTERRUPT-RECON M3-PIC-CUDA-DONOR-STUDY \
          M3A-ENTRY-TABLE M5-MMU-SR-WALL-ANALYSIS INTERRUPT-INJECTION-RECON; do
  move_if_exists "docs/planning/machine/$f.md" "docs/archive/2026-06/machine" "milestone complete"
done

echo "=== Superpowers plans ==="
for f in \
  2026-06-01-phase1-macos-baseline 2026-06-01-phase2-wx-and-addressing \
  2026-06-03-per-instance-diag-log 2026-06-04-e2e-vnc-harness-p1 \
  2026-06-05-benchmark-result-export \
  2026-06-06-guest-ui-introspection-p1-skeleton \
  2026-06-06-guest-ui-introspection-p2a-dialog-items \
  2026-06-06-guest-ui-introspection-p2b-control-state \
  2026-06-06-guest-ui-introspection-p2c-menus \
  2026-06-10-machine-layer-m0 2026-06-10-machine-layer-m1 \
  2026-06-10-machine-layer-m2 2026-06-10-machine-layer-m3a \
  2026-06-10-m6a-wave1-dr-dispatch 2026-06-10-mmu-sr-wall-wave0 \
  2026-06-11-68k-pc-desync 2026-06-11-fe1f-service-surface \
  2026-06-11-m6a-rung2-mixedmode-switch 2026-06-11-machine-layer-m3b \
  2026-06-11-nk-syscall-surface 2026-06-11-wave2-interrupt-chain \
  2026-06-12-interrupt-injection 2026-06-12-slot4-consumption \
  m1-task6-recon m1-task9-thunk-encodings; do
  move_if_exists "docs/superpowers/plans/$f.md" "docs/archive/2026-06/superpowers/plans" "milestone complete"
done

echo "=== Superpowers specs ==="
for f in docs/superpowers/specs/*.md; do
  [ -f "$f" ] && move_if_exists "$f" "docs/archive/2026-06/superpowers/specs" "milestone shipped"
done

echo "=== Superpowers research ==="
for f in docs/superpowers/research/*.md; do
  [ -f "$f" ] && move_if_exists "$f" "docs/archive/2026-06/superpowers/research" "historical research"
done

echo "=== Superseded planning docs ==="
move_if_exists "docs/planning/HANDOFF-NEWWORLD-SUPERVISOR-MMU.md" "docs/archive/2026-06/planning" "superseded by Machine Layer (Path A)"
move_if_exists "docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md"      "docs/archive/2026-06/planning" "superseded by Machine Layer"
move_if_exists "docs/planning/UPGRADE-CARD-PATH.md"               "docs/archive/2026-06/planning" "superseded (Path B dead-end)"
move_if_exists "docs/planning/CHAINING-VERIFICATION-PLAN.md"      "docs/archive/2026-06/planning" "resolved / superseded"
move_if_exists "docs/planning/JIT-APPROACH-RESET.md"              "docs/archive/2026-06/planning" "guidance baked into CLAUDE.md"
move_if_exists "docs/planning/MMU-DEFERRAL-REDTEAM.md"            "docs/archive/2026-06/planning" "decision memo, no pending actions"
move_if_exists "docs/planning/MMU-NANOKERNEL-MP-PLAN.md"          "docs/archive/2026-06/planning" "deferred indefinitely"
move_if_exists "BasiliskII/docs/AARCH64_JIT_BRINGUP.md"           "docs/archive/2026-05"           "BasiliskII deferred"

echo "=== Research and autoresearch ==="
mkdir -p docs/archive/2026-06/sheepshaver-research-research
for f in docs/planning/sheepshaver-research/research/*.md; do
  base=$(basename "$f")
  [ "$base" = "IMPLEMENTATION-BACKLOG.md" ] && continue  # keep — referenced in ROADMAP
  [ -f "$f" ] && move_if_exists "$f" "docs/archive/2026-06/sheepshaver-research-research" "historical research"
done
mkdir -p docs/archive/2026-06/autoresearch
for f in docs/planning/autoresearch/*.md; do
  [ -f "$f" ] && move_if_exists "$f" "docs/archive/2026-06/autoresearch" "auto-generated, not referenced"
done

# VIA-IFR: extract §7d to HANDOFF.md FIRST (manually), then:
# move_if_exists "docs/planning/machine/VIA-IFR-RECON.md" "docs/archive/2026-06/machine" "active blocker recipe extracted to HANDOFF.md"

MOVED=$(git diff --cached --name-only | grep -c '\.md$' || echo 0)
echo ""
echo "✅ Phase 1 complete."
echo "  Files moved to archive: ${MOVED}"
echo "  Before: ~249 active markdown files"
echo "  After Phase 1: ~$((249 - MOVED)) active markdown files"
echo "  (Target after Phase 2: ≤40)"
echo ""
echo "Next steps:"
echo "  1. Manually insert §7d recipe into HANDOFF.md (see Phase 0)"
echo "  2. Then uncomment the VIA-IFR move line above and re-run"
echo "  3. Update CLAUDE.md lines 467/565/597/606/615/617 (Phase 3)"
echo "  4. Slim HANDOFF.md, ROADMAP.md, AGENT-CONTEXT.md, LEARNINGS.md (Phase 2)"
echo "  5. Commit: git commit -F- <<'EOF'"
echo "     docs: Reku pass — archive completed milestones, slim active docs"
echo "     EOF"
```

---

## Post-Reku State

### What stays active

| File | Purpose | Target size |
|------|---------|-------------|
| `docs/HANDOFF.md` | Current blocker + pickup recipe | ≤80 lines |
| `docs/planning/ROADMAP.md` | Next 3-5 milestones | ≤300 lines |
| `docs/AGENT-CONTEXT.md` | Frontier + boot recipes + constants | ≤200 lines |
| `LEARNINGS.md` | Durable failure-mode patterns | ≤400 lines |
| `SheepShaver/docs/DIAGNOSTICS.md` | Knobs + instruments | Keep as-is |
| `docs/CONTRIBUTING.md` | Contribution guide | Keep, trim lifecycle section |
| `docs/MILESTONE-WORKFLOW.md` | Process | Keep; add "Solo Mode" section |
| `docs/planning/machine/VIA-IFR-RECON.md` | **Active blocker** | Keep until fix ships |
| `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` | Reference | Keep |
| `docs/planning/machine/M1-DEVICE-CONFORMANCE.md` | Ongoing M1 work | Keep |
| `docs/planning/machine/ROM-PATCH-AUDIT.md` | Patch tracking | Keep |

### Metrics target

| Metric | Before | After Phase 1 | After Phase 2 |
|--------|--------|---------------|---------------|
| Active markdown files | ~249 | ~180 | ~40 |
| Lines in active docs | ~20,000 | ~15,000 | ~3,500 |

---

## Immediate Next Actions

### Today (Archive Day)
1. Run the Phase 1 script above
2. Update CLAUDE.md references (Phase 3, except VIA-IFR lines)
3. Slim HANDOFF.md, ROADMAP.md, AGENT-CONTEXT.md, LEARNINGS.md (Phase 2)
4. Commit with a plain message (no AI attribution lines)

### Tomorrow (Sprint Day)
Run the VIA-IFR two-boot recon:
```bash
# Boot 1: baseline
SS_NW_IRQ_CONSUME=1 SS_NW_PIC=1 SS_PROBE_68K=0x5000ed08:10 \
  SheepShaver/tools/ss-slot-boot.sh --label via-ifr-baseline --timeout 20

# Boot 2: with VIA-IFR gate
SS_NW_VIA_IFR=1 SS_NW_IRQ_CONSUME=1 SS_NW_PIC=1 SS_PROBE_68K=0x5000ed08:10 \
  SheepShaver/tools/ss-slot-boot.sh --label via-ifr-gate --timeout 20
```

If probe fires → boot advances → CHANGELOG entry → archive VIA-IFR-RECON.md.
If probe doesn't fire → apply EMUL_OP_IRQ patch at ROM offset 0xed08 + rte, re-run.

---

## Success Criteria (30 Days)

- [ ] Archive pass complete
- [ ] VIA-IFR unblocked (68k handler at 0x5000ed08 reached, boot advances)
- [ ] M5 framebuffer (pixels on screen)
- [ ] 3+ weekly tags shipped
- [ ] Zero new recon docs committed
- [ ] Velocity: spinning → moving

---

*Execute the Reku pass, then archive this doc too.*
