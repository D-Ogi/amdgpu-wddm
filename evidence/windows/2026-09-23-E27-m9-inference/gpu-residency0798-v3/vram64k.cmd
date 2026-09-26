@echo off
"C:\BC250\m9\gpu-residency0798-v3\gpu-residency-probe.exe" 65536 vram > "C:\BC250\m9\gpu-residency0798-v3\vram64k.out" 2> "C:\BC250\m9\gpu-residency0798-v3\vram64k.err"
echo %ERRORLEVEL% > "C:\BC250\m9\gpu-residency0798-v3\vram64k.exit"
