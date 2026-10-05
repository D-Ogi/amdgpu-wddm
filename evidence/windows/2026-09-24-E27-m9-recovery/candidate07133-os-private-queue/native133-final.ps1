$ErrorActionPreference='Stop'
& C:\BC250\m8\bc250kmd_cli.exe log summary
if($LASTEXITCODE -ne 0){throw 'Summary failed'}
& C:\BC250\m8\bc250kmd_cli.exe info
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-Process dwm | ForEach-Object {
 'dwm_pid='+$_.Id+' start='+$_.StartTime.ToString('s')+' responding='+$_.Responding
 $_.Modules | Where-Object ModuleName -eq 'bc250d3d.dll' | ForEach-Object {'dwm_module='+$_.FileName+' sha256='+(Get-FileHash $_.FileName).Hash}
}
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object EnableFullWddm,UnconfirmedStarts,EnableSdmaIbControl,EnableSdmaVaControl,EnablePresentBlit,EnableDcnWrite,EnableVidPnFlip | Format-List
'final_time='+(Get-Date).ToString('s')
'final_check_complete'
