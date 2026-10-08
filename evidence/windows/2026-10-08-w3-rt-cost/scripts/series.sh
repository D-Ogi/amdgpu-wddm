#!/usr/bin/env bash
# Runs the remaining arms of the W3 RT effect-cost series one after another (run-arm.sh each) and stops at the first
# arm whose closure check or measurement is not clean: start not re-confirmed, a stack identity changed, the perftest
# marker left behind, a game process left running, no window B rate for the game, or a STOP request.
# Usage: series.sh "N|PRESET|PERFTEST|LABEL|NEXT" ...
w=/p/BC-250/scratch/w3-rt-cost
for spec in "$@"; do
  IFS='|' read -r n preset perftest label next <<< "$spec"
  python /p/BC-250/bc250-win/tools/win/bc250mon/mon.py "stop?" | grep -q "no stop request" || { echo "STOP requested before $n"; exit 2; }
  echo "== series arm $n $preset ${perftest:-} start $(date -u +%H:%M:%S)"
  ARM_LABEL="$label" ARM_NEXT="$next" bash "$w/run-arm.sh" "$n" "$preset" $perftest > "$w/arm-$n.out" 2>&1
  out="$w/arm-$n.out"
  grep -qE "confirmed after|already confirmed" "$out" || { echo "STOP: $n start not re-confirmed"; exit 3; }
  grep -q 'd3d12\\amdgpu_wddm_d3d12.dll BBB5803E' "$out" && grep -q 'd3d12\\amdgpu_wddm_radv.dll 822134D0' "$out" \
    && grep -q 'kmd sys 7580A8F7' "$out" && grep -q 'bc250d3d_router.dll 93F707BB' "$out" || { echo "STOP: $n identity changed"; exit 3; }
  grep -q 'radv-perftest marker: absent' "$out" || { echo "STOP: $n perftest marker left"; exit 3; }
  grep -q '^RUNNING' "$out" && { echo "STOP: $n game process left"; exit 3; }
  grep -E "^$n " "$out" | grep -q "rate [0-9]" || { echo "STOP: $n no window B rate"; exit 3; }
  grep -E "^$n " "$out"
done
echo "series end $(date -u +%H:%M:%S)"
