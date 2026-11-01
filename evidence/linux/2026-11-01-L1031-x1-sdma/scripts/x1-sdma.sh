#!/bin/sh
# X1: SDMA copy above 16 KiB with stock firmware (akandr/bc250-rocm: stock microcode fails above 16384 B).
#   x1-sdma.sh stock     stock cyan_skillfish2_sdma*.bin (Boot A, post-amdgpu)
#   x1-sdma.sh navi12    navi12 SDMA firmware via firmware_class.path (separate boot)
# Each mode runs x1-sdma-copy.py which submits Vulkan buffer copies at 4K, 16K, 64K, 256K, 1M through RADV.
# RADV routes buffer copies through SDMA (amdgpu sdma0/sdma1 rings). The copy is verified by writing a
# known pattern to the source, issuing vkCmdCopyBuffer, reading back from the destination, and checking.
# Writes /root/ul-out/x1-sdma-<mode>.txt. R0 (read-only Vulkan copy, no register write). Task time < 60 s.
IN=/root/ul
OUT=/root/ul-out
MODE=${1:-stock}
mkdir -p $OUT
F=$OUT/x1-sdma-$MODE.txt
{
echo "# X1 $MODE utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) uptime=$(cut -d' ' -f1 /proc/uptime) kernel=$(uname -r)"
echo "== SDMA firmware"
dmesg | grep -i -E 'sdma|firmware' | grep -i sdma | tail -10
ls -la /lib/firmware/amdgpu/cyan_skillfish2_sdma* 2>/dev/null || echo "(no sdma firmware in /lib/firmware/amdgpu)"
echo "== firmware_class.path"
cat /sys/module/firmware_class/parameters/path 2>/dev/null || echo "(not readable)"
echo "== Vulkan driver"
vulkaninfo --summary 2>/dev/null | head -30 || echo "(vulkaninfo not available; trying VK_ICD_FILENAMES)"
echo "== SDMA copy test"
python3 $IN/x1-sdma-copy.py 2>&1
echo "# end utc=$(date -u +%H:%M:%S)"
} > $F 2>&1
cat $F
# Judge: every copy size must produce "PASS" (destination matches source pattern). A FAIL or GPU timeout
# at >16 KiB with stock firmware confirms the community report. A PASS at all sizes with navi12 firmware
# confirms the fix. If vulkaninfo/RADV is not available, the test cannot run and the script reports that.
# Abort: GPU timeout (fence wait > 10 s): stop, do not retry. A hung SDMA can lock the GPU (M53 class).
