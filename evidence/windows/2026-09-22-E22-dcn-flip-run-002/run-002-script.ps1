# E22 run 002: one gated display flip (bc250kmd 0.7.20). Owner at the monitor. Display-only mode.
# Gates: EnableMmio + EnableDcnWrite + EnableVram + EnableVramWrite = 1 for this device start; all closed after.
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\kmd\bc250kmd_cli.exe'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' } | Select-Object -First 1
"gpu $($gpu.InstanceId) $($gpu.Status)"
Set-ItemProperty $params -Name UnconfirmedStarts -Value 0 -Type DWord   # the overlay confirms only after ~30 s of desktop
Set-ItemProperty $params -Name EnableMmioWrite -Value 0 -Type DWord
Set-ItemProperty $params -Name EnableMmio -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVram -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVramWrite -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableDcnWrite -Value 1 -Type DWord
"disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 4
"enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 8
& $cli info 2>&1 | Select-String 'version|last stage|gates|mode'
"== step 0: dump (baseline)"
& $cli dcn 2>&1 | Select-String 'HUBPREQ0_|HUBP0_|OTG0_OTG_(CONTROL|STATUS_FRAME|GLOBAL_SYNC)|decoded|hubp0|otg0'
"== step 1 (H4): no-op flip to the firmware address"
& $cli dcnflip 0x270000000 2>&1
Start-Sleep -Seconds 3
"== step 2 (H5): flip to a filled surface at 0x271000000 (blue field, white border, diagonal)"
& $cli dcnflip 0x271000000 fill 0xFF2060C0 2>&1
Start-Sleep -Seconds 8
"== step 2b: registers while the fill is on screen, and the scanout read back"
& $cli fbdump C:\BC250\tmp\e22-002-fill.bmp 2>&1 | Select-Object -Last 1
& $cli dcn 2>&1 | Select-String 'HUBPREQ0_DCSURF_(PRIMARY|SURFACE_INUSE|FLIP_CONTROL )|HUBP0_DCHUBP|OTG0_OTG_STATUS_FRAME'
"== step 3: restore"
& $cli dcnflip restore 2>&1
Start-Sleep -Seconds 2
"== step 4: dump after restore, scanout read back"
& $cli fbdump C:\BC250\tmp\e22-002-restored.bmp 2>&1 | Select-Object -Last 1
& $cli dcn 2>&1 | Select-String 'HUBPREQ0_DCSURF_(PRIMARY|SURFACE_INUSE|FLIP_CONTROL )|HUBP0_DCHUBP|OTG0_OTG_STATUS_FRAME|decoded|hubp0|otg0'
foreach ($n in 'EnableDcnWrite', 'EnableVramWrite', 'EnableVram', 'EnableMmio') { Set-ItemProperty $params -Name $n -Value 0 -Type DWord }
"gates closed (take effect at the next device start)"
& $cli log summary 2>&1 | Select-Object -Last 12
(Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' }).Status
