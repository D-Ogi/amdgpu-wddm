$ErrorActionPreference='Stop'
. "$PSScriptRoot\request-audit-checkpoint.ps1"
Add-Type -TypeDefinition 'using System; using System.IO; using System.Threading.Tasks; public static class CheckpointTestWriter { public static Task Append(string path, string first, string last) { return Task.Run(async () => { await Task.Delay(150); File.AppendAllText(path, first); await Task.Delay(80); File.AppendAllText(path, last); }); }}'
$root=Join-Path $PSScriptRoot ('checkpoint-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
try {
 $log=Join-Path $root 'audit.log';$marker=Join-Path $root 'marker.txt'
 [IO.File]::WriteAllText($log,'')
 $clock=[Diagnostics.Stopwatch]::StartNew()
 $params=@{MarkerPath=$marker;LogPath=$log;ProcessId=$PID;ProcessStartUtc=(Get-Process -Id $PID).StartTime.ToUniversalTime();TrialClock=$clock;DeadlineSeconds=20;TimeoutMilliseconds=1500}
 $writer=[CheckpointTestWriter]::Append($log,'BC250 audit lifetime event=checkpoint seq=1 time_ns=1 marker=1 requests=0 successful=0 failed=0 ended=0 pending=',"0 live=0`n")
 $receipt=Request-AuditCheckpoint @params -Marker 1
 [void]$writer.GetAwaiter().GetResult()
 if($receipt.marker -ne 1){throw 'Missing positive acknowledgement'}
 $rejected=0
 try {Request-AuditCheckpoint @params -Marker 1 | Out-Null} catch {if($_ -notmatch 'must increase'){throw};$rejected++}
 $writer=[CheckpointTestWriter]::Append($log,'BC250 audit lifetime event=checkpoint seq=2 time_ns=2 marker=2 requests=1 successful=0 failed=0 ended=0 pending=',"1 live=0`n")
 try {Request-AuditCheckpoint @params -Marker 2 | Out-Null} catch {if($_ -notmatch 'Pending map'){throw};$rejected++}
 [void]$writer.GetAwaiter().GetResult()
 $params.TimeoutMilliseconds=100
 try {Request-AuditCheckpoint @params -Marker 3 | Out-Null} catch {if($_ -notmatch 'acknowledgement deadline'){throw};$rejected++}
 $params.DeadlineSeconds=0
 try {Request-AuditCheckpoint @params -Marker 4 | Out-Null} catch {if($_ -notmatch 'Trial deadline'){throw};$rejected++}
 $params.DeadlineSeconds=20;$params.ProcessStartUtc=[datetime]::UtcNow.AddDays(-1)
 try {Request-AuditCheckpoint @params -Marker 4 | Out-Null} catch {if($_ -notmatch 'identity changed'){throw};$rejected++}
 if($rejected -ne 5){throw 'Negative control did not reject'}
 [pscustomobject]@{positive_partial_line=$true;rejected_controls=$rejected;lab_used=$false}|ConvertTo-Json
} finally {
 $resolved=[IO.Path]::GetFullPath($root)
 if(!$resolved.StartsWith([IO.Path]::GetFullPath($PSScriptRoot)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup boundary'}
 Remove-Item -LiteralPath $resolved -Recurse -Force
}
