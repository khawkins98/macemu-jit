#!/bin/sh
# Extract the 32-bit words of clang_ref.o's _xcheck_ref in order, then diff
# against the helper-produced encodings from ./thunk_ref --xcheck.
# otool -tvVj prints each instr as: "<addr>\t<bytes>\t<mnemonic>"; the bytes
# are little-endian space-separated -> reassemble into a 0xWORD.
set -e

# Disassemble; -j prints the instruction word alongside the mnemonic.
# Format (tab-separated):  <addr>\t<word>\t<mnemonic...>
# where <word> is the full 32-bit instruction in hex, already big-endian.
otool -tvVj clang_ref.o > /tmp/m1xc_dis.txt 2>/dev/null || true

clang_words=$(awk -F'\t' '
  /^[0-9a-f]+\t[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]\t/ {
    printf "0x%s\n", $2;
  }' /tmp/m1xc_dis.txt)

# Helper encodings.
./thunk_ref --xcheck > /tmp/m1xc_helper.txt

echo "$clang_words" > /tmp/m1xc_clang.txt
echo 0 > /tmp/m1xc_fail
i=0
printf "%-34s %-12s %-12s %s\n" "MNEMONIC" "HELPER" "CLANG" "RESULT"
printf "%-34s %-12s %-12s %s\n" "--------" "------" "-----" "------"
while IFS= read -r cw; do
  [ -z "$cw" ] && continue
  line=$(awk -v idx="$i" '$1=="XCHECK" && $2==idx {print}' /tmp/m1xc_helper.txt)
  hw=$(echo "$line" | awk '{print $3}')
  mnem=$(echo "$line" | cut -d' ' -f4-)
  if [ "$hw" = "$cw" ]; then
    res="MATCH"
  else
    res="*** MISMATCH ***"
    # [sketch] rows are EXPECTED to surface the plan's bug — not a helper failure.
    case "$mnem" in
      "[sketch]"*) res="*** MISMATCH (expected: plan-sketch bug, see doc §4.1) ***" ;;
      *) echo 1 > /tmp/m1xc_fail ;;
    esac
  fi
  printf "%-34s %-12s %-12s %s\n" "$mnem" "$hw" "$cw" "$res"
  i=$((i+1))
done < /tmp/m1xc_clang.txt

if [ "$(cat /tmp/m1xc_fail 2>/dev/null)" = "1" ]; then
  echo "XCHECK: FAIL (a HELPER encoding differs from clang)"
  exit 1
else
  echo "XCHECK: helpers ALL MATCH (1 expected sketch-bug mismatch flagged above)"
fi
