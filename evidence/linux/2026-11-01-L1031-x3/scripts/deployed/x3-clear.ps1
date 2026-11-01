# LAB ONLY: interactive administrator logon, never SYSTEM/session 0.
# A timer schedules an observation; it never acknowledges a boot by itself.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. (Join-Path $PSScriptRoot 'x3-env.ps1')
. (Join-Path $PSScriptRoot 'x3-storage.ps1')
. (Join-Path $PSScriptRoot 'x3-process.ps1')
$log = Join-Path $PSScriptRoot 'x3-clear.log'
function L([string]$Message) { Add-Content -LiteralPath $log -Value ('{0} {1}' -f [DateTime]::UtcNow.ToString('o'), $Message) }
function Get-X3Desktop {
    $reader = Join-Path $PSScriptRoot 'x3-session.ps1'
    $ps = "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
    $out = Invoke-X3Bounded $ps @('-NoProfile','-NonInteractive','-WindowStyle','Hidden','-ExecutionPolicy','Bypass','-File',$reader)
    return ($out | ConvertFrom-Json)
}
try {
    if ([Diagnostics.Process]::GetCurrentProcess().SessionId -le 0) { throw 'requires an interactive logon; session 0 is not a boot witness' }
    $boot = (Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 5).LastBootUpTime.ToUniversalTime().ToString('o')
    $first = Get-X3Desktop
    $h1 = $null; $h2 = $null
    if ($first.ForceCpu -eq 0) {
        $install = [string](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
        if (-not $install -or -not [IO.Path]::IsPathRooted($install)) { throw 'missing installed driver root' }
        $cli = Join-Path $install 'tools\bc250kmd_cli.exe'
        $h1 = Read-X3Health (Invoke-X3Bounded $cli @('health','read')) 0
    }
    Start-Sleep -Seconds 2
    $second = Get-X3Desktop
    if ($second.ForceCpu -eq 0) { $h2 = Read-X3Health (Invoke-X3Bounded $cli @('health','read')) 0 }
    # These are service/task states, NOT authenticated channel handshakes.
    $ssh = (Get-Service -Name sshd).Status -eq 'Running'
    $emergency = (Get-ScheduledTask -TaskName 'Lab emergency channel').State -eq 'Running'
    $route = Assert-X3Boot $first $second $boot $ssh $emergency $h1 $h2
    if ((Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 5).LastBootUpTime.ToUniversalTime().ToString('o') -cne $boot) { throw 'boot changed' }

    $v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
    if ($v.Count -ne 1) { throw 'expected one mounted BC250DIAG volume' }
    $disk = @(Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk)
    if ($disk.Count -ne 1 -or $v[0].FileSystem -ne 'FAT32' -or $disk[0].BusType -ne 'USB' -or $disk[0].Size -ne 64160400896) { throw 'stick identity mismatch' }
    $root = "$($v[0].DriveLetter):\"
    $envf = Join-Path $root 'boot\grub\grubenv'
    if (-not (Test-Path -LiteralPath (Join-Path $root 'boot\grub\bc250diag.id') -PathType Leaf)) { throw 'stick marker missing' }
    $result = Update-X3Counter $envf
    L "verified boot=$boot route=$route; $result"
    exit 0
} catch {
    $message = $_.Exception.Message
    try { L "ERROR $message" } catch { }
    [Console]::Error.WriteLine("X3 not acknowledged: $message")
    exit 1
}
