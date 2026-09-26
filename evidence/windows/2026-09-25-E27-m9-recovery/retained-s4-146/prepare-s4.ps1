$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) {throw 'Owner STOP requested'}
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
if($LASTEXITCODE -ne 0){throw 'Health unavailable'}
'power_states_before'
& powercfg.exe /a
& powercfg.exe /hibernate on
if($LASTEXITCODE -ne 0){throw 'Enable hibernation failed'}
& powercfg.exe /hibernate /type full
if($LASTEXITCODE -ne 0){throw 'Full hibernation failed'}
'power_states_after'
& powercfg.exe /a
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power' | Select-Object HibernateEnabled,HiberFileType | Format-List
'hibernate_prepared_no_transition'
