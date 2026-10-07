# L1007b switch, Windows side: stage the kit on the stick, set DpmMaxMHz 1000 for the C62 open-loop identification at
# the next Windows start (back to 1500 afterwards), state before the switch. The stick steering and the restart are
# separate calls (lab-emerg.py usb-boot linux:4, then the restart).
$ErrorActionPreference = 'Continue'
$stick = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.FileSystem -eq 'FAT32' } | Select-Object -First 1
"stick: $($stick.DriveLetter): $($stick.FileSystemLabel) $([int]($stick.Size / 1GB)) GB"
$dst = "$($stick.DriveLetter):\l1007b"
$null = New-Item -ItemType Directory -Force -Path $dst
Copy-Item C:\BC250\tmp\l1007b\run.sh, C:\BC250\tmp\l1007b\regs2.py $dst -Force
# LF line endings for sh
foreach ($f in 'run.sh', 'regs2.py') { $t = [IO.File]::ReadAllText("$dst\$f") -replace "`r`n", "`n"; [IO.File]::WriteAllText("$dst\$f", $t) }
Get-ChildItem $dst | ForEach-Object { '{0} {1}' -f $_.Name, $_.Length }
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty $k -Name DpmMaxMHz -Type DWord -Value 1000
'DpmMaxMHz now ' + (Get-ItemProperty $k).DpmMaxMHz
'STOP flag: ' + (Test-Path C:\BC250\mon\STOP)
'last boot: ' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s') + 'Z'
