# X3 fault control from Windows: set bc250_try=3 (keeps other variables), then restart. Expected next boot:
# GRUB sees the limit and boots entry 4 (network-only Linux). Refuses while this boot's acknowledgement is pending.
$ErrorActionPreference = 'Stop'
. 'C:\BC250\x3\x3-env.ps1'
. 'C:\BC250\x3\x3-storage.ps1'
$name = 'Lab boot counter reset'
$i = Get-ScheduledTaskInfo -TaskName $name
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
if ((Get-ScheduledTask -TaskName $name).State -ne 'Ready' -or $i.LastTaskResult -ne 0 -or $i.LastRunTime.ToUniversalTime() -lt $boot) {
    throw "acknowledgement of this boot not finished with 0 (state/result/run: $((Get-ScheduledTask -TaskName $name).State) $($i.LastTaskResult) $($i.LastRunTime))"
}
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
$disk = @(Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk)
if ($v.Count -ne 1 -or $v[0].FileSystem -ne 'FAT32' -or $disk[0].BusType -ne 'USB' -or $disk[0].Size -ne 64160400896) { throw 'stick identity mismatch' }
$envf = "$($v[0].DriveLetter):\boot\grub\grubenv"
$old = [IO.File]::ReadAllBytes($envf)
$lines = Read-X3Env $old
$text = "# GRUB Environment Block`nbc250_try=3`n"
foreach ($line in $lines) { if (-not $line.StartsWith('bc250_try=', [StringComparison]::Ordinal)) { $text += $line } }
$new = [Text.Encoding]::ASCII.GetBytes($text + ('#' * (1024 - $text.Length)))
[void](Read-X3Env $new)
$tag = 'set3-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
Write-X3Durable "$envf.x3-new-$tag" $new
Write-X3Durable "$envf.x3-backup-$tag" $old
[IO.File]::Delete($envf)
[IO.File]::Move("$envf.x3-new-$tag", $envf)
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($envf)) -cne [Convert]::ToBase64String($new)) { throw 'readback mismatch' }
'grubenv now: ' + (((Read-X3Env ([IO.File]::ReadAllBytes($envf))) | ForEach-Object { $_.Trim() }) -join ' ')
'sha256=' + (Get-FileHash $envf -Algorithm SHA256).Hash
'restart at ' + [DateTime]::UtcNow.ToString('o')
shutdown.exe /r /t 5 /c "BC-250 lab: X3 test, counter 3 -> Linux expected"
