#!/usr/bin/env bash
# W3 RT effect-cost series (2026-10-08): one Witcher 3 session per arm on the deployed hand deviation (KMD 0.7.216.20,
# D3D12 shell BBB5803E = adapter132, accepted), direct start with no experiment (the shell defaults, as a Steam start
# without an application profile), 1920x1080 fullscreen, VSync off, LimitFPS 240, FXAA, no upscaler/FG/DRS.
# Fixed drive-still input (Kaer Morhen room, window B 90 s GPU only from the first look), intro skip, plug sampling,
# overlay summary poll paused for the session, optional RADV_PERFTEST marker (cleared afterwards).
# Closure: on KMD 0.7.216.20 the game's exclusive 1920x1080 fullscreen commits a real mode (stretched to the panel's
# 1920x1200), the KMD epoch moves and the supervisor's Verify refuses ("Confirmed CPU baseline required", as 466).
# The runner then re-confirms the start, pulls the archive and checks the stack identities by hand (identity.ps1).
# Usage: [ARM_LABEL="A1 all RT"] [ARM_NEXT="B in ~10 min"] run-arm.sh N PRESET [PERFTEST]
n="$1"; preset="$2"; perftest="${3:-}"; label="${ARM_LABEL:-$preset}"
d=/p/BC-250/scratch/m15/native-caps001; w=/p/BC-250/scratch/w3-rt-cost
[ -f "$d/plans/plan-$n.md" ] || { echo "plan-$n.md missing"; exit 1; }
cd /p/BC-250/bc250-win || exit 1
python tools/win/bc250mon/mon.py status "W3 RT test: arm $label starting (trial $n): game loads ~4 min, in-world view (Kaer Morhen room, still) about $(date -d "+5 min" +%H:%M)-$(date -d "+${ARM_WORLD_END:-7} min" +%H:%M) local time. Please do not change display modes." > /dev/null 2>&1
( timeout 60 python tools/win/target.py ps ../scratch/w3-rt-cost/lab/arm-knobs.ps1 -Pause on ) \
  || { echo "knob step failed"; exit 3; }
cd "$d" || exit 1
# The preflight admits a RADV_PERFTEST marker only when it is newer than the attempt directory that the push creates
# (gate of 251; 475 was refused with the marker written before the push). So the marker is written when run-slot
# prints "package flushed", before Prepare, the witness wait and Start.
if [ -n "$perftest" ]; then
  ( for i in $(seq 1 240); do
      grep -q "package flushed" trial-$n.log 2>/dev/null && break
      grep -q '"status"' trial-$n.log 2>/dev/null && exit 0; sleep 1
    done
    grep -q "package flushed" trial-$n.log 2>/dev/null || exit 0
    cd /p/BC-250/bc250-win && timeout 60 python tools/win/target.py ps ../scratch/w3-rt-cost/lab/arm-knobs.ps1 \
      -Perftest "$perftest" -Pause on > "$w/knob-$n.out" 2>&1; echo "armed $(date -u +%H:%M:%S)" >> "$w/knob-$n.out" ) &
fi
( for i in $(seq 1 120); do
    grep -q "Start Running" trial-$n.log 2>/dev/null && break
    grep -q '"status"' trial-$n.log 2>/dev/null && exit 0; sleep 5
  done
  grep -q "Start Running" trial-$n.log 2>/dev/null || exit 0
  bash /p/BC-250/scratch/bd050/lab/plug-sample.sh "$d/plug-$n.log" ${PLUG_SAMPLES:-45} ) &
# ARM_DRIVE=none (in-game sweep 474+): the operator drives the session with gco.sh, no fixed input.
[ "${ARM_DRIVE:-still}" = still ] && { bash drive-still.sh "$n" > drive-$n.out 2>&1 & }
( for i in $(seq 1 120); do grep -q "Start Running" trial-$n.log 2>/dev/null && break; sleep 5; done
  grep -q "Start Running" trial-$n.log 2>/dev/null || exit 0
  for i in $(seq 1 20); do
    sleep 15
    if timeout 90 python ../game-recon/game-control.py "native-caps$n" peek 2>&1 | grep -q -E " menu [0-9]+s"; then
      timeout 90 python ../game-recon/game-control.py "native-caps$n" "tap:39;wait:1500;tap:39;wait:1500;tap:39" \
        > skip-$n.out 2>&1; echo "skip sent $(date -u +%H:%M:%S)" >> skip-$n.out; exit 0
    fi
  done ) &
SESSION_ADAPTER=adapter132 SESSION_ACCEPTED=1 SESSION_SAMPLER=1 SESSION_SAMPLER_SAMPLES=${ARM_SAMPLES:-340} SESSION_EXPERIMENT=none \
SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_ETW_ARGS="${ARM_ETW:--Seconds 40 -LatestB 447 -FpsSeconds 90}" \
  bash run-m157.sh "$n" "$preset" "${ARM_BOUND:-600}" > run-$n.out 2>&1
echo "run-m157 exit $?" >> run-$n.out
wait
cd /p/BC-250/bc250-win || exit 1
timeout 60 python tools/win/target.py ps ../scratch/w3-rt-cost/lab/arm-knobs.ps1 -Pause off
echo "== closure check $(date -u +%H:%M:%S)"
grep -h '"status"' "$d/trial-$n.log" | tail -1 | cut -c1-200
timeout 90 python tools/win/target.py ps ../scratch/m15/iflip2/lab/confirm-rerun.ps1 | grep -E "already|confirmed after|no start-confirm"
( cd "$d" && timeout 300 python run.py Pull --attempt "native-caps$n" | tail -1 )
timeout 60 python tools/win/target.py ps ../scratch/w3-rt-cost/lab/identity.ps1 > "$w/lab/identity-after$n.txt" 2>&1
grep -E "kmd sys|d3d12\\\\|bc250d3d_router|marker|RUNNING" "$w/lab/identity-after$n.txt"
cd "$d" || exit 1
grep -A3 "== window B" run-$n.out
grep "window B trigger\|window B gpu-only" run-$n.out
python "$w/analyze.py" "$n:$preset${perftest:++$perftest}"
python tools/win/bc250mon/mon.py status "W3 RT test: arm $label done (trial $n). ${ARM_NEXT:-}" > /dev/null 2>&1
echo "arm end $(date -u +%H:%M:%S)"
