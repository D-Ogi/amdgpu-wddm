#!/bin/bash
# E24 run 002/003: the same bring-up E24 run 001 hung on, but one stage per escape call, so that the console
# names the stage the machine dies on (a hard hang leaves no dump and no log ring, fact M96, and GuardStage
# cannot write from inside the escape: it holds GartLock, which is a fast mutex, i.e. APC_LEVEL).
# Usage: e24_run2.sh <pagingnode 0|1>   - run 002 is the control with the node closed, run 003 has it open.
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.24
PAGING=${1:-0}
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | tr -d '\000' | cut -c1-200; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label"
    step "$@" > $S/e24r2_step.txt
    grep -E 'rc |result|exit code|stage |ring |REFUSED|refused|error' $S/e24r2_step.txt | tail -${TAILN:-6}
    grep -q 'exit code 0' $S/e24r2_step.txt || { echo "STEP FAILED: $label"; return 1; }
    nostop || { echo "STOP flag: stopping after $label"; return 1; }
}
undo() {
    echo "=== undo"
    step -Phase gfx -Op fini -Tag e24r2 | grep -E 'exit code' | tail -1
    step -Phase psp -Op unload | grep -E 'exit code' | tail -1
    step -Phase gart -Op restore | grep -E 'exit code|NTSTATUS' | tail -2
    step -Phase gate -Full 0 | grep -E 'device |stages |events |presents'
    step -Phase confirm | tail -1
    temp
}
nostop || { echo "STOP flag set: not starting"; exit 3; }
bash $S/mon_status.sh "E24 run with PagingNode=$PAGING: the bring-up one stage at a time, to name the stage that hangs. The screen may go black and stay black" warn
step -Phase state -Tag pre-e24r2 | grep -E 'stages |driver ' | tee $S/e24r2_state_pre.txt
grep -q 'last 61' $S/e24r2_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e24r2_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
# M78: a bring-up is the first one of its boot or it is not run at all. Two hangs today were exactly this.
$T ps /p/BC-250/scratch/tmp/bringup_guard.ps1 2>&1 | tr -d '\000' | grep -E "boot |bringup |verdict " | tee $S/e24r2_guard.txt
grep -q "verdict  FRESH" $S/e24r2_guard.txt || { echo "M78: this boot has already seen a bring-up - reboot the unit first, not starting"; exit 7; }
temp
echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode $PAGING"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode $PAGING | grep -E 'device |stages |gate  |presents'
sleep 8
must "gart enable" -Phase gart -Op enable || { undo; exit 5; }
must "psp load" -Phase psp -Op load || { undo; exit 5; }
must "ih init" -Phase ih -Op init || { undo; exit 5; }
for n in 1 2 3 4 5 6 7 8; do
    echo "--- gfx run stage $n  ($(date +%H:%M:%S))"
    TAILN=10 must "gfx run $n" -Phase gfx -Op run -Stage $n -Tag e24r2s$n || { echo "stopped at stage $n"; undo; exit 6; }
done
echo "=== all eight stages returned"
temp
undo
