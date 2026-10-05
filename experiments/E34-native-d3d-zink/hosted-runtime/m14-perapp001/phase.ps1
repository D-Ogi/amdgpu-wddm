param([Parameter(Mandatory)][ValidateSet('Capture','Install','Cpu','Gpu','Restore','Verify')][string]$Phase)
$ErrorActionPreference='Stop'
$d='C:\BC250\m14\perapp001'
if($PSScriptRoot -ine $d){throw 'Unexpected trial directory'}
. "$d\registration.ps1"
. "$d\durable.ps1"
. "$d\file-routing.ps1"
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baseline='CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'
$manifest=Get-Content "$d\stage-manifest.json" -Raw|ConvertFrom-Json
$candidate=$manifest.files.'bc250radv.dll'
function Check-StopThermal {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 $text=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1|Out-String
 if($LASTEXITCODE -ne 0 -or $text -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -or [double]$Matches[1] -ge 85){throw 'Temperature unavailable or above limit'}
}
if($Phase -eq 'Capture'){
 Check-StopThermal
 $raw=& "$d\preflight171.ps1"|Out-String
 Write-DurableText "$d\before.json" $raw
 Copy-VerifiedDurable $active "$d\icd-baseline.dll" $baseline
 if(Test-Path "$d\app"){throw 'Existing app directory'}
 $null=New-Item -ItemType Directory "$d\app"
 foreach($name in @('d3d11bench.exe','d3d11.dll','dxgi.dll')){
  $staged=if($name -eq 'd3d11bench.exe'){$name}else{'perapp-'+$name}
  Copy-VerifiedDurable "$d\$staged" "$d\app\$name" $manifest.files.$staged
 }
}elseif($Phase -in @('Install','Restore')){
 if($Phase -eq 'Install'){
  Check-StopThermal
  Install-M14FileRoute $active "$d\icd-baseline.dll" "$d\original-icd.dll" "$d\bc250radv.dll" $baseline $candidate
 }else{
  if(Test-Path "$d\enable"){Remove-Item -LiteralPath "$d\enable" -Force}
  Restore-M14FileRoute $active "$d\icd-baseline.dll" $baseline $candidate
 }
 Write-DurableText "$d\$Phase-file.json" (@{sha256=(Get-FileHash -LiteralPath $active).Hash}|ConvertTo-Json)
}elseif($Phase -in @('Cpu','Gpu')){
 Check-StopThermal
 $gpu=($Phase -eq 'Gpu')
 Remove-Item Env:BC250_M14_RUNTIME_PROBE -ErrorAction SilentlyContinue
 Remove-Item Env:BC250_M14_CAPTURE_DUMP -ErrorAction SilentlyContinue
 if($gpu){$env:BC250_M14_CAPTURE_DUMP='1'}
 $env:DXVK_SHADER_CACHE='0';$env:MESA_SHADER_CACHE_DISABLE='true';$env:DXVK_LOG_PATH=$d
 $env:DXVK_LOG_LEVEL='info'
 # Functional debugger run only. Its timings cannot establish the performance bound.
 $imageDir="$d\$Phase-images"
 if(Test-Path $imageDir){throw 'Existing image directory'}
 $null=New-Item -ItemType Directory $imageDir
 $client=if($gpu){"$d\app\d3d11bench.exe"}else{"$d\d3d11bench.exe"}
 $args=@('25',$client,'--mode','offscreen','--adapter','1002:13fe','--size','64x64','--scenes','draws,fill,shaders','--draws','8','--layers','2','--shaders','4','--frames','3','--warmup','0','--deadline','20','--out',"$d\$Phase.json",'--dump',$imageDir)
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
 $expectedRoute=if($gpu){'app-local'}else{'system'}
 if($result.result -ne 'measured' -or $result.exit -ne 0 -or $result.d3d11 -ne $expectedRoute){throw 'Runtime result failed'}
 . "$d\scene-gate.ps1"
 $reference=if($gpu){'native-reference.json'}else{'cpu-reference.json'}
 Assert-M14Scenes $result (Get-Content "$d\$reference" -Raw|ConvertFrom-Json)
 $paths=@($result.modules|ForEach-Object {$_.path})
 if($gpu){
  foreach($name in @('d3d11.dll','dxgi.dll')){if("$d\app\$name" -notin $paths){throw "Missing app module $name"}}
  if(@($result.icds).Count -ne 1 -or $result.icds[0].path -ine $active -or $result.icds[0].sha256 -ine $candidate){throw 'Wrong GPU ICD'}
  if('C:\BC250\m11\resource-close\bc250d3d.dll' -in $paths){throw 'Native UMD in per-app route'}
 }else{
  foreach($name in @('d3d11.dll','dxgi.dll')){if("$env:windir\System32\$name" -notin $paths){throw 'System runtime not witnessed'}}
  if('C:\BC250\m11\resource-close\bc250d3d.dll' -notin $paths -or @($result.icds).Count){throw 'CPU path not witnessed'}
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
