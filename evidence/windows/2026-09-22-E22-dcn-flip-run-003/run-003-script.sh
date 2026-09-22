#!/bin/bash
# E22 run 003 (step 3): bc250kmd 0.7.24, the VidPn flip and the VUPDATE interrupt in the full table. Owner at the monitor:
# the desktop is shown through SetVidPnSourceAddress's own hardware flip, vsync from OTG0's interrupt. No engines are
# needed (EnableMmio + EnableDcnWrite + EnableVidPnFlip; the CPU blit of E20 stays on). About a minute of desktop, ring
# log lines, DCN dump, temperature, gate closed. Usage: e22_run3.sh
cd /p/BC-250/bc250-win || exit 1
S=/p/BC-250/scratch/tmp
T="python tools/win/target.py"
V=0.7.24
CLI='C:\BC250\e16\bc250kmd_cli.exe'
step() { $T ps experiments/E19-full-wddm-stage-c/e19_target.ps1 "$@" 2>&1 | cut -c1-220; }
nostop() { python tools/win/bc250mon/mon.py 'stop?' 2>&1 | tail -1 | grep -qi 'no stop'; }
temp() { python tools/win/bc250rd/temp.py 2>&1 | tail -1; }
nostop || { echo "STOP flag set: not starting"; exit 3; }
bash $S/mon_status.sh "E22 run 3: bc250kmd $V, full table + hardware flip and vsync interrupt (EnableVidPnFlip). Screen may go black; the desktop should return through the flip" warn
step -Phase state -Tag pre-e22r3 | grep -E 'stages |driver ' | tee $S/e22r3_state_pre.txt
grep -q 'last 61' $S/e22r3_state_pre.txt || { echo "not at stage 61: not starting"; exit 4; }
grep -q "$V" $S/e22r3_state_pre.txt || { echo "driver is not $V: not starting"; exit 4; }
step -Phase confirm | tail -1
temp
echo "=== gate -Full 1 -GpuVa 1 -Blit 1 -VidPnFlip 1 (no engines)"
step -Phase gate -Full 1 -GpuVa 1 -Blit 1 -VidPnFlip 1 | grep -E 'device |stages |gates |gate  |events |presents'
sleep 20
python tools/win/bc250mon/mon.py screenshot 2>&1 | tail -2
python tools/win/bc250mon/mon.py scanout --out /p/BC-250/scratch/screens/e22r3-flip-half.png 2>&1 | tail -2
$T run "$CLI dcn" 2>&1 | grep -E 'HUBPREQ0_DCSURF_(PRIMARY|FLIP_CONTROL )|OTG0_OTG_(STATUS_FRAME|GLOBAL_SYNC)|HUBP0_DCHUBP|decoded|hubp0|otg0' | head -12
sleep 20
$T run "$CLI log summary" 2>&1 | grep -i -E 'vsync|flip|blit|refus|counters|present' | head -20
step -Phase log -Tag e22r3 | tail -1
$T ps 'P:\BC-250\scratch\tmp\e24_lines.ps1' 2>&1 | cut -c1-210
temp
echo "=== gate closed (the stop restores the firmware's surface address)"
step -Phase gate -Full 0 | grep -E 'device |stages |events |reports|presents'
step -Phase confirm | tail -1
python tools/win/bc250mon/mon.py screenshot 2>&1 | tail -1
temp
