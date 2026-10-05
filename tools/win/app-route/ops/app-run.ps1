# LAB (elevated SSH): one bounded client run (at most 170 s) for the application-routing ladder. Output in
# <Root>\runs\<UTC>-<client>\ (run.json, the client's result.json/stdout/stderr, child.json when interactive) and
# run.json on stdout.
#   d3d11bench | d3d11mt | d3d11fl12   the package's clients (hash-checked), offscreen; in this SSH session (session 0)
#                                      unless -Interactive. --out and a --deadline below -Seconds are added.
#   dxdiag    dxdiag /t <run>\dxdiag.txt in the interactive session (its Feature Levels lines are the FL witness)
#   taskmgr   Task Manager in the interactive session for -Seconds, then ended (screenshots from the host meanwhile)
#   exe       -Exe <absolute path> in the interactive session for -Seconds, then its process tree ended
# Interactive runs go through the one-shot task named in app-route.json (user bc250, logged on, highest run level),
# removed at the end. Refused under the owner STOP flag.
param([Parameter(Mandatory)][ValidateSet('d3d11bench','d3d11mt','d3d11fl12','dxdiag','taskmgr','exe')][string]$Client,
      [string]$Arguments='',[ValidateRange(5,170)][int]$Seconds=120,[switch]$Interactive,[string]$Exe='',
      [string]$Root='C:\BC250\m14\app-route-001')
$ErrorActionPreference='Stop'
$clock=[Diagnostics.Stopwatch]::StartNew()
. "$Root\ops\approute-lib.ps1"
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$run=Join-Path $Root "runs\$stamp-$Client"
$r=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');client=$Client;run=$run;seconds=$Seconds}
$task=$null
function Step([string]$s){try{Add-Content -LiteralPath "$run\steps.txt" -Value ('{0} {1}' -f $clock.ElapsedMilliseconds,$s)}catch{}}
try {
 $m=Read-AppRouteManifest $Root
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 $null=New-Item -ItemType Directory -Force -Path $run
 $console=$true;$kill=$false
 switch($Client){
  {$_ -in @('d3d11bench','d3d11mt','d3d11fl12')} {
   $name="$Client.exe"
   $exePath=Join-Path $Root "clients\$name"
   if((Get-AppFileSha $exePath) -ne [string]$m.clients.$name){throw "$name is not the package's"}
   $a=$Arguments
   if($Client -eq 'd3d11fl12' -and !$a){$a='--adapter 1002 --min 12_1'}
   if($a -notmatch '--deadline'){$a+=" --deadline $([Math]::Max(5,$Seconds-10))"}
   $a+=" --out `"$run\result.json`""
  }
  'dxdiag' {$exePath="$env:windir\System32\dxdiag.exe";$a="/t `"$run\dxdiag.txt`"";$console=$false;$Interactive=$true}
  'taskmgr' {$exePath="$env:windir\System32\Taskmgr.exe";$a='';$console=$false;$kill=$true;$Interactive=$true}
  'exe' {
   if($Exe -notmatch '^[A-Za-z]:\\.+\.exe$' -or !(Test-Path -LiteralPath $Exe -PathType Leaf)){throw '-Exe must name an existing absolute .exe'}
   $exePath=$Exe;$a=$Arguments;$console=$false;$kill=$true;$Interactive=$true
  }
 }
 $r.exe=$exePath;$r.arguments=$a;$r.interactive=[bool]$Interactive
 $r.exe_sha256=Get-AppFileSha $exePath
 if(!$Interactive){
  $p=Start-Process -FilePath $exePath -ArgumentList $a -PassThru -NoNewWindow -WorkingDirectory $run `
       -RedirectStandardOutput "$run\stdout.txt" -RedirectStandardError "$run\stderr.txt"
  $r.pid=$p.Id;$null=$p.Handle
  Step 'started'
  if(!$p.WaitForExit($Seconds*1000)){& taskkill.exe /T /F /PID $p.Id | Out-Null;$r.killed=$true;$null=$p.WaitForExit(10000)}
  Step 'waited'
  $r.exit_code=if($p.HasExited){$p.ExitCode}else{$null}
  Step 'exit read'
 } else {
  $task=[string]$m.client_task
  if(@(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Running' }).Count){$task=$null;throw 'Client task already running'}
  $spec=[ordered]@{exe=$exePath;arguments=$a;seconds=$Seconds;console=$console;kill_at_end=$kill}
  [IO.File]::WriteAllText("$run\spec.json",($spec|ConvertTo-Json))
  Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
  $act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$Root\ops\app-child.ps1`" -Run `"$run`""
  $pr=New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Highest
  $st=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds+30)) -AllowStartIfOnBatteries
  $null=Register-ScheduledTask -TaskName $task -Action $act -Principal $pr -Settings $st
  Start-ScheduledTask -TaskName $task
  $deadline=[Math]::Min($Seconds+20,175)
  while($clock.Elapsed.TotalSeconds -lt $deadline -and !(Test-Path -LiteralPath "$run\child.json")){Start-Sleep -Milliseconds 500}
  if(Test-Path -LiteralPath "$run\child.json"){$r.child=Get-Content -LiteralPath "$run\child.json" -Raw | ConvertFrom-Json;$r.exit_code=$r.child.exit_code}
  else{$r.child='missing at the bound'}
 }
 if(Test-Path -LiteralPath "$run\result.json"){$raw=Get-Content -LiteralPath "$run\result.json" -Raw;try{$r.result=$raw | ConvertFrom-Json}catch{$r.result_text=@($raw -split "`r?`n" | Where-Object { $_ })}}
 Step 'result read'
 if(Test-Path -LiteralPath "$run\stdout.txt"){$r.stdout_tail=@(Get-Content -LiteralPath "$run\stdout.txt" -Tail 20 | ForEach-Object { [string]$_ })}
 Step 'stdout read'
 if(Test-Path -LiteralPath "$run\dxdiag.txt"){$r.dxdiag_feature_levels=@(Select-String -LiteralPath "$run\dxdiag.txt" -Pattern 'Feature Levels|DDI Version|Driver Name|Card name' | ForEach-Object { $_.Line.Trim() })}
 $r.outcome='ok'
} catch {
 $r.outcome='failed';$r.error=[string]$_
} finally {
 if($task){
  $t=Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
  if($t){if($t.State -eq 'Running'){Stop-ScheduledTask -TaskName $task};Unregister-ScheduledTask -TaskName $task -Confirm:$false}
 }
}
 Step 'finally done'
$r.elapsed_s=$clock.Elapsed.TotalSeconds
$json=$r|ConvertTo-Json -Depth 8
if(Test-Path -LiteralPath $run){[IO.File]::WriteAllText("$run\run.json",$json)}
$json
if($r.outcome -ne 'ok'){exit 1}
