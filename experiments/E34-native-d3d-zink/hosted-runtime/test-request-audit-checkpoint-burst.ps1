param([Parameter(Mandatory)][string]$Helper,[Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. $Helper
if(Test-Path $Out){throw 'Existing control output'}
New-Item -ItemType Directory $Out | Out-Null
Add-Type -TypeDefinition 'using System; using System.IO; using System.Threading.Tasks; public static class BurstWriter { public static Task Append(string path,string marker,string text) { return Task.Run(async () => { var stop=DateTime.UtcNow.AddSeconds(10); while(!File.Exists(marker) && DateTime.UtcNow<stop) await Task.Delay(10); if(!File.Exists(marker)) throw new Exception("Marker not published"); File.AppendAllText(path,text); }); }}'
$raw=[IO.File]::ReadAllText($Fixture)
$match=[regex]::Match($raw,'(?m)^BC250 audit lifetime event=checkpoint [^\r\n]* marker=1 [^\r\n]*\r?\n')
if(!$match.Success){throw 'Fixture lacks marker1'}
$prefix=$raw.Substring(0,$match.Index+$match.Length)
$log=Join-Path $Out 'audit.log';$marker=Join-Path $Out 'marker.txt'
[IO.File]::WriteAllText($log,'')
$clock=[Diagnostics.Stopwatch]::StartNew()
$writer=[BurstWriter]::Append($log,$marker,$prefix)
$result=[ordered]@{fixture_bytes=$prefix.Length;pass=$false;error=$null;elapsed_ms=0;receipt=$null}
try{$result.receipt=Request-AuditCheckpoint -MarkerPath $marker -LogPath $log -Marker 1 -ProcessId $PID -ProcessStartUtc (Get-Process -Id $PID).StartTime.ToUniversalTime() -TrialClock $clock -DeadlineSeconds 20 -TimeoutMilliseconds 4000;$result.pass=$true}catch{$result.error=$_.Exception.Message}
$result.elapsed_ms=$clock.Elapsed.TotalMilliseconds
[void]$writer.GetAwaiter().GetResult()
$result|ConvertTo-Json -Depth 4|Set-Content (Join-Path $Out 'result.json')
$result|ConvertTo-Json -Depth 4
