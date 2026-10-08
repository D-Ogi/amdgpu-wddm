param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
if(Test-Path $OutputDir){throw 'Capture directory already exists'}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$meta=[ordered]@{SchemaVersion=1;Status='error';CapturedUtc=[DateTime]::UtcNow.ToString('o');Error='Capture did not finish'}
try {
 if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
 # The pinned vulkaninfo.exe ships next to this script, never from an old lab directory.
 $tool=Join-Path $PSScriptRoot 'vulkaninfo.exe'
 # The ICD the adapter registers (display class key VulkanDriverName), not a pinned lab copy: the panel must
 # describe the driver that applications get. The library path comes from the manifest, next to it.
 $class=Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue |
  Where-Object {(Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).MatchingDeviceId -like 'pci\ven_1002&dev_13fe*'} | Select-Object -First 1
 if(-not $class){throw 'No BC250 display class key'}
 $icd=[string](Get-ItemProperty $class.PSPath).VulkanDriverName
 if(-not $icd -or -not (Test-Path $icd)){throw 'No registered Vulkan ICD manifest'}
 $libraryPath=[string](Get-Content $icd -Raw | ConvertFrom-Json).ICD.library_path
 $library=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($libraryPath)){$libraryPath}else{Join-Path (Split-Path $icd) $libraryPath}))
 if(-not (Test-Path $library)){throw 'Registered ICD library missing'}
 $meta.ToolSha256=(Get-FileHash $tool).Hash
 if($meta.ToolSha256 -ne '02A70101D8F9CBBCD4741FBBE5F7E18EC8799BE82DCC0A66DA3DBE5026C6EE00'){throw 'Unexpected vulkaninfo artifact'}
 $meta.Collector='vulkaninfo '+(Get-Item $tool).VersionInfo.FileVersion
 $meta.IcdPath=$icd;$meta.IcdSha256=(Get-FileHash $icd).Hash
 $meta.IcdLibraryPath=$library;$meta.IcdLibrarySha256=(Get-FileHash $library).Hash
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
 while(-not $process.HasExited -and $timer.Elapsed.TotalSeconds -lt 120){
  try {foreach($m in $process.Modules){if($m.ModuleName -match 'vulkan|bc250'){$modules[$m.FileName]=1}}}catch{}
  Start-Sleep -Milliseconds 20
  $process.Refresh()
 }
 $meta.LoadedModules=@($modules.Keys | Sort-Object)
 $meta.IcdVerified=($modules.ContainsKey($library))
 $meta.ElapsedMs=$timer.ElapsedMilliseconds
 if(-not $process.HasExited){
  $meta.TimedOut=$true
  & taskkill.exe /PID $process.Id /T /F | Out-File "$OutputDir\timeout.txt"
  throw 'vulkaninfo timed out; no subsequent Vulkan workload permitted'
 }
 $process.WaitForExit();$meta.ExitCode=$process.ExitCode
 if($process.ExitCode -ne 0){throw ('vulkaninfo failed: '+$process.ExitCode)}
 $meta.Status='captured';$meta.Error='';$meta.CapturedUtc=[DateTime]::UtcNow.ToString('o')
} catch {$meta.Error=$_.Exception.Message}
$meta | ConvertTo-Json -Depth 5 | Set-Content "$OutputDir\capture.json" -Encoding UTF8
if($meta.Status -ne 'captured'){exit 1}
