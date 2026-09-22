#!/bin/bash
# E24 run 004: give the paging node something to do, and settle whether the IH ring delivers anything at all.
# Order: the positive control first (a CP fence WITH its interrupt - if that vector does not arrive, nothing about
# the display's interrupt can be concluded, M103), then pressure on VidMm so that it has to move allocations over
# node 1 (M102), then the counters. Fresh boot, one bring-up (M78), guarded.
# Usage: e24_run4.sh [sizeMB]   default 512
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.25
CLI='C:\BC250\e16\bc250kmd_cli.exe'
SIZE=${1:-512}
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | tr -d '\000' | cut -c1-200; }
dcn()  { echo "--- dcn $1"; $T run "$CLI dcn" 2>&1 | tr -d '\000' | grep -E 'vblank event|master update|flip pending|keepout|OTG0_OTG_GLOBAL_SYNC_STATUS|OTG0_OTG_STATUS_FRAME_COUNT' | head -8; }
ihs()  { echo "--- ih $1"; step -Phase ih -Op state | grep -E 'ring |windows |interrupt' | head -4; }
summary() { echo "--- counters $1"; $T run "$CLI log summary" 2>&1 | tr -d '\000' | grep -i -E 'vsync|vidpn flip|BuildPagingBuffer:|node 1|node 0|paging operation|presents' | head -12; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label  ($(date +%H:%M:%S))"
    step "$@" > $S/e24r4_step.txt
    grep -E 'rc |result|exit code|stage |ring |fence|REFUSED|refused|error' $S/e24r4_step.txt | tail -${TAILN:-6}
    grep -q 'exit code 0' $S/e24r4_step.txt || { echo "STEP FAILED: $label"; return 1; }
    nostop || { echo "STOP flag: stopping after $label"; return 1; }
}
undo() {
    echo "=== undo  ($(date +%H:%M:%S))"
    step -Phase gfx -Op fini -Tag e24r4 | grep -E 'exit code' | tail -1
    $T run "$CLI ih fini" 2>&1 | tr -d '\000' | grep -E 'done|REFUSED' | tail -1
    step -Phase psp -Op unload | grep -E 'exit code' | tail -1
    step -Phase gart -Op restore | grep -E 'exit code' | tail -1
    step -Phase gate -Full 0 | grep -E 'device |stages |events |presents'
    step -Phase confirm | tail -1
    temp
}
nostop || { echo "STOP flag set: not starting"; exit 3; }
step -Phase state -Tag pre-e24r4 | grep -E 'stages |driver ' | tee $S/e24r4_state_pre.txt
grep -q 'last 61' $S/e24r4_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e24r4_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
$T ps experiments/E19-full-wddm-stage-c/bringup_guard.ps1 2>&1 | tr -d '\000' | grep -E "boot |bringup |verdict " | tee $S/e24r4_guard.txt
grep -q "verdict  FRESH" $S/e24r4_guard.txt || { echo "M78: this boot has already seen a bring-up - reboot first (shutdown /r), not starting"; exit 7; }
bash $S/mon_status.sh "E24 run 4 on bc250kmd $V: interrupt control, then ${SIZE} MB of VidMm pressure so the paging node has work" warn
temp
echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 -VidPnFlip 1  ($(date +%H:%M:%S))"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 -VidPnFlip 1 | grep -E 'device |stages |gate  |presents'
sleep 10
dcn "A: after the start"
must "gart enable" -Phase gart -Op enable || { undo; exit 5; }
must "psp load" -Phase psp -Op load || { undo; exit 5; }
TAILN=8 must "ih init" -Phase ih -Op init || { undo; exit 5; }
for n in 1 2 3 4 5 6 7 8; do
    TAILN=8 must "gfx run $n" -Phase gfx -Op run -Stage $n -Tag e24r4s$n || { echo "stopped at stage $n"; undo; exit 6; }
done
echo "=== the positive control: a CP fence with its interrupt (M77's path)"
ihs "before the fence"
TAILN=12 must "fence gfx x2 (interrupt expected)" -Phase fence -Op gfx -Count 2 || true
ihs "after the fence - vectors here mean the IH ring delivers in this device start"
dcn "B: after the fence"
echo "=== VidMm pressure: kmtprobe holds ${SIZE} MB so something has to move  ($(date +%H:%M:%S))"
$T run "C:\BC250\tmp\kmtprobe.exe --size ${SIZE}M --hold 20 --timeout 90" 2>&1 | tr -d '\000' | grep -iE "allocat|resident|paging|fence|error|failed|ok|bytes" | head -18
summary "after the pressure"
dcn "C: after the pressure"
ihs "after the pressure"
python tools/win/bc250mon/mon.py scanout --out /p/BC-250/scratch/screens/e24r4-half.png 2>&1 | tail -1
step -Phase log -Tag e24r4 | tail -1
$T ps $S/e24_lines.ps1 2>&1 | tr -d '\000' | cut -c1-200 | head -30
temp
undo
