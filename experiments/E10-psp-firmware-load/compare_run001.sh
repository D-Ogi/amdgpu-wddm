#!/bin/sh
# Runs compare.py over the evidence of run 001; its output is comparison.txt in the evidence directory.
cd "$(dirname "$0")/../.." || exit 1
E=evidence/windows/2026-09-21-E10-run-001
C="python experiments/E10-psp-firmware-load/compare.py"
FW=${1:?usage: compare_run001.sh <directory holding cyan_skillfish2_*.bin>}
s() { ls $E/sweep-GC-$1-*.log $E/sweep-MMHUB-$1-*.log $E/sweep-MP0-$1-*.log; }
{
echo "== H2: the plan (psp-plan-plan)"; $C commands $E/psp-plan-plan-*.txt --firmware $FW --expect planned
echo; echo "== H2: state after the plan, control = before, before2, before3"
$C state --control $(s before) $(s before2) $(s before3) --after $(s afterplan)
echo; echo "== H4: the load (psp-load-load)"; $C commands $E/psp-load-load-*.txt --firmware $FW --expect loaded
echo; echo "== H6: the second load (psp-load-second)"; $C commands $E/psp-load-second-*.txt --firmware $FW --expect loaded
echo; echo "== H5: state after the load, control = the sweep after gart enable"
$C state --control $(s gart) --after $(s loaded)
echo; echo "== H6: state after the unload, control = the sweep after gart enable"
$C state --control $(s gart) --after $(s unloaded)
echo; echo "== H6: state after the second load, control = the sweep after gart enable"
$C state --control $(s gart) --after $(s second)
echo; echo "== H7: state after the device restart while loaded, control = before, before2, before3"
$C state --control $(s before) $(s before2) $(s before3) --after $(s afterstop)
} 2>&1
{
echo; echo "== after a warm reboot of the target, control = before, before2, before3"
$C state --control $(s before) $(s before2) $(s before3) --after $(s afterreboot)
} 2>&1
