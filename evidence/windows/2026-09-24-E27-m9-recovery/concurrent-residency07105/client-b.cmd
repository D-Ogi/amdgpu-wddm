@echo off
"C:\BC250\m9\concurrent-residency07105\gpu-residency-probe.exe" 67108864 vram > "C:\BC250\m9\concurrent-residency07105\client-b.out" 2> "C:\BC250\m9\concurrent-residency07105\client-b.err"
echo %ERRORLEVEL% > "C:\BC250\m9\concurrent-residency07105\client-b.exit"
