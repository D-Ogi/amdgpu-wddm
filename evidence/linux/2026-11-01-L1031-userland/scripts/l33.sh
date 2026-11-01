#!/bin/sh
# L33: shader coherency probe on Linux - same E14 inthash SPIR-V, 1000 MHz, normal 16 rounds.
#   l33.sh   (post-amdgpu, R0, < 120 s including build)
# Needs: the reference chroot (build-root.ext4 on BC250WIN) or Alpine gcc + vulkan-loader + mesa-vulkan-adi.
# The source (vkcompute.c + shader-coherency-probe.c) and SPIR-V are pushed by the lead.
# Writes /root/ul-out/l33-result.txt. Task time: build < 30 s, run < 60 s.
IN=/root/ul
OUT=/root/ul-out
SRC=/root/ul/l33-src
mkdir -p $OUT
F=$OUT/l33-result.txt
{
echo "# L33 utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) uptime=$(cut -d' ' -f1 /proc/uptime) kernel=$(uname -r)"
echo "== driver and Vulkan"
cat /sys/module/amdgpu/parameters/dpm 2>/dev/null | sed 's/^/amdgpu.dpm=/'
modinfo amdgpu 2>/dev/null | grep -E '^version|^srcversion' | head -2
vulkaninfo --summary 2>/dev/null | head -20 || echo "(vulkaninfo not available)"
echo "== clock"
cat /sys/class/drm/card*/device/pp_dpm_sclk 2>/dev/null
echo "== build"
if [ -d "$SRC" ] && [ -f "$SRC/shader-coherency-probe.c" ]; then
	cd $SRC
	gcc -std=c99 -O2 -Wall -o shader-coherency-probe shader-coherency-probe.c -lvulkan -lm 2>&1
	if [ $? -eq 0 ]; then
		echo "BUILD OK"
		echo "== run: normal 16 rounds"
		./shader-coherency-probe spirv --runs 16 2>&1
		echo "exit=$?"
		echo "== run: --stale-input-control"
		./shader-coherency-probe spirv --runs 16 --stale-input-control 2>&1
		echo "exit=$?"
	else
		echo "BUILD FAILED"
	fi
else
	echo "SKIP: source not pushed to $SRC (lead stages vkcompute.c, shader-coherency-probe.c and spirv/)"
fi
echo "# end utc=$(date -u +%H:%M:%S)"
} > $F 2>&1
cat $F
# Judge: exit 0 = every GPU hash equals its CPU hash (same as the Windows M264 positive control).
# The output lines show BUFFER memory types, per-test hashes and native exit codes. Compare:
#   - hashes must equal the CPU oracle (GPU_HASH == CPU_HASH on every line)
#   - memory types and driver revision are recorded for the evidence
#   - stale-input-control run must also pass (no false visibility from stale data)
# Abort: build failure (missing headers/libraries, needs the chroot or Alpine packages).
#   GPU timeout: stop (M53 class, no reset).
