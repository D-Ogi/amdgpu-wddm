@echo off
"C:\BC250\m9\gpu-residency0798-v2\gpu-residency-probe.exe" 1073741824 vram > "C:\BC250\m9\gpu-residency0798-v2\vram1g.out" 2> "C:\BC250\m9\gpu-residency0798-v2\vram1g.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency0798-v2\vram1g.exit"
