#!/bin/bash
# E24 run 001: bc250kmd 0.7.24 (installed display-only already), the paging node in the full table. Owner at the monitor.
# Sequence of E19 run 3 / E20 run 8: state, confirm, gate (-Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode 1),
# engines up by escape (gart, psp, ih, gfx run 8 = SDMA rings included), then about a minute of desktop for VidMm to page,
# ring log lines, temperature, gate closed. Usage: e24_run1.sh [nopaging]   (nopaging = same run with the node gate shut: H3)
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.24
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | cut -c1-220; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
must() {
    local label=$1; shift
    echo "=== $label"
    step "$@" > $S/e24_step.txt
    grep -E 'rc |result|exit code|stage|fence|check|vectors|interrupts|REFUSED|refused|error|RESULT' $S/e24_step.txt | tail -${TAILN:-8}
    grep -q 'exit code 0' $S/e24_step.txt || { echo "STEP FAILED: $label - stopping here, nothing undone"; temp; exit 5; }
    nostop || { echo "STOP flag: stopping after $label"; exit 3; }
}
PAGING=1; case " $* " in *" nopaging "*) PAGING=0 ;; esac
nostop || { echo "STOP flag set: not starting"; exit 3; }
bash $S/mon_status.sh "E24 run 1: bc250kmd $V, full table + paging node (EnablePagingNode $PAGING). Screen may go black for a few minutes; engines come up by escape, then a minute of desktop" warn
step -Phase state -Tag pre-e24 | grep -E 'stages |driver ' | tee $S/e24_state_pre.txt
grep -q 'last 61' $S/e24_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e24_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
temp
echo "=== gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode $PAGING"
step -Phase gate -Full 1 -GpuVa 1 -Engines 1 -GpuSubmit 1 -Blit 1 -PagingNode $PAGING | grep -E 'device |stages |gates |gate  |events |presents'
sleep 8
must "gart enable" -Phase gart -Op enable
must "psp load" -Phase psp -Op load
must "ih init" -Phase ih -Op init
TAILN=12 must "gfx run 8" -Phase gfx -Op run -Stage 8
temp
must "fence gfx x1 test (control)" -Phase fence -Op gfx -Count 1 -Mode test
must "fence s0 x1 test (SDMA0 control, the node's ring)" -Phase fence -Op s0 -Count 1 -Mode test
bash $S/mon_status.sh "E24 run 1: engines up, node 1 live; a minute of desktop for VidMm to page (move a window if you like)" warn
sleep 45
python tools/win/bc250mon/mon.py screenshot 2>&1 | tail -2
$T run "C:\BC250\e16\bc250kmd_cli.exe log summary" 2>&1 | grep -i -E 'paging|node|fence|flip|blit|refus' | head -20
step -Phase log -Tag e24 | tail -1
$T ps 'P:\BC-250\scratch\tmp\e24_lines.ps1' 2>&1 | cut -c1-210
temp
echo "=== undo: gfx fini, psp unload, gart restore, gate closed"
step -Phase gfx -Op fini -Tag e24 | grep -E 'exit code|stage' | tail -2
step -Phase psp -Op unload | grep -E 'exit code' | tail -1
step -Phase gart -Op restore | grep -E 'exit code' | tail -1
step -Phase gate -Full 0 | grep -E 'device |stages |events |reports|presents'
step -Phase confirm | tail -1
temp
