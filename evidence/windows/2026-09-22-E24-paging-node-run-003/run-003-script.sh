#!/bin/bash
# E24 run 003 + E22 step 3's open question, in one bring-up, because a boot buys exactly one (M78).
# Order is chosen so that the cheap answers come before the risky step: the display-only half of the vsync
# question first (does OTG0 latch VUPDATE_NO_LOCK_EVENT_OCCURRED at all), then the engines, then the CP and
# SDMA stages one escape call at a time (E24 run 002 died inside stage 6 as the second bring-up of its boot),
# then the paging node's own traffic. Every dcn dump is reads only.
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.25
CLI='C:\BC250\e16\bc250kmd_cli.exe'
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | tr -d '\000' | cut -c1-200; }
dcn()  { echo "--- dcn $1"; $T run "$CLI dcn" 2>&1 | tr -d '\000' | grep -E 'vupdate|keepout|master update|flip pending|event|HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS |OTG0_OTG_STATUS_FRAME_COUNT|OTG0_OTG_GLOBAL_SYNC_STATUS' | head -12; }
summary() { echo "--- counters $1"; $T run "$CLI log summary" 2>&1 | tr -d '\000' | grep -i -E 'vsync|vidpn flip|paging|node 1|presents|blit gate' | head -10; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label  ($(date +%H:%M:%S))"
    step "$@" > $S/e24r3_step.txt
    grep -E 'rc |result|exit code|stage |ring |REFUSED|refused|error' $S/e24r3_step.txt | tail -${TAILN:-6}
    grep -q 'exit code 0' $S/e24r3_step.txt || { echo "STEP FAILED: $label"; return 1; }
    nostop || { echo "STOP flag: stopping after $label"; return 1; }
}
undo() {
    echo "=== undo  ($(date +%H:%M:%S))"
    step -Phase gfx -Op fini -Tag e24r3 | grep -E 'exit code' | tail -1
    $T run "$CLI ih fini" 2>&1 | tr -d '\000' | grep -E 'done|REFUSED|refused' | tail -1
    step -Phase psp -Op unload | grep -E 'exit code' | tail -1
    step -Phase gart -Op restore | grep -E 'exit code|NTSTATUS' | tail -1
    step -Phase gate -Full 0 | grep -E 'device |stages |events |presents'
    step -Phase confirm | tail -1
    temp
}
nostop || { echo "STOP flag set: not starting"; exit 3; }
bash $S/mon_status.sh "E24 run 3 on bc250kmd $V: full table, flip, vsync and the paging node, bring-up one stage at a time. Fresh boot, one bring-up (M78)" warn
step -Phase state -Tag pre-e24r3 | grep -E 'stages |driver ' | tee $S/e24r3_state_pre.txt
grep -q 'last 61' $S/e24r3_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e24r3_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
$T ps $S/bringup_guard.ps1 2>&1 | tr -d '\000' | grep -E "boot |bringup |verdict " | tee $S/e24r3_guard.txt
grep -q "verdict  FRESH" $S/e24r3_guard.txt || { echo "M78: this boot has already seen a bring-up - reboot first"; exit 7; }
temp
echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 -VidPnFlip 1  ($(date +%H:%M:%S))"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1 -VidPnFlip 1 | grep -E 'device |stages |gate  |presents'
sleep 10
dcn "A: just after the start (flip done, vsync armed, no engine up)"
echo "=== 40 s of desktop, display only"
sleep 40
dcn "B: after 40 s of desktop - does the event latch without any engine?"
summary "B"
must "gart enable" -Phase gart -Op enable || { undo; exit 5; }
must "psp load" -Phase psp -Op load || { undo; exit 5; }
TAILN=8 must "ih init" -Phase ih -Op init || { undo; exit 5; }
dcn "C: engines up, IH ring live"
for n in 1 2 3 4 5 6 7 8; do
    TAILN=10 must "gfx run $n" -Phase gfx -Op run -Stage $n -Tag e24r3s$n || { echo "stopped at stage $n"; dcn "at failure"; undo; exit 6; }
done
echo "=== all eight stages returned  ($(date +%H:%M:%S))"
temp
echo "=== 40 s of desktop with the paging node live"
sleep 40
dcn "D: everything up, after 40 s"
summary "D"
TAILN=8 must "ih state" -Phase ih -Op state || true
python tools/win/bc250mon/mon.py scanout --out /p/BC-250/scratch/screens/e24r3-half.png 2>&1 | tail -1
step -Phase log -Tag e24r3 | tail -1
$T ps $S/e24_lines.ps1 2>&1 | tr -d '\000' | cut -c1-200 | head -40
temp
undo
