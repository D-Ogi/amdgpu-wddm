#!/bin/sh
# E14/cs: capture the PM4 command stream RADV submits for ONE compute dispatch.
# POSIX sh, busybox compatible, runs on the lab. Everything under /tmp/e14/cs, which is RAM.
#
#   sh run-cs.sh          capture everything
#   sh run-cs.sh probe    only: which dump mechanism does this Mesa have?
#
# Mechanism (established by testing, not assumed - see README):
#   RADV_DEBUG=dumpibs works on a HEALTHY submit on Mesa 26.1.6 and prints ac_debug's own decode
#   of every IB to stderr. `noibchaining` makes RADV inline the state IB instead of pointing at it
#   with an INDIRECT_BUFFER, so the two runs are kept side by side: the chained one carries the IB
#   GPU VA and size, the unchained one the full body. `RADV_DEBUG=hang` only arms the hang path
#   and prints nothing on a healthy submit; RADV_TRACE_FILE does nothing in this build.
#
# Towards the GPU this is an ordinary Vulkan client. Nothing is written outside /tmp; the debugfs
# ring files are read by exact name only.
set -u

OUT=/tmp/e14/cs
BIN=${E14_BIN:-/tmp/e14/build/vkcompute}
SPV=${E14_SPV:-/tmp/e14/build/shaders}
D=/sys/kernel/debug/dri/0000:01:00.0
mkdir -p "$OUT"

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }
say() { echo "== $*"; }

capture() {
	name=$1
	dbg=$2
	RADV_DEBUG="$dbg" "$BIN" "$SPV" --only fill_g1 --runs 1 \
		>"$OUT/run-$name.out" 2>"$OUT/ib-$name.raw"
	strip_ansi < "$OUT/ib-$name.raw" > "$OUT/ib-$name.txt"
	echo "$name (RADV_DEBUG=$dbg): $(wc -l < "$OUT/ib-$name.txt") lines"
}

phase_probe() {
	say "which mechanism does this Mesa have? bytes on stderr for one dispatch"
	for opt in dumpibs dumpibs,noibchaining hang hang,syncshaders; do
		RADV_DEBUG="$opt" "$BIN" "$SPV" --only fill_g1 --runs 1 >/dev/null 2>"$OUT/.probe"
		printf '  RADV_DEBUG=%-24s %s bytes\n' "$opt" "$(wc -c < "$OUT/.probe")"
	done
	RADV_TRACE_FILE="$OUT/.trace" "$BIN" "$SPV" --only fill_g1 --runs 1 >/dev/null 2>"$OUT/.probe"
	printf '  RADV_TRACE_FILE                    %s bytes stderr, trace file: %s\n' \
		"$(wc -c < "$OUT/.probe")" "$([ -e "$OUT/.trace" ] && echo created || echo "not created")"
	rm -f "$OUT/.probe" "$OUT/.trace"
}

[ "${1:-all}" = probe ] && { phase_probe; exit 0; }

say "IB dumps"
capture chained   dumpibs
capture unchained dumpibs,noibchaining
capture allbos    dumpibs,noibchaining,allbos

say "IB sections captured"
grep -n '^-\{3,\}' "$OUT/ib-unchained.txt"

say "is there a BO list anywhere in the allbos run? (expected: no, RADV does not print one)"
grep -ciE 'bo list|buffer list|bo_list' "$OUT/ib-allbos.txt"

# Ring files are read by exact name, never globbed: reading the wrong debugfs file does something.
say "gfx ring after the run"
if [ -e "$D/amdgpu_ring_gfx_0.0.0" ]; then
	dd if="$D/amdgpu_ring_gfx_0.0.0" bs=4096 2>/dev/null | od -A x -t x4 -v \
		> "$OUT/ring_gfx_0.0.0.txt"
	echo "  $(wc -l < "$OUT/ring_gfx_0.0.0.txt") lines"
else
	echo "  no $D/amdgpu_ring_gfx_0.0.0"
fi

say "compute ring 0 after the run"
if [ -e "$D/amdgpu_ring_comp_1.0.0" ]; then
	dd if="$D/amdgpu_ring_comp_1.0.0" bs=4096 2>/dev/null | od -A x -t x4 -v \
		> "$OUT/ring_comp_1.0.0.txt"
	echo "  $(wc -l < "$OUT/ring_comp_1.0.0.txt") lines"
fi

say "sizes"
ls -l "$OUT"
echo
echo "Now pull $OUT to the PC and decode it there with decode_cs.py."
