# End the client of an arm that failed or hit its bound, so that the next arm does not start beside it.
# Generic: the names are arguments, so one copy serves every train and every demo.
#
#   kill-clients.ps1 -Names q2rtx[,DXRPathTracer,ROTTR] [-Tasks bc250-pt-run]
#
# The lab runners bound their own GPU work (pt-run.ps1 registers its client with an ExecutionTimeLimit of
# Seconds + 30), so this is a backstop for the case where the host gave up on the ssh call and nothing on
# this side had asked the client to go. It kills the named processes only; it never touches the driver, the
# overlay (bc250mon), sshd or the emergency channel.
param([Parameter(Mandatory = $true)][string]$Names,
      [string]$Tasks = '')
$ErrorActionPreference = 'Continue'
$keep = @('bc250mon', 'sshd', 'dwm', 'winlogon', 'csrss', 'services', 'lsass', 'System', 'Idle')
foreach ($name in ($Names -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    if ($keep -contains $name) { "kill refused: $name is not a client of an arm"; continue }
    $found = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if (-not $found.Count) { "gone process $name"; continue }
    "ending process {0} pid {1}" -f $name, ($found.Id -join ',')
    $found | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 700
    $left = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if ($left.Count) { "LEFT process {0} pid {1}" -f $name, ($left.Id -join ',') } else { "ended process $name" }
}
foreach ($task in ($Tasks -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    if (-not $t) { "gone task $task"; continue }
    try { Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue } catch {}
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    "ended task $task"
}
'kill-clients end'
