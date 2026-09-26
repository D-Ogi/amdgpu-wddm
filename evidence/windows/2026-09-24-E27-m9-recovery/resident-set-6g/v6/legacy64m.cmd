@echo off
"C:\BC250\m9\gpu-residency-v6-results\gpu-residency-probe.exe" 67108864 vram > "C:\BC250\m9\gpu-residency-v6-results\legacy64m.out" 2> "C:\BC250\m9\gpu-residency-v6-results\legacy64m.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency-v6-results\legacy64m.exit"
