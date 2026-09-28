$ErrorActionPreference='Stop'
. "$PSScriptRoot\vsync-witness.ps1"
$t=@(
'wddm summary: vsync enabled, 0 ticks, 98 reported to dxgkrnl',
'wddm summary: vidpn flip open: 30 hardware flips, 0 refused, 1 hardware vsyncs armed 100 acked 0 refused 2 completion-deferred 1 old-buffer-reports',
'vsync vector: DPC polls 101 ACKs 3 sync-failures 0',
'vsync diagnostic: no-event 2 read-fail 0 ack-fail 0',
'vsync diagnostic: 100ns irq 900000 entry 900000 ack 900000 notify 910000 status 00101104') -join "`n"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$w=Get-KmdVsyncWitness $t 1000000
if($w.ticks -ne 100 -or $w.dpc_acks -ne 3 -or $w.ack_age_upper_ms -ne 10 -or $w.notify_age_upper_ms -ne 9){throw 'Counter/clock decoding failed'}
Must-Reject {Get-KmdVsyncWitness ($t+"`nwddm summary: vsync enabled, 0 ticks, 99 reported to dxgkrnl") 1000000}
Must-Reject {Get-KmdVsyncWitness ($t -replace 'vsync vector:','missing:') 1000000}
Must-Reject {Get-KmdVsyncWitness $t 800000}
Must-Reject {Get-KmdVsyncWitness ($t -replace 'ack 900000','ack 0') 1000000}
'PASS: KMD170 layout, timestamp bounds, newest-block and missing/future/zero controls'
$start=@{enabled=$true;sync_failures=0;read_failures=0;ack_failures=0;ticks=100;reports=100;deferred=0;old_buffer=0;dpc_acks=0;read_begin_100ns=10000000}
$end=@{enabled=$true;sync_failures=0;read_failures=0;ack_failures=0;ticks=6100;reports=6100;deferred=0;old_buffer=0;dpc_acks=1;read_begin_100ns=1010000000;ack_100ns=1009990000;notify_100ns=1009990000}
$r=Assert-KmdVsyncInterval $start $end
if($r.ack_hz -ne 60 -or !$r.dpc_recovered_ack_observed){throw 'Positive interval control failed'}
$bad=$end.Clone();$bad.ack_100ns=1008000000;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.ticks=101;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.sync_failures=1;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.deferred=61;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.reports=6099;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.ticks=99;Must-Reject {Assert-KmdVsyncInterval $start $bad}
$bad=$end.Clone();$bad.ack_100ns=1010010000;Assert-KmdVsyncInterval $start $bad|Out-Null
'PASS: interval rate, stale ACK, error, skip, report coverage, regression and during-read controls'
$skip="wddm summary: vsync enabled`nvsync skip: odd 1 read 0 same 2 changed 0`nvsync skip100ns: odd 900000 read 0 same 950000 changed 0"
$r=Get-KmdVsyncSkipWitness $skip 1000000
if($r.odd.count -ne 1 -or $r.same.last_100ns -ne 950000){throw '170 skip decode'}
Must-Reject {Get-KmdVsyncSkipWitness ($skip -replace 'odd 900000','odd 0') 1000000}
Must-Reject {Get-KmdVsyncSkipWitness $skip 940000}
Must-Reject {Get-KmdVsyncSkipWitness ($skip+"`nwddm summary: vsync enabled") 1000000}
'PASS:170 reason counters/times, missing newest block and invalid stamps'
