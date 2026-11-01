# X3 live acceptance, step 0 (read-only): stick state, counter bytes, driver install root, logon account, health.
$ErrorActionPreference = 'Stop'
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
'volumes={0}' -f $v.Count
$disk = Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk
'{0}: fs={1} bus={2} size={3}' -f $v[0].DriveLetter, $v[0].FileSystem, $disk.BusType, $disk.Size
$root = "$($v[0].DriveLetter):\"
$envf = Join-Path $root 'boot\grub\grubenv'
$b = [IO.File]::ReadAllBytes($envf)
$t = [Text.Encoding]::ASCII.GetString($b)
'grubenv bytes={0} sha256={1}' -f $b.Length, (Get-FileHash -LiteralPath $envf -Algorithm SHA256).Hash
'grubenv vars: ' + (($t.Substring(25) -split "`n" | Where-Object { $_ -and $_ -notmatch '^#+$' }) -join ' | ')
(Select-String -LiteralPath (Join-Path $root 'boot\grub\grub.cfg') -Pattern '^set default=').Line
Get-ChildItem (Join-Path $root 'efi\boot'), (Join-Path $root 'boot\grub') -File | ForEach-Object { '{0} {1} {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.Substring(0,16) }
'install root: ' + (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$w = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
'autologon={0} user={1}' -f $w.AutoAdminLogon, $w.DefaultUserName
'admins: ' + ((Get-LocalGroupMember -SID 'S-1-5-32-544' | ForEach-Object Name) -join ', ')
'DwmForceCpu=' + (Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter' -Name DwmForceCpu -ErrorAction SilentlyContinue).DwmForceCpu
'boot ' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
'sshd=' + (Get-Service sshd).Status + ' emerg=' + (Get-ScheduledTask -TaskName 'Lab emergency channel').State
'x3 task: ' + @(Get-ScheduledTask -TaskName 'Lab boot counter reset' -ErrorAction SilentlyContinue).Count
'conhost: ' + (Test-Path "$env:WINDIR\System32\conhost.exe")
Test-Path C:\BC250\x3
