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

for claim in "${claim_files[@]}"; do
    label="$(head -n 1 "$claim")"
    # Skip claims we own.
    if [ -n "$my_label" ] && [ "$label" = "$my_label" ]; then
        continue
    fi
    # Remaining lines = claimed paths (skip blanks and # comments).
    while IFS= read -r path; do
        case "$path" in
            ''|\#*) continue ;;
        esac
        for s in "${staged[@]}"; do
            if [ "$s" = "$path" ]; then
                echo "CLAIMS GUARD: staged path '$s' is CLAIMED by '$label'" >&2
                echo "              (claim file: ${claim#"$REPO_ROOT"/})" >&2
                fail=1
            fi
        done
    done < <(tail -n +2 "$claim")
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
