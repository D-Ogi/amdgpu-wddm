$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
function Deadline([double]$Seconds){[Diagnostics.Stopwatch]::GetTimestamp()+[long]($Seconds*[Diagnostics.Stopwatch]::Frequency)}
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$initial=@{flags=7;generation=[uint64]4294967296;epoch=5;completed=1;age_ms=0;ready_ms=59999}
$script:reads=0;$script:eligible=New-Object Collections.Generic.List[bool]
$result=Wait-KmdConfirmEligible $initial -Deadline (Deadline 2) -IntervalMs 1 -Read {
 $script:reads++;$h=$initial.Clone();if($script:reads -gt 1){$h.ready_ms=60000};$h
} -Record {param($s);$script:eligible.Add($s.eligible)}
if($script:reads -ne 2 -or $script:eligible[0] -or !$script:eligible[1] -or $result.ready_ms -ne 60000){throw 'Premature age admission'}
Must-Reject {Wait-KmdConfirmEligible $initial -Deadline (Deadline 1) -Read {$h=$initial.Clone();$h.epoch=6;$h}}
Must-Reject {Wait-KmdConfirmEligible $initial -Deadline (Deadline 1) -Read {$h=$initial.Clone();$h.generation++;$h}}
# Even an otherwise eligible read returned after the absolute deadline cannot pass.
Must-Reject {Wait-KmdConfirmEligible $initial -Deadline (Deadline 0.02) -Read {Start-Sleep -Milliseconds 35;$h=$initial.Clone();$h.ready_ms=60000;$h}}
foreach($mode in @('young','stale','no-work')){
 $watch=[Diagnostics.Stopwatch]::StartNew()
 Must-Reject {Wait-KmdConfirmEligible $initial -Deadline (Deadline 0.05) -IntervalMs 1 -Read {
  $h=$initial.Clone()
  if($mode -eq 'stale'){$h.ready_ms=60000;$h.age_ms=15001}
  if($mode -eq 'no-work'){$h.ready_ms=60000;$h.completed=0}
  $h
 }}
 if($watch.Elapsed.TotalSeconds -gt 1){throw 'Wait exceeded bounded tolerance'}
}
$confirmed=$initial.Clone();$confirmed.flags=15;$confirmed.ready_ms=60000
$r=Wait-KmdConfirmEligible $initial -Deadline (Deadline 1) -Read {$confirmed}
Assert-KmdConfirmedHealth $initial $r
Assert-KmdFreshWork $r
'PASS: wait age/freshness/no-work, changed identity, post-deadline read and externally confirmed state'
