# Run on the lab. Does not install anything or change driver registration.
param([Parameter(Mandatory)][string]$Config,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$c=Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
if(Test-Path $Out){throw 'Result directory already exists'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
New-Item -ItemType Directory $Out | Out-Null
$expected=@{};$seen=@{}
foreach($m in $c.modules){
 if($expected.ContainsKey($m.name)){throw 'Duplicate module'}
 if((Get-FileHash -LiteralPath $m.path).Hash -ne $m.sha256){throw "Hash mismatch: $($m.name)"}
 $expected[$m.name]=$m
}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$si=New-Object Diagnostics.ProcessStartInfo
$si.FileName=$c.exe;$si.Arguments=$c.arguments;$si.UseShellExecute=$false;$si.CreateNoWindow=$true
$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
$p=New-Object Diagnostics.Process;$p.StartInfo=$si
$status='FAIL';$code=125;$errorText='';$stdout=$null;$stderr=$null
try {
 [void]$p.Start();$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while(!$p.HasExited){
  $p.Refresh()
  foreach($module in $p.Modules){
   if($expected.ContainsKey($module.ModuleName) -and !$seen.ContainsKey($module.ModuleName)){
    $want=$expected[$module.ModuleName]
    if([IO.Path]::GetFullPath($module.FileName) -ine [IO.Path]::GetFullPath($want.path)){throw "Unexpected module path: $($want.name)"}
    $hash=(Get-FileHash -LiteralPath $module.FileName).Hash
    if($hash -ne $want.sha256){throw "Loaded module hash mismatch: $($want.name)"}
    $seen[$want.name]=$hash
   }
  }
  if($watch.Elapsed.TotalSeconds -gt $c.timeout_seconds){throw 'Smoke deadline'}
  Start-Sleep -Milliseconds 100
 }
 $p.WaitForExit();$code=$p.ExitCode
 if($code -ne 0){throw "Native exit $code"}
 if($stdout.Result -notmatch $c.pass_pattern){throw 'Missing content oracle PASS'}
 foreach($name in $expected.Keys){if(!$seen.ContainsKey($name)){throw "No live module witness: $name"}}
 if((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o') -ne $boot){throw 'Boot changed'}
 $status='PASS'
} catch {$errorText=$_.Exception.Message}
finally {
 if($p.Id -and !$p.HasExited){Stop-Process -Id $p.Id -Force;$p.WaitForExit()}
 if($stdout){$stdout.Result | Set-Content "$Out\stdout.txt"}
 if($stderr){$stderr.Result | Set-Content "$Out\stderr.txt"}
 @{status=$status;exit_code=$code;loaded_artifacts=$seen;boot=$boot;error=$errorText;config_sha256=(Get-FileHash $Config).Hash} | ConvertTo-Json -Depth 5 | Set-Content "$Out\receipt.json"
}
if($status -ne 'PASS'){throw $errorText}
Write-Output 'Smoke PASS: content, live module identity, unchanged boot'
