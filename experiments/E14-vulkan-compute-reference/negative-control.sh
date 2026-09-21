#!/bin/sh
# Negative control for E14. A harness that can only print match=yes proves nothing, so before the
# result table means anything, show that the comparison catches a wrong answer: build one shader
# that writes value+1 instead of value and check that the run reports match=NO and exits 1.
#
#   sh negative-control.sh
#
# Everything in RAM under /tmp/e14/negctl, nothing else is touched. Needs build.sh to have run.
set -u

BUILD=${E14_OUT:-/tmp/e14/build}
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/e14/negctl

rm -rf "$OUT"
mkdir -p "$OUT/shaders"
cp "$BUILD"/shaders/*.spv "$OUT/shaders/"

sed 's/o\.v\[i\] = p\.value;/o.v[i] = p.value + 1u;/' "$SRC/shaders/fill.comp" > "$OUT/fill-bad.comp"
echo "== the one changed line"
grep -n 'o\.v\[i\]' "$OUT/fill-bad.comp"
glslangValidator -V "$OUT/fill-bad.comp" -o "$OUT/shaders/fill.spv" >/dev/null || exit 2

# No pipe around the program here: a pipeline's exit status is the last command's, not its.
echo "== fill_g1 against the wrong fill.spv (expected: match=NO, exit 1)"
"$BUILD/vkcompute" "$OUT/shaders" --only fill_g1 --runs 1 > "$OUT/bad.txt" 2>&1
bad_rc=$?
grep 'fill_g1' "$OUT/bad.txt"
echo "exit=$bad_rc"

echo "== inthash from the same directory (expected: match=yes, exit 0)"
"$BUILD/vkcompute" "$OUT/shaders" --only inthash --runs 1 > "$OUT/good.txt" 2>&1
good_rc=$?
grep 'inthash' "$OUT/good.txt"
echo "exit=$good_rc"

if [ "$bad_rc" -eq 1 ] && [ "$good_rc" -eq 0 ]; then
	echo "NEGATIVE CONTROL PASSED: the comparison distinguishes right from wrong"
	exit 0
fi
echo "NEGATIVE CONTROL FAILED: the harness did not react as it must"
exit 1
