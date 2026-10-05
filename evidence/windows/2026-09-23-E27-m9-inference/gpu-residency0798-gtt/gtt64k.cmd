@echo off
"C:\BC250\m9\gpu-residency0798-v4\gpu-residency-probe.exe" 65536 gtt > "C:\BC250\m9\gpu-residency0798-v4\gtt64k.out" 2> "C:\BC250\m9\gpu-residency0798-v4\gtt64k.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency0798-v4\gtt64k.exit"
