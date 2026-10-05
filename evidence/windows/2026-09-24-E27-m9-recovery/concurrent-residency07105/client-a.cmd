@echo off
"C:\BC250\m9\concurrent-residency07105\gpu-residency-probe.exe" 67108864 vram > "C:\BC250\m9\concurrent-residency07105\client-a.out" 2> "C:\BC250\m9\concurrent-residency07105\client-a.err"
echo %ERRORLEVEL% > "C:\BC250\m9\concurrent-residency07105\client-a.exit"
