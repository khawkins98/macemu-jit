#!/bin/bash
# dump-manifest.sh — list/verify the persistent ROM dumps at /Users/Shared/macemu/dumps/.
#
# Default: list each MANIFEST.txt record alongside the file's current md5.
# --check: recompute md5s and FAIL (exit 1) on any mismatch or missing file —
#          this is the provenance ritual before tagging evidence [RAW-ROM]/[PATCH].
#
# MANIFEST.txt format (pipe-separated): filename | md5 | provenance | note
# (# comments and blank lines ignored.)

set -u

DUMPS_DIR="${SS_DUMPS_DIR:-/Users/Shared/macemu/dumps}"
MANIFEST="$DUMPS_DIR/MANIFEST.txt"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

if [ ! -f "$MANIFEST" ]; then
    echo "No manifest at $MANIFEST" >&2
    exit 1
fi

compute_md5() {
    # macOS md5 / Linux md5sum portability
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$1"
    else
        md5sum "$1" | awk '{print $1}'
    fi
}

trim() { echo "$1" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'; }

fail=0
echo "Dump manifest: $DUMPS_DIR"
echo
while IFS='|' read -r fname rec_md5 prov note; do
    fname="$(trim "${fname:-}")"
    case "$fname" in ''|\#*) continue ;; esac
    rec_md5="$(trim "${rec_md5:-}")"
    prov="$(trim "${prov:-}")"
    note="$(trim "${note:-}")"
    path="$DUMPS_DIR/$fname"

    if [ ! -f "$path" ]; then
        echo "MISSING  $fname  (recorded md5 $rec_md5)"
        fail=1
        continue
    fi

    if [ "$CHECK" -eq 1 ]; then
        cur_md5="$(compute_md5 "$path")"
        if [ "$cur_md5" = "$rec_md5" ]; then
            status="OK     "
        else
            status="MISMATCH"
            fail=1
        fi
        echo "$status  $fname"
        echo "          recorded: $rec_md5"
        [ "$status" = "MISMATCH" ] && echo "          current:  $cur_md5"
    else
        echo "$fname"
        echo "          md5:  $rec_md5"
    fi
    echo "          prov: $prov"
    [ -n "$note" ] && echo "          note: $note"
    echo
done < "$MANIFEST"

# Flag files present in the dir but absent from the manifest.
for f in "$DUMPS_DIR"/*; do
    base="$(basename "$f")"
    [ "$base" = "MANIFEST.txt" ] && continue
    [ -f "$f" ] || continue
    if ! grep -qE "^[[:space:]]*$base[[:space:]]*\|" "$MANIFEST"; then
        echo "UNMANIFESTED  $base  (present in $DUMPS_DIR but not in MANIFEST.txt)"
        [ "$CHECK" -eq 1 ] && fail=1
    fi
done

if [ "$CHECK" -eq 1 ]; then
    if [ "$fail" -ne 0 ]; then
        echo
        echo "PROVENANCE CHECK FAILED — do not tag [RAW-ROM]/[PATCH] from these dumps." >&2
        exit 1
    fi
    echo "All dumps match the manifest."
fi
exit 0
