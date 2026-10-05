# LAB (elevated SSH, read-only): per-thread CPU rates of one process over -Seconds: user and kernel milliseconds
# per second for the -Top busiest threads, with the thread's state and wait reason at the end of the window.
param([string]$Image = 'ROTTR', [int]$Seconds = 5, [int]$Top = 10)
$p = Get-Process -Name $Image -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Image process"; exit 2 }
function Snap($proc) {
    $m = @{}
    foreach ($t in $proc.Threads) {
        try { $m[$t.Id] = @($t.UserProcessorTime.TotalMilliseconds, $t.PrivilegedProcessorTime.TotalMilliseconds) } catch { }
    }
    $m
}
$a = Snap $p
$t0 = Get-Date
Start-Sleep -Seconds $Seconds
$p.Refresh()
$b = Snap $p
$dt = ((Get-Date) - $t0).TotalSeconds
$rows = foreach ($id in $b.Keys) {
    if (-not $a.ContainsKey($id)) { continue }
    $u = ($b[$id][0] - $a[$id][0]) / $dt; $k = ($b[$id][1] - $a[$id][1]) / $dt
    [pscustomobject]@{ Tid = $id; UserMsPerS = [math]::Round($u, 1); KernelMsPerS = [math]::Round($k, 1); Sum = $u + $k }
}
"{0} pid {1} window {2:N1} s, process cpu {3:N1} s" -f $Image, $p.Id, $dt, $p.TotalProcessorTime.TotalSeconds
$states = @{}
foreach ($t in $p.Threads) { try { $states[$t.Id] = '{0} {1}' -f $t.ThreadState, $(if ($t.ThreadState -eq 'Wait') { $t.WaitReason } else { '' }) } catch { } }
$rows | Sort-Object Sum -Descending | Select-Object -First $Top | ForEach-Object {
    '  tid {0,6} user {1,7} ms/s kernel {2,7} ms/s  {3}' -f $_.Tid, $_.UserMsPerS, $_.KernelMsPerS, $states[$_.Tid]
}
