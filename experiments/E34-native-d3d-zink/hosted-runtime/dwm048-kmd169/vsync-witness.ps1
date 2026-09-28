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
function Save-KmdVsyncWitness {
 param([string]$Cli,[string]$LogPath,[string]$ReceiptPath)
 if(!('KmdVsyncClock' -as [type])){Add-Type 'using System;using System.Runtime.InteropServices;public static class KmdVsyncClock{[DllImport("kernel32.dll")]public static extern void QueryInterruptTime(out ulong time);}' }
 [uint64]$begin=0;[uint64]$end=0
 [KmdVsyncClock]::QueryInterruptTime([ref]$begin)
 & $Cli log summary *> $LogPath
 $code=$LASTEXITCODE
 [KmdVsyncClock]::QueryInterruptTime([ref]$end)
 if($code -ne 0){throw 'VSync summary escape failed'}
 $w=Get-KmdVsyncWitness (Get-Content $LogPath -Raw) $end
 $w.read_begin_100ns=$begin;$w.read_duration_ms=($end-$begin)/10000.0
 Write-DurableText $ReceiptPath ($w|ConvertTo-Json)
 return $w
}
