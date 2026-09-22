#!/bin/bash
# E24 run 006: 0.7.28 advances pDmaBuffer (M108). Does dxgkrnl now submit what the node builds?
# Same shape as run 005 - fresh boot, one bring-up (M78), pressure at 64, 256 and 512 MB - with two
# deliberate differences: the VidPnFlip gate stays CLOSED (this experiment has no use for it, and run 005
# left the owner watching M100's unpainted primary for four minutes), and the counters are read for the
# thing that was missing: SubmitCommand, and node 1's own submitted/completed numbers.
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
exec > >(tee $S/e24r6_console.txt) 2>&1
T="python tools/win/target.py"
V=0.7.28
CLI='C:\BC250\e16\bc250kmd_cli.exe'
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | tr -d '\000' | cut -c1-200; }
ihs()  { echo "--- ih $1"; step -Phase ih -Op state | grep -E 'ring |windows |interrupt' | head -4; }
paging() {
    echo "--- paging counters $1"
    $T run "$CLI log summary" 2>&1 | tr -d '\000' |
        grep -i -E 'node 1 \(paging|BuildPagingBuffer: |SubmitCommand |submissions |paging operation' | head -10
}
tdr() { step -Phase state -Tag e24r6 | grep -E 'events |device ' | head -4; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label  ($(date +%H:%M:%S))"
    step "$@" > $S/e24r6_step.txt
    grep -E 'rc |result|exit code|stage |ring |fence|REFUSED|refused|error' $S/e24r6_step.txt | tail -${TAILN:-6}
    grep -q 'exit code 0' $S/e24r6_step.txt || { echo "STEP FAILED: $label"; return 1; }
    nostop || { echo "STOP flag: stopping after $label"; return 1; }
}
pressure() {
    local mb=$1
    echo "=== VidMm pressure ${mb} MB  ($(date +%H:%M:%S))"
    $T run "C:\BC250\tmp\kmtprobe.exe --size ${mb}M --hold 10 --timeout 90" 2>&1 | tr -d '\000' |
        grep -iE "allocat|resident|paging|fence|error|failed|ok$|bytes at base" | head -10
    paging "after ${mb} MB"
    tdr
    temp
}
undo() {
    echo "=== undo  ($(date +%H:%M:%S))"
    step -Phase gfx -Op fini -Tag e24r6 | grep -E 'exit code' | tail -1
    $T run "$CLI ih fini" 2>&1 | tr -d '\000' | grep -E 'done|REFUSED' | tail -1
    step -Phase psp -Op unload | grep -E 'exit code' | tail -1
    step -Phase gart -Op restore | grep -E 'exit code' | tail -1
    step -Phase gate -Full 0 | grep -E 'device |stages |events |presents'
    step -Phase confirm | tail -1
    temp
}

nostop || { echo "STOP flag set: not starting"; exit 3; }
step -Phase state -Tag pre-e24r6 | grep -E 'stages |driver |version' | tee $S/e24r6_state_pre.txt
grep -q "last 61" $S/e24r6_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
step -Phase confirm | tail -1
$T ps experiments/E19-full-wddm-stage-c/bringup_guard.ps1 2>&1 | tr -d '\000' | grep -E "boot |bringup |verdict " | tee $S/e24r6_guard.txt
grep -q "verdict  FRESH" $S/e24r6_guard.txt || { echo "M78: this boot has already seen a bring-up - reboot first, not starting"; exit 7; }
bash $S/mon_status.sh "E24 run 6 on bc250kmd $V: the paging node with pDmaBuffer fixed - the screen stays as it is this time" warn
temp

echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1  (flip gate CLOSED)  ($(date +%H:%M:%S))"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 | grep -E 'device |stages |gate  |presents'
sleep 10
must "gart enable" -Phase gart -Op enable || { undo; exit 5; }
must "psp load" -Phase psp -Op load || { undo; exit 5; }
TAILN=8 must "ih init" -Phase ih -Op init || { undo; exit 5; }
for n in 1 2 3 4 5 6 7 8; do
    TAILN=8 must "gfx run $n" -Phase gfx -Op run -Stage $n -Tag e24r6s$n || { echo "stopped at stage $n"; undo; exit 6; }
done

echo "=== the control: a CP fence with its interrupt, before anything is asked of the node"
ihs "before the fence"
TAILN=12 must "fence gfx x2" -Phase fence -Op gfx -Count 2 || true
ihs "after the fence"
paging "before any pressure"

pressure 64
pressure 256
pressure 512

ihs "after the pressure"
step -Phase log -Tag e24r6 | tail -1
$T ps $S/e24_lines.ps1 2>&1 | tr -d '\000' | cut -c1-200 | head -30
temp
undo
