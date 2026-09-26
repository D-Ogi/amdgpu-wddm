@echo off
"C:\BC250\m9\pagefile32-pnp-residency\gpu-residency-probe.exe" 67108864 vram > "C:\BC250\m9\pagefile32-pnp-residency\boot64m.out" 2> "C:\BC250\m9\pagefile32-pnp-residency\boot64m.err"
echo %ERRORLEVEL% > "C:\BC250\m9\pagefile32-pnp-residency\boot64m.exit"
