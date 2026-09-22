#!/bin/bash
# E23 run 001 (draft): the sdmacopy positive control on bc250kmd 0.7.21. Owner present, FRESH BOOT first (M78).
# Prerequisites: bash scratch/tmp/e16_install0721.sh umd   (package + cli in C:\BC250\e16-umd)
cd /p/BC-250/bc250-win || exit 1
S="python tools/win/target.py ps experiments/E15-compute-dispatch/e15_target.ps1 -Package C:\\BC250\\e16-umd"
T="python tools/win/bc250rd/temp.py"
echo "=== temperature"; $T 2>&1 | tail -1
echo "=== gate on"; $S -Phase gate -On 1 2>&1 | tail -6
echo "=== EnableVramWrite (read at the device start, so one more restart)"; python tools/win/target.py ps /p/BC-250/scratch/tmp/e23_vramwrite.ps1 2>&1 | grep -v "^Warning" | tail -3
echo "=== gart enable"; $S -Phase gart -Op enable 2>&1 | tail -4
echo "=== psp load"; $S -Phase psp -Op load 2>&1 | tail -6
echo "=== gfx run 7 (through SDMA, no interrupt sources)"; $S -Phase gfx -Op run -Stage 7 -Tag sdmacopy 2>&1 | tail -10
echo "=== sdma ring test s0 (control)"; $S -Phase fence -Op s0 -Count 1 -Mode test 2>&1 | tail -3
echo "=== sdmacopy 4096"; python tools/win/target.py run "C:\BC250\e16-umd\bc250kmd_cli.exe sdmacopy 4096" 2>&1 | tail -14
echo "=== sdmacopy 65536"; python tools/win/target.py run "C:\BC250\e16-umd\bc250kmd_cli.exe sdmacopy 65536" 2>&1 | tail -14
echo "=== temperature"; $T 2>&1 | tail -1
echo "=== gfx fini"; $S -Phase gfx -Op fini -Tag sdmacopy 2>&1 | tail -4
echo "=== psp unload + gart restore (the full undo, M78)"; $S -Phase psp -Op unload 2>&1 | tail -3; $S -Phase gart -Op restore 2>&1 | tail -3
echo "=== gate off"; $S -Phase gate -On 0 2>&1 | tail -4
echo "=== log"; python tools/win/target.py run "C:\BC250\e16-umd\bc250kmd_cli.exe log summary" 2>&1 | tail -10
