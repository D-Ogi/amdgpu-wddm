@echo off
"C:\BC250\m9\gpu-residency-v5-results\gpu-residency-probe.exe" 67108864 vram --resident-only > "C:\BC250\m9\gpu-residency-v5-results\resident64m.out" 2> "C:\BC250\m9\gpu-residency-v5-results\resident64m.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency-v5-results\resident64m.exit"
