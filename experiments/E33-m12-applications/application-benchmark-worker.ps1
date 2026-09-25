param(
 [Parameter(Mandatory=$true)][string]$Out,
 [Parameter(Mandatory=$true)][string]$Package,
 [Parameter(Mandatory=$true)][ValidateSet('Instancing9','Instancing10','AsteroidsVk','Asteroids11','Asteroids12')][string]$Kind,
 [Parameter(Mandatory=$true)][string]$VulkanIcd,
 [Parameter(Mandatory=$true)][string]$Cli,
 [ValidateRange(30,1800)][int]$TimeoutSeconds=300,
 [switch]$Capture
)
$ErrorActionPreference='Stop'
if([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0){throw 'Requires interactive lab session'}
if(Test-Path $Out){throw 'Result directory already exists'}
if(@(Get-ScheduledTask | Where-Object {$_.TaskName -like 'BC250-M12-cts*' -and $_.State -in @('Running','Queued')}).Count){throw 'CTS is active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
New-Item -ItemType Directory -Path $Out | Out-Null
function Save($name,$value){$value | ConvertTo-Json -Depth 8 | Set-Content "$Out\$name.json" -Encoding UTF8}
function Health($stage){
 foreach($mode in @('health','clock')){
  $path="$Out\$stage-$mode.txt"
  $p=Start-Process $Cli -ArgumentList $mode,'read' -WindowStyle Hidden -PassThru -RedirectStandardOutput $path
  $null=$p.Handle
  try{
   if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw 'Telemetry timeout'}
   if($p.ExitCode -ne 0){throw 'Telemetry failed'}
  }finally{$p.Dispose()}
  $text=[IO.File]::ReadAllText($path)
  if($mode -eq 'clock'){
   if($text -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
  }else{
   if($text -notmatch 'flags=15 generation=(\d+) epoch=(\d+)'){throw 'GPU health gate'}
   $id=$Matches[1]+':'+$Matches[2]
   if($script:GpuIdentity -and $script:GpuIdentity -ne $id){throw 'GPU identity changed'}
   $script:GpuIdentity=$id
  }
 }
}

$child=$null;$stdout=$null;$stderr=$null
try {
 $manifest=Get-Content (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
 $hashes=@{}
 foreach($f in $manifest.files){
  $path=Join-Path $Package $f.name
  $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  if($hash -ne $f.sha256){throw "Hash mismatch: $($f.name)"}
  $hashes[$f.name]=$hash
 }
 $required=@{}
 $icdHash='4D027149571DC000DA1E5006E6E393FCA6178DB32F1D9CB25D684E60849A5805'
 if((Get-FileHash $VulkanIcd).Hash -ne $icdHash){throw 'ICD mismatch'}
 $required[$VulkanIcd]=$icdHash
 $systemLoader=Join-Path $env:windir 'System32\vulkan-1.dll'
 $required[$systemLoader]=(Get-FileHash $systemLoader).Hash
 $csv=Join-Path $Out 'frames.csv'
 $exe='Asteroids.exe';$image=Join-Path $Out 'frame660.ppm'
 switch($Kind){
  'Instancing9' {$exe='Instancing.exe';$dlls=@('d3d9.dll');$api=9}
  'Instancing10' {$exe='Instancing10.exe';$dlls=@('d3d10core.dll','d3d11.dll','dxgi.dll');$api=10}
  'AsteroidsVk' {$dlls=@('GraphicsEngineVk_64r.dll');$mode='-vk'}
  'Asteroids11' {$dlls=@('GraphicsEngineD3D11_64r.dll','d3d11.dll','dxgi.dll');$mode='-d3d11'}
  'Asteroids12' {$dlls=@('GraphicsEngineD3D12_64r.dll','d3d12.dll','d3d12core.dll','dxgi.dll');$mode='-d3d12'}
 }
 if(-not $hashes.ContainsKey($exe)){throw 'Executable absent from manifest'}
 foreach($dll in $dlls){
  if(-not $hashes.ContainsKey($dll)){throw "DLL absent from manifest: $dll"}
  $required[(Join-Path $Package $dll)]=$hashes[$dll]
 }
 foreach($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','BC250_BENCHMARK_CSV','BC250_BENCHMARK_CAPTURE')){Remove-Item "Env:$name" -ErrorAction SilentlyContinue}
 $env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
 $env:RADV_EXPERIMENTAL='sparse'
 $env:PATH=($env:PATH.Split(';') | Where-Object {$_ -notlike 'C:\BC250\*'}) -join ';'
 $env:BC250_TRACE_SUBMITS='0';$env:DXVK_LOG_PATH=$Out;$env:DXVK_LOG_LEVEL='info'
 $env:VKD3D_LOG_FILE=Join-Path $Out 'vkd3d.log'
 if($Kind -like 'Instancing*'){
  $commandArguments="-forceapi:$api -windowed -width:1080 -height:720 -forcehal -forcevsync:0 -constantframetime:0.016666666666666667 -quitafterframe:660 -noerrormsgboxes -nostats"
  $env:BC250_BENCHMARK_CSV=$csv;$image=Join-Path $Out 'frame660.png'
  if($Capture){$env:BC250_BENCHMARK_CAPTURE=$image}
 }else{
  $commandArguments="$mode -window 1080 720 -threads 4 -benchmark_frames 660 -benchmark_output `"$csv`""
  if($Capture){$commandArguments+=" -benchmark_capture `"$image`""}
 }
 Save 'identity' @{kind=$Kind;capture=[bool]$Capture;required_modules=$required;command=$commandArguments;radv_experimental=$env:RADV_EXPERIMENTAL;package=$Package;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
 Health 'before'
 $psi=[Diagnostics.ProcessStartInfo]::new()
 $psi.FileName=Join-Path $Package $exe;$psi.Arguments=$commandArguments;$psi.WorkingDirectory=$Package
 $psi.UseShellExecute=$false;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true;$psi.CreateNoWindow=$true
 $child=[Diagnostics.Process]::Start($psi)
 $stdout=[IO.File]::Create("$Out\application.out");$stderr=[IO.File]::Create("$Out\application.err")
 $copyOut=$child.StandardOutput.BaseStream.CopyToAsync($stdout);$copyErr=$child.StandardError.BaseStream.CopyToAsync($stderr)
 $timer=[Diagnostics.Stopwatch]::StartNew();$healthTimer=[Diagnostics.Stopwatch]::StartNew();$healthIndex=0;$modules=@{}
 while(-not $child.HasExited -and $timer.Elapsed.TotalSeconds -lt $TimeoutSeconds){
  try{foreach($m in $child.Modules){if($required.ContainsKey($m.FileName)){$modules[$m.FileName]=1}}}catch{}
  if($healthTimer.Elapsed.TotalSeconds -ge 5){
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
   $healthIndex++;Health ('during-'+$healthIndex);$healthTimer.Restart()
  }
  Start-Sleep -Milliseconds 100;$child.Refresh()
 }
 if(-not $child.HasExited){throw 'Benchmark timeout'}
 if(-not $copyOut.Wait(2000) -or -not $copyErr.Wait(2000)){throw 'Output drain timeout'}
 $stdout.Dispose();$stdout=$null;$stderr.Dispose();$stderr=$null
 Save 'process' @{exit_code=$child.ExitCode;elapsed_ms=$timer.ElapsedMilliseconds;modules=@($modules.Keys)}
 if($child.ExitCode -ne 0){throw 'Application failed'}
 foreach($name in $required.Keys){if(-not $modules.ContainsKey($name)){throw "Missing module witness: $name"}}
 $rows=@(Import-Csv $csv)
 if($rows.Count -ne 660){throw "Unexpected frame count: $($rows.Count)"}
 $metric=if($Kind -like 'Instancing*'){'cpu_render_submit_ms'}else{'cpu_update_submit_present_ms'}
 for($i=0;$i -lt 660;$i++){
  if([int]$rows[$i].frame -ne $i+1){throw 'Frame sequence mismatch'}
  $v=[double]::Parse($rows[$i].$metric,[Globalization.CultureInfo]::InvariantCulture)
  if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -le 0){throw 'Invalid frame timing'}
 }
 $captureHash=$null
 if($Capture){
  if(-not(Test-Path $image) -or (Get-Item $image).Length -le 100){throw 'Capture missing or truncated'}
  $captureHash=(Get-FileHash $image).Hash
 }
 Health 'after'
 Save 'result' @{status='RUN_COMPLETED';frames=660;metric=$metric;csv_sha256=(Get-FileHash $csv).Hash;capture_sha256=$captureHash;image_correctness='not_evaluated';linux_parity='pending'}
}catch{
 Save 'result' @{status='FAIL';message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}
 throw
}finally{
 if($child -and -not $child.HasExited){Stop-Process -Id $child.Id -Force}
 if($stdout){$stdout.Dispose()};if($stderr){$stderr.Dispose()}
 if($child){$child.Dispose()}
}
