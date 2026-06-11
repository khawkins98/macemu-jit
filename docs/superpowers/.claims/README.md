# File claims — multi-agent commit guard protocol

When multiple agents work the tree concurrently, each task's owner **claims** the files
it will commit. The claim is an ephemeral file here; a pre-commit hook
(`tools/check-claims.sh`) refuses commits that stage paths claimed by someone else.

## Claim file format

`<task-name>.claim` (any name ending in `.claim`):

```
stream-D-tooling
# paths this task will commit (repo-relative, one per line)
tools/check-claims.sh
tools/install-claims-hook.sh
docs/superpowers/.claims/README.md
```

- **Line 1** = the label identifying the owner (matched against `$CLAIM_LABEL`).
- **Lines 2+** = repo-relative paths, one per line. Blank lines and `#` comments OK.
- Exact path match only (no globs) — list every file.

## Lifecycle

1. **Task start**: coordinator (or the agent) writes `<task>.claim` listing the
   deliverable paths, label = the task identity.
2. **While working**: the agent commits with `CLAIM_LABEL=<task>` in the environment;
   the hook passes for its own claims, fails loudly on anyone else's.
3. **Final commit**: delete the claim file (it is gitignored — never committed).

## Semantics

- `CLAIM_LABEL` set: committing a path claimed under a *different* label fails.
- `CLAIM_LABEL` unset ("no identity"): committing ANY claimed path fails.
- No claims dir / no `.claim` files: the hook is a silent no-op.

## Install / scope

```bash
tools/install-claims-hook.sh    # idempotent; chains an existing pre-commit hook
```

The hook is **advisory armor** — the coordinator's serialization of commits remains
the primary protection. Manual check: `tools/check-claims.sh [paths...]` (no args =
currently staged paths). Claims are ephemeral and gitignored (`.claims/*.claim`);
only this README is tracked.
