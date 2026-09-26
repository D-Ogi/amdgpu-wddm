$ErrorActionPreference='Continue'
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object EnableFullWddm,LastStage,StageHistory,UnconfirmedStarts | Format-List
Get-ChildItem C:\BC250\kmdlog | Sort-Object LastWriteTime -Descending | Select-Object -First 4 Name,Length,LastWriteTime | Format-Table
$last=Get-ChildItem C:\BC250\kmdlog | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if($last){ Get-Content $last.FullName -Tail 180 }
Get-ChildItem C:\Windows\Minidump,C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue | Select-Object Name,Length,LastWriteTime | Format-Table
& C:\BC250\m8\bc250kmd_cli.exe log summary
