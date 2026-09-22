#!/bin/bash
# E22 run 004: does the VUPDATE_NO_LOCK interrupt arrive once the IH ring is running (M98's inference)?
# Same gates as run 003 (full table, blit, VidPnFlip) plus -Engines 1, and of the bring-up only gart enable,
# psp load and ih init - the three steps E24 run 001 completed cleanly. `gfx run` is NOT touched: that is the
# step that hung the machine, and nothing here needs the CP or SDMA. The paging node stays closed.
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.24
CLI='C:\BC250\e16\bc250kmd_cli.exe'
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | tr -d '\000' | cut -c1-220; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label"
    step "$@" > $S/e22r4_step.txt
    grep -E 'rc |result|exit code|ring |interrupt|vectors|REFUSED|refused|error' $S/e22r4_step.txt | tail -${TAILN:-6}
    grep -q 'exit code 0' $S/e22r4_step.txt || { echo "STEP FAILED: $label - stopping, undo below"; return 1; }
    nostop || { echo "STOP flag: stopping after $label"; return 1; }
}
undo() {
    echo "=== undo: psp unload, gart restore, gate closed"
    step -Phase psp -Op unload | grep -E 'exit code' | tail -1
    step -Phase gart -Op restore | grep -E 'exit code' | tail -1
    step -Phase gate -Full 0 | grep -E 'device |stages |events |reports|presents'
    step -Phase confirm | tail -1
    temp
}
nostop || { echo "STOP flag set: not starting"; exit 3; }
bash $S/mon_status.sh "E22 run 4: bc250kmd $V, full table + flip + IH ring only (no gfx run, no paging node). Screen blinks; ~40 s of desktop, then everything closes" warn
step -Phase state -Tag pre-e22r4 | grep -E 'stages |driver ' | tee $S/e22r4_state_pre.txt
grep -q 'last 61' $S/e22r4_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e22r4_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
temp
echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -Blit 1 -VidPnFlip 1 (paging node closed, no gfx)"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -Blit 1 -VidPnFlip 1 | grep -E 'device |stages |gates |gate  |events |presents'
sleep 8
must "gart enable" -Phase gart -Op enable || { undo; exit 5; }
must "psp load" -Phase psp -Op load || { undo; exit 5; }
TAILN=8 must "ih init" -Phase ih -Op init || { undo; exit 5; }
echo "=== 40 s of desktop with the IH ring up and the hardware vsync armed"
sleep 40
$T run "$CLI log summary" 2>&1 | tr -d '\000' | grep -i -E 'vsync|flip|blit|present|counters' | head -12
TAILN=8 must "ih state (how many interrupts and vectors came)" -Phase ih -Op state || true
python tools/win/bc250mon/mon.py scanout --out /p/BC-250/scratch/screens/e22r4-half.png 2>&1 | tail -1
temp
undo
