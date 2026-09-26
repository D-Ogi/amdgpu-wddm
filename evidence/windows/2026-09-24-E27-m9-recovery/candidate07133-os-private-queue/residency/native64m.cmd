@echo off
"C:\BC250\m9\native133-residency\gpu-residency-probe.exe" 67108864 vram > "C:\BC250\m9\native133-residency\native64m.out" 2> "C:\BC250\m9\native133-residency\native64m.err"
echo %ERRORLEVEL% > "C:\BC250\m9\native133-residency\native64m.exit"
