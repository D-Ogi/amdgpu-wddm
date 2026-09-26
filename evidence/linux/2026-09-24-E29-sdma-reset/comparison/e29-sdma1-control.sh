set -eu
D=
for d in /sys/kernel/debug/dri/*; do
 [ -f "$d/amdgpu_sdma_sched_mask" ] || continue
 D=$(readlink -f "$d"); break
done
[ -n "$D" ] || exit 2
old=$(cat "$D/amdgpu_sdma_sched_mask")
[ "$old" = 3 ] || exit 2
[ "$(grep -c '(sdma' "$D/amdgpu_fence_info")" = 2 ] || exit 2
echo before_fences
cat "$D/amdgpu_fence_info"
restore() { echo "$old" > "$D/amdgpu_sdma_sched_mask"; printf 'restored_mask='; cat "$D/amdgpu_sdma_sched_mask"; }
trap restore EXIT
echo 2 > "$D/amdgpu_sdma_sched_mask"
[ "$(cat "$D/amdgpu_sdma_sched_mask")" = 2 ] || exit 2
echo selected_mask=2
sh /tmp/bc250-e29/run-sdma-reset-trace.sh control /tmp/bc250-e29/sdma-reset-probe.py /tmp/bc250-e29/reference /tmp/bc250-e29/packets.json
echo after_fences
cat "$D/amdgpu_fence_info"
restore
trap - EXIT
echo physical_sdma1_control_complete
