#!/bin/bash
# check-claims.sh — claims-file commit guard (advisory armor for multi-agent work).
#
# Reads docs/superpowers/.claims/*.claim files. Claim file format:
#   line 1:  label identifying the owner (e.g. "stream-D-tooling")
#   lines 2+: repo-relative paths, one per line; blank lines and # comments ignored
#
# Given staged paths (as arguments, or computed via `git diff --cached --name-only`),
# FAIL (exit 1) if any staged path appears in a claim whose label differs from
# $CLAIM_LABEL. If CLAIM_LABEL is unset, the committer has no identity: fail on ANY
# claimed path. No claims dir / no claim files = exit 0 silently.
#
# Additionally (post-mortem, the PROBE68K-sweep case): when claims are live but a
# staged file is claimed by NOBODY, emit a WARNING (exit 0) — the staged diff is
# uncommitted work that no claim vouches for, and may include another agent's
# in-flight edits. Verify the diff is all yours before committing.
#
# Protocol: docs/superpowers/.claims/README.md. The coordinator's serialization
# remains primary; this hook is a backstop against accidental cross-agent commits.

set -u

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
CLAIMS_DIR="$REPO_ROOT/docs/superpowers/.claims"

# No claims dir => nothing to guard.
[ -d "$CLAIMS_DIR" ] || exit 0

# Collect claim files; none => nothing to guard.
claim_files=()
for f in "$CLAIMS_DIR"/*.claim; do
    [ -e "$f" ] && claim_files+=("$f")
done
[ "${#claim_files[@]}" -gt 0 ] || exit 0

# Staged paths: from args if given, else from the index.
if [ "$#" -gt 0 ]; then
    staged=("$@")
else
    staged=()
    while IFS= read -r p; do
        [ -n "$p" ] && staged+=("$p")
    done < <(git -C "$REPO_ROOT" diff --cached --name-only)
fi
[ "${#staged[@]}" -gt 0 ] || exit 0

my_label="${CLAIM_LABEL:-}"
fail=0

# Track which staged paths appear in ANY live claim (own label included), so the
# unclaimed-path warning below can fire on the remainder.
claimed_any=""

for claim in "${claim_files[@]}"; do
    label="$(head -n 1 "$claim")"
    own=0
    if [ -n "$my_label" ] && [ "$label" = "$my_label" ]; then
        own=1
    fi
    # Remaining lines = claimed paths (skip blanks and # comments).
    while IFS= read -r path; do
        case "$path" in
            ''|\#*) continue ;;
        esac
        for s in "${staged[@]}"; do
            if [ "$s" = "$path" ]; then
                claimed_any="$claimed_any|$s|"
                if [ "$own" -eq 0 ]; then
                    echo "CLAIMS GUARD: staged path '$s' is CLAIMED by '$label'" >&2
                    echo "              (claim file: ${claim#"$REPO_ROOT"/})" >&2
                    fail=1
                fi
            fi
        done
    done < <(tail -n +2 "$claim")
done

# WARNING path (non-fatal): staged + dirty but claimed by nobody. Claims are live
# (we got past the early exits), so other agents may have in-flight edits in this
# file that a commit would silently absorb (the PROBE68K sweep case).
for s in "${staged[@]}"; do
    case "$claimed_any" in
        *"|$s|"*) continue ;;
    esac
    echo "CLAIMS GUARD WARNING: '$s' — staging a file with uncommitted changes that" >&2
    echo "                      is claimed by nobody; the diff may include another" >&2
    echo "                      agent's work — verify the diff is all yours." >&2
done

if [ "$fail" -ne 0 ]; then
    if [ -n "$my_label" ]; then
        echo "Your CLAIM_LABEL is '$my_label' — it does not match the claim owner." >&2
    else
        echo "CLAIM_LABEL is unset — set it to your claim label if you own this work." >&2
    fi
    echo "Coordinate with the owner or wait for the claim to be released" >&2
    echo "(claims are deleted at the owner's final commit). Protocol:" >&2
    echo "docs/superpowers/.claims/README.md" >&2
    exit 1
fi
exit 0
