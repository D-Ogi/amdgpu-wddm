@echo off
"C:\BC250\m9\gpu-residency0798-v4\gpu-residency-probe.exe" 67108864 gtt > "C:\BC250\m9\gpu-residency0798-v4\gtt64m.out" 2> "C:\BC250\m9\gpu-residency0798-v4\gtt64m.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency0798-v4\gtt64m.exit"
