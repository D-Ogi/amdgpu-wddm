#Requires -Version 5.1
param(
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [string]$Adapter = 'BC-250',
 [ValidateSet('control','diagnostic')][string]$Icd = 'control',
 [ValidateSet('off','control8','pal8','exposed16')][string]$Mode = 'off',
 [switch]$HostNativeControl,
 [ValidateSet(8,16)][int]$NativeSamples = 8,
 [ValidateSet('native','paired','hostpaired')][string]$Program = 'native',
 [ValidateRange(0,5)][int]$Negative = 0
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
 throw 'Run non-elevated. The elevated Vulkan loader ignores per-process ICD manifest overrides.'
}
$kit = $PSScriptRoot
if ($Program -eq 'native' -and ($Mode -ne 'off' -or $Negative -ne 0)) { throw 'Native controls require off mode and no negative mutation' }
if ($Program -eq 'paired' -and ($Mode -ne 'exposed16' -or $NativeSamples -ne 8 -or $HostNativeControl)) { throw 'Paired mode requires hosted exposed16 at logical8' }
if ($Program -eq 'hostpaired' -and (!$HostNativeControl -or $NativeSamples -ne 16 -or $Mode -ne 'off')) { throw 'Host surrogate requires explicit native16 host mode' }
if ($NativeSamples -eq 16 -and !$HostNativeControl) { throw 'Native16 is a host reference only; hosted ICD remains logical8' }
if ($HostNativeControl -and $Mode -ne 'off') { throw 'Host native control only allows mode off' }
if ($Icd -eq 'control' -and $Mode -ne 'off') { throw 'Unpatched control only allows mode off' }
if ($Adapter.Contains('"')) { throw 'Adapter must not contain quotes' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $out) { throw 'Use a fresh output directory; evidence is immutable' }
$manifest = Get-Content -LiteralPath (Join-Path $kit 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest.files) {
 $file = Join-Path $kit $entry.path
 if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Hash mismatch: $file" }
}
New-Item -ItemType Directory -Path $out | Out-Null
$exe = Join-Path $out 's16f8-client.exe'
Copy-Item -LiteralPath (Join-Path $kit 's16f8-client.exe') -Destination $exe
$engine = Join-Path $kit 'amdgpu_wddm_vkd3d.dll'
$driver = Join-Path $kit "$Icd\amdgpu_wddm_radv.dll"
$engineHash = (Get-FileHash -LiteralPath $engine).Hash
$driverHash = (Get-FileHash -LiteralPath $driver).Hash
$prefix = Join-Path $out 'capture'
New-Item -ItemType Directory -Path (Join-Path $out 'temp') | Out-Null
$env:TEMP = Join-Path $out 'temp'
$env:TMP = $env:TEMP
$env:APPDATA = Join-Path $out 'profile'
$env:LOCALAPPDATA = Join-Path $out 'localappdata'
$env:BC250_S16F8_OUT = $prefix
$env:BC250_S16F8_PROGRAM = $Program
$env:BC250_S16F8_NEGATIVE = [string]$Negative
Remove-Item Env:BC250_S16F8_NATIVE16 -ErrorAction SilentlyContinue
if ($NativeSamples -eq 16) { $env:BC250_S16F8_NATIVE16 = '1' }
Remove-Item Env:VK_ADD_DRIVER_FILES -ErrorAction SilentlyContinue
$clearedEnvironment = @('AMDGPU_WDDM_LAB_DDI0102_LIMITS','radv_emulate_point_rounding',
 'VK_INSTANCE_LAYERS','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE',
 'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VKD3D_LOG_FILE',
 'VKD3D_FEATURE_LEVEL','VKD3D_SHADER_MODEL')
foreach ($name in $clearedEnvironment) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
if ($Mode -eq 'off') { Remove-Item Env:RADV_S16F8_PROBE_MODE -ErrorAction SilentlyContinue }
else { $env:RADV_S16F8_PROBE_MODE = $Mode }
$env:RADV_DEBUG = 'nocache'
$env:MESA_SHADER_CACHE_DISABLE = 'true'
$env:VKD3D_CONFIG = 'pipeline_library_app_cache'
$env:VKD3D_DEBUG = 'info'
$env:AMDGPU_WDDM_LOG = 'stderr'
$env:VK_LOADER_DEBUG = 'all'
if ($HostNativeControl) {
 Remove-Item Env:VK_DRIVER_FILES -ErrorAction SilentlyContinue
 Remove-Item Env:VK_ICD_FILENAMES -ErrorAction SilentlyContinue
} else {
 $icdManifest = Join-Path $out 'icd.json'
 @{file_format_version='1.0.0';ICD=@{library_path=$driver;api_version='1.4.0'}} |
  ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $icdManifest -Encoding ascii
 $env:VK_DRIVER_FILES = $icdManifest
 $env:VK_ICD_FILENAMES = $icdManifest
}
$stdout = "$prefix-stdout.txt"
$stderr = "$prefix-stderr.txt"
$start = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $exe -WorkingDirectory $out `
 -ArgumentList @('--engine',('"{0}"' -f $engine),'--adapter',('"{0}"' -f $Adapter)) `
 -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
# WinPS5.1 may otherwise lose ExitCode after refreshing an exited child.
$null = $process.Handle
$modules = @{}
$moduleErrors = @()
$timedOut = $false
while (!$process.HasExited) {
 try {
  $process.Refresh()
  foreach ($module in $process.Modules) {
   $path = $module.FileName
   if ($module.ModuleName -match '(?i)vkd3d|radv|vulkan|nvoglv|amdvlk' -and !$modules.ContainsKey($path)) {
    $modules[$path] = @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;observed_utc=[DateTime]::UtcNow.ToString('o')}
   }
  }
 } catch { if ($moduleErrors.Count -lt 4) { $moduleErrors += $_.Exception.Message } }
 if ($clock.Elapsed.TotalSeconds -ge 170) {
  $timedOut = $true
  if (!$process.HasExited) { $process.Kill() }
  break
 }
 $null = $process.WaitForExit(50)
}
$process.WaitForExit()
$exitCode = $process.ExitCode
$clock.Stop()
$seenEngine = @($modules.Values | Where-Object { $_.sha256 -eq $engineHash }).Count -gt 0
$seenDriver = @($modules.Values | Where-Object { $_.sha256 -eq $driverHash }).Count -gt 0
$markers = [regex]::Matches([IO.File]::ReadAllText($stderr),
 'RADV_S16F8_PROBE mode=(\d+) active=(\d+) coverage=(\d+) ps=(\d+) mask=([0-9a-fA-F]+)')
$stateVerified = $markers.Count -eq 0
$zeroMaskMarkerObserved = $false
if ($Mode -ne 'off') {
 $expected = switch ($Mode) { 'control8' { @(1,0,8,8) }; 'pal8' { @(2,1,16,8) }; 'exposed16' { @(3,1,16,8) } }
 $stateVerified = $markers.Count -gt 0
 $seenMasks = @{}
 foreach ($marker in $markers) {
  for ($index=0; $index -lt 4; $index++) {
   if ([int]$marker.Groups[$index+1].Value -ne $expected[$index]) { $stateVerified = $false }
  }
  $seenMasks[[Convert]::ToUInt32($marker.Groups[5].Value,16)] = $true
 }
 $masks = @(0,0xffff) + @(0..15 | ForEach-Object { 1 -shl $_ })
 for ($a=0; $a -lt 16; $a++) { for ($b=$a+1; $b -lt 16; $b++) { $masks += ((1 -shl $a) -bor (1 -shl $b)) } }
 # Every one of the138 masks, including zero, requires the diagnostic state.
 $zeroMaskMarkerObserved = $seenMasks.ContainsKey([uint32]0)
 if ($seenMasks.Count -ne 138) { $stateVerified = $false }
 foreach ($mask in $masks) { if (!$seenMasks.ContainsKey([uint32]$mask)) { $stateVerified = $false } }
}
$artifacts = @(Get-ChildItem -LiteralPath $out -File | ForEach-Object {
 @{path=$_.Name;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
$result = @{utc=$start.ToString('o');elapsed_seconds=$clock.Elapsed.TotalSeconds;timed_out=$timedOut;
 exit_code=$exitCode;adapter=$Adapter;mode=$Mode;icd=$Icd;host_native_control=[bool]$HostNativeControl;
 expected_engine_sha256=$engineHash;expected_icd_sha256=$driverHash;
 observed_engine=$seenEngine;observed_icd=$seenDriver;icd_verification_required=(!$HostNativeControl);
 program=$Program;negative=$Negative;native_samples=$NativeSamples;zero_mask_marker_observed=$zeroMaskMarkerObserved;
 kit_manifest_sha256=(Get-FileHash -LiteralPath (Join-Path $kit 'manifest.json')).Hash;
 runner_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
 cleared_process_environment=$clearedEnvironment;driver_state_verified=$stateVerified;driver_marker_count=$markers.Count;
 modules=@($modules.Values);module_errors=$moduleErrors;
 artifacts=$artifacts;scope='Stage B capture only. Native16 pixel-frequency host surrogate is not AMD scheduling proof. PRIVATE+THREADED, no cap change.'}
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath "$prefix-result.json" -Encoding utf8
[pscustomobject]$result | Select-Object elapsed_seconds,exit_code,timed_out,observed_engine,observed_icd,driver_state_verified | Format-List
if ($timedOut -or $null -eq $exitCode -or $exitCode -ne 0 -or !$seenEngine -or (!$HostNativeControl -and !$seenDriver) -or !$stateVerified) { exit 1 }
exit 0
