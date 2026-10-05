# LAB (elevated SSH, read-only): last boot time, bugcheck records since a given UTC time, newest dump files,
# and whether a given game image is running. Used after the owner reports a blue screen.
param([string]$SinceUtc = '', [string]$Image = 'ROTTR')
$os = Get-CimInstance Win32_OperatingSystem
'boot={0:yyyy-MM-ddTHH:mm:ssZ} now={1:yyyy-MM-ddTHH:mm:ssZ}' -f $os.LastBootUpTime.ToUniversalTime(), (Get-Date).ToUniversalTime()
$since = if ($SinceUtc) { [datetime]::Parse($SinceUtc).ToLocalTime() } else { (Get-Date).AddHours(-1) }
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 1001, 41, 6008, 1074, 6005, 6006 } -ErrorAction SilentlyContinue)
foreach ($e in ($ev | Sort-Object TimeCreated)) {
    $msg = ($e.Message -replace '\s+', ' ')
    if ($msg.Length -gt 300) { $msg = $msg.Substring(0, 300) }
    'event {0:HH:mm:ssZ} id={1} {2}: {3}' -f $e.TimeCreated.ToUniversalTime(), $e.Id, $e.ProviderName, $msg
}
foreach ($p in @('C:\Windows\MEMORY.DMP', 'C:\Windows\Minidump', 'C:\Windows\LiveKernelReports')) {
    if (Test-Path $p) {
        Get-ChildItem $p -Recurse -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 3 |
            ForEach-Object { 'dump {0} {1:N0} bytes {2:HH:mm:ssZ}' -f $_.FullName, $_.Length, $_.LastWriteTime.ToUniversalTime() }
    }
}
$g = @(Get-Process -Name $Image -ErrorAction SilentlyContinue)
'game {0}: {1}' -f $Image, $(if ($g.Count) { ($g | ForEach-Object { 'pid {0} start {1:HH:mm:ssZ}' -f $_.Id, $_.StartTime.ToUniversalTime() }) -join ', ' } else { 'not running' })
$cc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -ErrorAction SilentlyContinue
'crashcontrol AutoReboot={0} CrashDumpEnabled={1}' -f $cc.AutoReboot, $cc.CrashDumpEnabled
