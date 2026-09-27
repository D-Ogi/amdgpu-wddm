param(
 [Parameter(Mandatory)][ValidatePattern('^C:\\BC250\\m13\\[a-zA-Z0-9_-]+(?:\\[a-zA-Z0-9_-]+)*$')][string]$Directory,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ProbeSha256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$DwmUmdSha256,
 [Parameter(Mandatory)][int]$ExpectedDwmPid,
 [Parameter(Mandatory)][string]$ExpectedDwmStartUtc,
 [switch]$GdiPaint)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\handshake-observation.ps1"
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$exe=Join-Path $Directory 'redirblt-probe.exe'
function Receipt([string]$Name,$Value){
 $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 8))
 $stream=[IO.FileStream]::new((Join-Path $Directory $Name),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
 try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
function Snapshot {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 $dwm=@(Get-Process dwm)
 if($dwm.Count -ne 1 -or $dwm[0].Id -ne $ExpectedDwmPid -or $dwm[0].StartTime.ToUniversalTime().ToString('o') -ne $ExpectedDwmStartUtc){throw 'DWM identity changed'}
 $mods=@($dwm[0].Modules | Where-Object {$_.ModuleName -like 'bc250d3d*'} | ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
 if(!($mods|Where-Object {$_.sha256 -eq $DwmUmdSha256})){throw 'Expected DWM UMD not loaded'}
 $health=& $cli health read|Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A4 flags=15 '){throw 'Confirmed KMD164 required'}
 $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read|Out-String
 if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
 return @{utc=[DateTime]::UtcNow.ToString('o');dwm_pid=$dwm[0].Id;dwm_start=$ExpectedDwmStartUtc;modules=$mods;health=$health;clock=$clock;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
}
if(!(Test-Path -LiteralPath $Directory -PathType Container)){throw 'Missing fresh staged directory'}
if((Test-Path "$Directory\done.json") -or (Test-Path "$Directory\stdout.txt") -or (Test-Path "$Directory\stderr.txt")){throw 'Existing attempt; inspect original'}
Receipt 'started.json' @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;gdi_paint=[bool]$GdiPaint;probe_sha256=$ProbeSha256}
$p=$null;$failure=$null;$observation=$null;$before=$null;$code=1
try {
 if((Get-Process -Id $PID).SessionId -eq 0){throw 'Interactive session required'}
 if((Get-FileHash $exe).Hash -ne $ProbeSha256){throw 'Reviewed probe hash mismatch'}
 $before=Snapshot;Receipt 'before.json' $before
 $info=& $cli info|Out-String
 if($LASTEXITCODE -ne 0){throw 'Adapter info failed'}
 $luids=[regex]::Matches($info,'(?im)adapter\s+handle\s+0x[0-9a-f]+\s+luid\s+([0-9a-f]{8})-([0-9a-f]{8})')
 if($luids.Count -ne 1){throw 'Adapter LUID ambiguous'}
 $luid=$luids[0].Groups[1].Value+':'+$luids[0].Groups[2].Value
 $arguments=@('--handshake-only','--no-open','--luid',$luid,'--size','641x479','--pump','500','--hold','1','--handshake-timeout','2000','--timeout','40')
 if($GdiPaint){$arguments+='--gdi-paint'}
 Receipt 'arguments.json' @{arguments=$arguments}
 $p=Start-Process $exe -ArgumentList $arguments -WindowStyle Normal -PassThru -RedirectStandardOutput "$Directory\stdout.txt" -RedirectStandardError "$Directory\stderr.txt"
 $handle=$p.Handle
 Receipt 'process.json' @{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while(!$p.WaitForExit(200)){
  if($timer.Elapsed.TotalSeconds -ge 45){throw 'Probe exceeded45 seconds; terminate original process'}
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 }
 $p.Refresh()
 $observation=Get-HandshakeObservation -Transcript (Get-Content "$Directory\stdout.txt" -Raw) -ExitCode $p.ExitCode
 Receipt 'observation.json' $observation
 $after=Snapshot;Receipt 'after.json' $after
 if($after.boot -ne $before.boot){throw 'Boot changed'}
 $code=0
}catch{$failure=@{message=$_.Exception.Message;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}}
finally{
 if($p -and !$p.HasExited){try{$p.Kill();[void]$p.WaitForExit(5000)}catch{$failure=@{message='Failed to terminate original probe';detail=$_.Exception.Message};$code=1}}
 Receipt 'done.json' @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;failure=$failure;process_alive=($p -and !$p.HasExited);observation=$observation}
}
exit $code
