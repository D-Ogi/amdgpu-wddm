param([Parameter(Mandatory)][ValidateSet('Capture','Install','Cpu','Gpu','Restore','Verify')][string]$Phase)
$ErrorActionPreference='Stop'
$d='C:\BC250\m14\runtime001'
if($PSScriptRoot -ine $d){throw 'Unexpected trial directory'}
. "$d\registration.ps1"
. "$d\durable.ps1"
. "$d\file-routing.ps1"
$active='C:\BC250\m11\resource-close\bc250d3d.dll'
$baseline='8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'
$manifest=Get-Content "$d\stage-manifest.json" -Raw|ConvertFrom-Json
$candidate=$manifest.files.'bc250d3d-router.dll'
function Check-StopThermal {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 $text=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1|Out-String
 if($LASTEXITCODE -ne 0 -or $text -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -or [double]$Matches[1] -ge 85){throw 'Temperature unavailable or above limit'}
}
if($Phase -eq 'Capture'){
 Check-StopThermal
 $raw=& "$d\preflight171.ps1"|Out-String
 Write-DurableText "$d\before.json" $raw
 Copy-VerifiedDurable $active "$d\bc250d3d-cpu.dll" $baseline
}elseif($Phase -in @('Install','Restore')){
 if($Phase -eq 'Install'){
  Check-StopThermal
  Install-M14FileRoute $active "$d\bc250d3d-cpu.dll" "$d\original-umd.dll" "$d\bc250d3d-router.dll" $baseline $candidate
 }else{
  if(Test-Path "$d\enable"){Remove-Item -LiteralPath "$d\enable" -Force}
  Restore-M14FileRoute $active "$d\bc250d3d-cpu.dll" $baseline $candidate
 }
 Write-DurableText "$d\$Phase-file.json" (@{sha256=(Get-FileHash -LiteralPath $active).Hash}|ConvertTo-Json)
}elseif($Phase -in @('Cpu','Gpu')){
 Check-StopThermal
 $gpu=($Phase -eq 'Gpu')
 Remove-Item Env:BC250_M14_RUNTIME_PROBE -ErrorAction SilentlyContinue
 Remove-Item Env:BC250_M14_CAPTURE_DUMP -ErrorAction SilentlyContinue
 if($gpu){Write-DurableText "$d\enable" 'M14 process only';$env:BC250_M14_RUNTIME_PROBE='1';$env:BC250_M14_CAPTURE_DUMP='1'}
 $env:DXVK_SHADER_CACHE='0';$env:MESA_SHADER_CACHE_DISABLE='true';$env:DXVK_LOG_PATH=$d
 $env:DXVK_LOG_LEVEL='info'
 # Functional debugger run only. Its timings cannot establish the performance bound.
 $args=@('25',"$d\d3d11bench.exe",'--mode','offscreen','--adapter','1002:13fe','--size','64x64','--scenes','draws,fill,shaders','--draws','8','--layers','2','--shaders','4','--frames','3','--warmup','0','--deadline','20','--out',"$d\$Phase.json")
 $child=Start-Process -FilePath "$d\debug-child.exe" -ArgumentList $args -WorkingDirectory $d -WindowStyle Hidden -PassThru -RedirectStandardOutput "$d\$Phase-debug.txt" -RedirectStandardError "$d\$Phase-stderr.txt"
 $null=$child.Handle;$clock=[Diagnostics.Stopwatch]::StartNew();$last=0
 try{
  while(!$child.WaitForExit(250)){
   if($clock.Elapsed.TotalSeconds -ge 32){throw 'Debugger deadline'}
   if($clock.Elapsed.TotalSeconds-$last -ge 2){Check-StopThermal;$last=$clock.Elapsed.TotalSeconds}
  }
  $child.Refresh()
  if($child.ExitCode -ne 0){throw "Runtime control exit $($child.ExitCode)"}
 }finally{if(!$child.HasExited){$child.Kill();$null=$child.WaitForExit(5000)}}
 $result=Get-Content "$d\$Phase.json" -Raw|ConvertFrom-Json
 if($result.result -ne 'measured' -or $result.exit -ne 0 -or $result.d3d11 -ne 'system'){throw 'Runtime result failed'}
 . "$d\scene-gate.ps1"
 if($gpu){Assert-M14Scenes $result (Get-Content "$d\Cpu.json" -Raw|ConvertFrom-Json)}else{Assert-M14Scenes $result}
 $paths=@($result.modules|ForEach-Object {$_.path})
 if("$env:windir\System32\d3d11.dll" -notin $paths -or $active -notin $paths){throw 'System runtime/router not witnessed'}
 if($gpu){
  foreach($name in @('bc250d3d11.dll','bc250dxvk.dll','bc250radv.dll')){if("$d\$name" -notin $paths){throw "Missing module $name"}}
  if(@($result.icds).Count -ne 1 -or $result.icds[0].path -ine "$d\bc250radv.dll" -or $result.icds[0].sha256 -ine 'C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0'){throw 'Wrong GPU ICD'}
 }else{
  if("$d\bc250d3d-cpu.dll" -notin $paths -or @($result.icds).Count){throw 'CPU path not witnessed'}
  foreach($name in @('bc250d3d11.dll','bc250dxvk.dll','bc250radv.dll')){if("$d\$name" -in $paths){throw 'GPU module in negative selection control'}}
 }
}elseif($Phase -eq 'Verify'){
 $raw=& "$d\preflight171.ps1"|Out-String
 Write-DurableText "$d\after.json" $raw
 $before=Get-Content "$d\before.json" -Raw|ConvertFrom-Json;$after=$raw|ConvertFrom-Json
 if($before.boot -ne $after.boot -or $before.confirmed.generation -ne $after.confirmed.generation -or $before.confirmed.epoch -ne $after.confirmed.epoch){throw 'OS/driver generation changed'}
 if(!(Test-M14RegistrationEqual $before.umd_registration $after.umd_registration) -or !(Test-M14RegistrationEqual $before.icd_registration $after.icd_registration)){throw 'Registration changed'}
 if(($before.dwm|ConvertTo-Json -Depth 5 -Compress) -cne ($after.dwm|ConvertTo-Json -Depth 5 -Compress)){throw 'DWM identity/modules changed'}
}
Write-DurableText "$d\$Phase-done.json" (@{phase=$Phase;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
