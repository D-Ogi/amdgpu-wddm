#!/bin/sh
# Runs compare.py over the evidence of run 001; its output is comparison.txt in the evidence directory.
cd "$(dirname "$0")/../.." || exit 1
E=evidence/windows/2026-09-21-E11-run-001
C="python experiments/E11-gfx-bringup/compare.py"
s() { ls $E/$2sweep-GC-$1-*.log $E/$2sweep-MMHUB-$1-*.log $E/$2sweep-MP0-$1-*.log $E/$2sweep-NBIO-$1-*.log; }
{
echo "== attempt on 0.5.4: the plan that showed the register missing from the driver table (GCMC_VM_CACHEABLE_DRAM_ADDRESS_END)"
echo "   (compared here with the corrected trace filter: the plan itself was right, the table and the first filter were not)"
$C writes $E/attempt-0.5.4/gfx-plan-x-*.txt | tail -3
echo; echo "== H3: the plan of stages 1 to 5"; $C writes $E/gfx-plan-x-*.txt
echo; echo "== H3: state after the plan, control = the sweep after the PSP load"
$C state --control $(s loaded) --after $(s afterplan)
echo; echo "== H4 to H8: the executed writes of the seven calls (gfx-run-s1 .. s7), the whole step against the trace"
$C writes --whole $E/gfx-run-s1-*.txt $E/gfx-run-s2-*.txt $E/gfx-run-s3-*.txt $E/gfx-run-s4-*.txt $E/gfx-run-s5-*.txt $E/gfx-run-s6-*.txt $E/gfx-run-s7-*.txt
previous=loaded
for n in 1 2 3 4 5 6 7; do
    echo; echo "== stage $n: state, control = the sweep before it ($previous)"
    $C state --control $(s $previous) --after $(s s$n)
    previous=s$n
done
echo; echo "== H9a: state after gfx fini, control = the sweep after the PSP load"
$C state --control $(s loaded) --after $(s fini)
echo; echo "== after the device restart that followed, control = before, before2"
$C state --control $(s before) $(s before2) --after $(s restarted)
echo; echo "== boot 2: all seven stages in one call, the whole step against the trace"
$C writes --whole $E/boot-2/gfx-run-all-*.txt
echo; echo "== boot 2, H10: state after a device restart while the engines ran, control = before"
$C state --control $(s before boot-2/) --after $(s restarted boot-2/)
} 2>&1
