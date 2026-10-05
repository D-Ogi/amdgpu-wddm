@echo off
"C:\BC250\m9\native131-residency\gpu-residency-probe.exe" 1073741824 vram > "C:\BC250\m9\native131-residency\native1g.out" 2> "C:\BC250\m9\native131-residency\native1g.err"
echo %ERRORLEVEL% > "C:\BC250\m9\native131-residency\native1g.exit"
