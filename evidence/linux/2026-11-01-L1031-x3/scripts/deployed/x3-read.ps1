# Read-only X3 audit. Does not run the acknowledgement or the GUI probe.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'x3-env.ps1')
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
if ($v.Count -ne 1) { throw 'expected one mounted BC250DIAG volume' }
$disk = @(Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk)
if ($disk.Count -ne 1 -or $v[0].FileSystem -ne 'FAT32' -or $disk[0].BusType -ne 'USB' -or $disk[0].Size -ne 64160400896) { throw 'stick identity mismatch' }
$root = "$($v[0].DriveLetter):\"
$envf = Join-Path $root 'boot\grub\grubenv'
if (-not (Test-Path -LiteralPath (Join-Path $root 'boot\grub\bc250diag.id') -PathType Leaf)) { throw 'X3 marker missing' }
$b = [IO.File]::ReadAllBytes($envf)
$lines = Read-X3Env $b
$counter = @($lines | Where-Object { $_.StartsWith('bc250_try=', [StringComparison]::Ordinal) })
'grubenv bytes={0} counter={1}' -f $b.Length, $(if ($counter.Count) { $counter[0].Trim() } else { 'absent (clear)' })
'grubenv sha256=' + (Get-FileHash -LiteralPath $envf -Algorithm SHA256).Hash
(Select-String -LiteralPath (Join-Path $root 'boot\grub\grub.cfg') -Pattern '^set default=').Line
foreach ($relative in @('boot\grub\grub.cfg','efi\boot\bootx64.efi','efi\boot\bootx64.off','efi\boot\bootx64-alpine.efi')) {
    $path = Join-Path $root $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) { '{0} sha256={1}' -f $relative, (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
}
'windows boot ' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$task = @(Get-ScheduledTask -TaskName 'Lab boot counter reset' -ErrorAction SilentlyContinue)
if ($task.Count -eq 0) { 'X3 task absent' } else {
    $info = Get-ScheduledTaskInfo -TaskName 'Lab boot counter reset'
    'X3 task state={0} lastResult={1} lastRun={2:o}' -f $task[0].State, $info.LastTaskResult, $info.LastRunTime
    $task[0].Principal | Select-Object LogonType, RunLevel
    $task[0].Triggers | Select-Object @{n='Kind';e={$_.CimClass.CimClassName}},Delay
}
