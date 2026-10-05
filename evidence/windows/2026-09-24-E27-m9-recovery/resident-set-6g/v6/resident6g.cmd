@echo off
"C:\BC250\m9\gpu-residency-v6-results\gpu-residency-probe.exe" 6442450944 vram --resident-only > "C:\BC250\m9\gpu-residency-v6-results\resident6g.out" 2> "C:\BC250\m9\gpu-residency-v6-results\resident6g.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency-v6-results\resident6g.exit"
