# Step 0 of the Performant Labs comparison block: set the DPM ceiling to 2000 MHz.
# Decision 2026-10-10: the release ceiling is 2000; the base clock stays 1000 and idle 500.
# Writes exactly one value, DpmMaxMHz, under the KMD service parameters. It applies at the next restart.
# No other registry value, no BIOS, no NVRAM, no fan and no voltage setting is touched.
$ErrorActionPreference = 'Stop'
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$before = Get-ItemProperty $par
'before: DpmMode={0} DpmMaxMHz={1} DpmIdleMHz={2} CuMode={3} applied={4} TdrDelay-note=see GraphicsDrivers' -f `
    $before.DpmMode, $before.DpmMaxMHz, $before.DpmIdleMHz, $before.CuMode, $before.CuModeLastApplied
if ($before.DpmMode -ne 1) { 'REFUSED DpmMode is not 1; this block does not change the mode'; exit 3 }
Set-ItemProperty -Path $par -Name 'DpmMaxMHz' -Value 2000 -Type DWord
$after = Get-ItemProperty $par
'after: DpmMode={0} DpmMaxMHz={1} DpmIdleMHz={2}' -f $after.DpmMode, $after.DpmMaxMHz, $after.DpmIdleMHz
if ($after.DpmMaxMHz -ne 2000) { 'FAILED read-back is not 2000'; exit 4 }
'ok: DpmMaxMHz=2000 written, applies at the next restart'
