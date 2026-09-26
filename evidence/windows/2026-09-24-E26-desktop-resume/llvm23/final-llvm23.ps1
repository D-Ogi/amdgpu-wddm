$ErrorActionPreference='Stop'
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=[datetime]'2026-09-24T13:52:00'} -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | Format-List
$gpu=@(Get-PnpDevice -Class Display | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*')
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
Get-ItemProperty $class | Select-Object UserModeDriverName | Format-List
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object Enable*,LastStage,UnconfirmedStarts | Format-List
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
& C:\BC250\m8\bc250kmd_cli.exe info
& C:\BC250\m8\bc250kmd_cli.exe dcn
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m8\bc250kmd_cli.exe log
& C:\BC250\m8\bc250kmd_cli.exe confirm
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'final_time='+(Get-Date).ToString('s')
'initialized_desktop_retained'

Get-Process dwm | ForEach-Object {
 'dwm_renderer_pid='+$_.Id
 $_.Modules | Where-Object ModuleName -eq 'bc250d3d.dll' | Select-Object ModuleName,FileName | Format-List
 $file='C:\BC250\e26\umdlogs\mesa-'+$_.Id+'.txt'
 if(Test-Path $file){Select-String -Path $file -Pattern 'BC250 Renderer:|BC250 Perf' | ForEach-Object {$_.Line}}
}
$s=Invoke-RestMethod http://127.0.0.1:2250/state
$s.panels | Where-Object name -eq 'graphics' | ConvertTo-Json -Depth 8
