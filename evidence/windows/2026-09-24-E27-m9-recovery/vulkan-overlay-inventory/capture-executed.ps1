param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
if(Test-Path $OutputDir){throw 'Capture directory already exists'}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$meta=[ordered]@{SchemaVersion=1;Status='error';CapturedUtc=[DateTime]::UtcNow.ToString('o');Error='Capture did not finish'}
try {
 if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
 $tool='C:\BC250\m8\vulkaninfo.exe'
 $icd='C:\BC250\m9\radv-main-icd2\radeon_icd.json'
 $library='C:\BC250\m9\radv-main-icd2\vulkan_radeon.dll'
 $meta.ToolSha256=(Get-FileHash $tool).Hash
 if($meta.ToolSha256 -ne '02A70101D8F9CBBCD4741FBBE5F7E18EC8799BE82DCC0A66DA3DBE5026C6EE00'){throw 'Unexpected vulkaninfo artifact'}
 $meta.Collector='vulkaninfo '+(Get-Item $tool).VersionInfo.FileVersion
 $meta.IcdPath=$icd;$meta.IcdSha256=(Get-FileHash $icd).Hash
 $meta.IcdLibraryPath=$library;$meta.IcdLibrarySha256=(Get-FileHash $library).Hash
 if($meta.IcdLibrarySha256 -ne 'DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986'){throw 'Unexpected ICD'}
 $gpus=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpus.Count -ne 1 -or $gpus[0].Status -ne 'OK'){throw 'Expected one healthy BC250'}
 $meta.KmdVersion=(Get-PnpDeviceProperty -InstanceId $gpus[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
 $meta.BootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
 $meta.Command='vulkaninfo --text --show-formats'
 $meta.ExitCode=$null;$meta.TimedOut=$false
 $env:VK_DRIVER_FILES=$icd;$env:VK_ICD_FILENAMES=$icd;$env:VK_LOADER_DEBUG='driver'
 $env:PATH='C:\BC250\m8;'+$env:PATH
 $env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
 $process=Start-Process -FilePath $tool -ArgumentList '--text','--show-formats' -WorkingDirectory $OutputDir -WindowStyle Hidden -PassThru -RedirectStandardOutput "$OutputDir\vulkan-inventory.txt" -RedirectStandardError "$OutputDir\vulkan-loader.txt"
 $meta.ProcessId=$process.Id
 $modules=@{}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while(-not $process.HasExited -and $timer.Elapsed.TotalSeconds -lt 40){
  try {foreach($m in $process.Modules){if($m.ModuleName -match 'vulkan|bc250'){$modules[$m.FileName]=1}}}catch{}
  Start-Sleep -Milliseconds 20
  $process.Refresh()
 }
 if(-not $process.HasExited){
  $meta.TimedOut=$true
  & taskkill.exe /PID $process.Id /T /F | Out-File "$OutputDir\timeout.txt"
  throw 'vulkaninfo timed out; no subsequent Vulkan workload permitted'
 }
 $process.WaitForExit();$meta.ExitCode=$process.ExitCode
 $meta.LoadedModules=@($modules.Keys | Sort-Object)
 $meta.IcdVerified=($modules.ContainsKey($library))
 $meta.ElapsedMs=$timer.ElapsedMilliseconds
 if($process.ExitCode -ne 0){throw ('vulkaninfo failed: '+$process.ExitCode)}
 $meta.Status='captured';$meta.Error='';$meta.CapturedUtc=[DateTime]::UtcNow.ToString('o')
} catch {$meta.Error=$_.Exception.Message}
$meta | ConvertTo-Json -Depth 5 | Set-Content "$OutputDir\capture.json" -Encoding UTF8
if($meta.Status -ne 'captured'){exit 1}
