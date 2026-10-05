function Get-KmdVsyncWitness {
 param([string]$Text,[uint64]$ReadEnd100ns)
 $headers=[regex]::Matches($Text,'wddm summary: vsync (enabled|not enabled by ControlInterrupt), [0-9]+ ticks, ([0-9]+) reported to dxgkrnl')
 if(!$headers.Count){throw 'VSync summary missing'}
 $header=$headers[$headers.Count-1];$tail=$Text.Substring($header.Index)
 $patterns=@{
  flip='vidpn flip (?:open|closed): ([0-9]+) hardware flips, [0-9]+ refused, [0-9]+ hardware vsyncs armed ([0-9]+) acked [0-9]+ refused ([0-9]+) completion-deferred ([0-9]+) old-buffer-reports'
  vector='vsync vector: DPC polls ([0-9]+) ACKs ([0-9]+) sync-failures ([0-9]+)'
  errors='vsync diagnostic: no-event ([0-9]+) read-fail ([0-9]+) ack-fail ([0-9]+)'
  times='vsync diagnostic: 100ns irq ([0-9]+) entry ([0-9]+) ack ([0-9]+) notify ([0-9]+) status [0-9A-Fa-f]+'
 }
 $m=@{}
 foreach($name in $patterns.Keys){$found=[regex]::Matches($tail,$patterns[$name]);if($found.Count -ne 1){throw "Missing/ambiguous current VSync field: $name"};$m[$name]=$found[0]}
 $ack=[uint64]$m.times.Groups[3].Value;$notify=[uint64]$m.times.Groups[4].Value
 if(!$ack -or !$notify -or $ack -gt $ReadEnd100ns -or $notify -gt $ReadEnd100ns){throw 'VSync timestamps missing or outside sampled clock'}
 return @{
  enabled=($header.Groups[1].Value -eq 'enabled');reports=[uint64]$header.Groups[2].Value
  flips=[uint64]$m.flip.Groups[1].Value;ticks=[uint64]$m.flip.Groups[2].Value
  deferred=[uint64]$m.flip.Groups[3].Value;old_buffer=[uint64]$m.flip.Groups[4].Value
  dpc_polls=[uint64]$m.vector.Groups[1].Value;dpc_acks=[uint64]$m.vector.Groups[2].Value;sync_failures=[uint64]$m.vector.Groups[3].Value
  no_event=[uint64]$m.errors.Groups[1].Value;read_failures=[uint64]$m.errors.Groups[2].Value;ack_failures=[uint64]$m.errors.Groups[3].Value
  ack_100ns=$ack;notify_100ns=$notify;read_end_100ns=$ReadEnd100ns
  ack_age_upper_ms=($ReadEnd100ns-$ack)/10000.0;notify_age_upper_ms=($ReadEnd100ns-$notify)/10000.0
 }
}
# API-set contract lists QueryInterruptTime; do not assume a kernel32 export.
# https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/realtimeapiset/nf-realtimeapiset-queryinterrupttime.md
function Get-KmdInterruptTime {
 if(!('KmdVsyncClock' -as [type])){Add-Type 'using System;using System.Runtime.InteropServices;public static class KmdVsyncClock{[DllImport("api-ms-win-core-realtime-l1-1-2.dll", ExactSpelling=true)]public static extern void QueryInterruptTime(out ulong time);}' }
 [uint64]$value=0
 [KmdVsyncClock]::QueryInterruptTime([ref]$value)
 if(!$value){throw 'Interrupt clock unavailable'}
 return $value
}
function Save-KmdVsyncWitness {
 param([string]$Cli,[string]$LogPath,[string]$ReceiptPath)

 [uint64]$begin=0;[uint64]$end=0
 $begin=Get-KmdInterruptTime
 & $Cli log summary *> $LogPath
 $code=$LASTEXITCODE
 $end=Get-KmdInterruptTime
 if($code -ne 0){throw 'VSync summary escape failed'}
 $w=Get-KmdVsyncWitness (Get-Content $LogPath -Raw) $end
 $w.read_begin_100ns=$begin;$w.read_duration_ms=($end-$begin)/10000.0
 Write-DurableText $ReceiptPath ($w|ConvertTo-Json)
 return $w
}
function Assert-KmdVsyncInterval {
 param($Start,$End,[double]$ExpectedRefreshHz=60)
 if(!$Start.enabled -or !$End.enabled){throw 'VSync reporting not enabled at both boundaries'}
 foreach($counter in @('sync_failures','read_failures','ack_failures')){
  if($End[$counter] -ne $Start[$counter]){throw "VSync error counter changed: $counter"}
 }
 $delta=@{}
 foreach($counter in @('ticks','reports','deferred','old_buffer','dpc_acks')){
  if($End[$counter] -lt $Start[$counter]){throw "VSync counter regressed: $counter"}
  $delta[$counter]=[decimal]$End[$counter]-[decimal]$Start[$counter]
 }
 $seconds=([decimal]$End.read_begin_100ns-[decimal]$Start.read_begin_100ns)/10000000
 if($seconds -le 0 -or $ExpectedRefreshHz -le 0){throw 'Invalid VSync interval or expected refresh'}
 # Read-begin ages are lower bounds: an event during the read yields a negative age.
 # Retain read-end upper bounds in the original receipts; neither is an atomic snapshot.
 $ackAge=([decimal]$End.read_begin_100ns-[decimal]$End.ack_100ns)/10000
 $notifyAge=([decimal]$End.read_begin_100ns-[decimal]$End.notify_100ns)/10000
 if($ackAge -ge 100 -or $notifyAge -ge 100){throw 'VSync ACK or notification was stale before the final read'}
 $rate=$delta.ticks/$seconds
 if($rate -lt 0.95*$ExpectedRefreshHz){throw 'VSync ACK rate below trial acceptance threshold'}
 $skipped=$delta.deferred-$delta.old_buffer
 if($skipped -lt 0){throw 'Inconsistent deferred/old-buffer interval; cannot validate independently sampled counters'}
 if($skipped -gt 0.01*$delta.ticks){throw 'More than one percent of VSync ACKs deferred without old-buffer notification'}
 if($delta.reports -lt $delta.ticks-$skipped){throw 'VSync notifications do not cover acknowledged interval'}
 return @{seconds=$seconds;expected_refresh_hz=$ExpectedRefreshHz;ack_hz=$rate;delta=$delta;skipped=$skipped;ack_age_lower_ms=$ackAge;notify_age_lower_ms=$notifyAge;dpc_recovered_ack_observed=($delta.dpc_acks -gt 0);pass=$true}
}
