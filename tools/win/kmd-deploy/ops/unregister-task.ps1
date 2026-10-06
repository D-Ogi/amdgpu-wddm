# Recovery step 5 of the kmd173 RUNBOOK (recovery-required): unregister the transition task by hand, only once it
# is not Running and RestoreDetector has run. Generic copy of the kmd175-deploy002 one-off.
$t = Get-ScheduledTask -TaskName 'BC250-KMD-Watch' -ErrorAction SilentlyContinue
if ($t) { "state $($t.State)"; if ($t.State -ne 'Running') { Unregister-ScheduledTask -TaskName 'BC250-KMD-Watch' -Confirm:$false; 'unregistered' } else { 'still running, left' } } else { 'task absent' }
