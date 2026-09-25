param(
 [ValidatePattern('^[a-z0-9-]+$')][string]$Run,
 [string]$Tools='C:\BC250\m12\system-icd\tools',
 [string]$SourceExe='C:\BC250\m10\cts-smoke-06\deqp-vk.exe',
 [ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedHash='35CBBC05F04C3B974B8D0638E102A854F78CB5E56B6E56FDA37D1BFEE452183F'
)
$ErrorActionPreference='Stop'
$out="C:\BC250\m12\$Run"
if(Test-Path "$out\start.json"){throw 'Existing run'}
if(@(Get-ScheduledTask 'BC250-M12-*' | Where-Object TaskName -ne 'BC250-M12-ShaderGalleryView' | Where-Object State -in @('Running','Queued')).Count){throw 'M12 task still active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$exe=$SourceExe
if((Get-FileHash $exe).Hash -ne $ExpectedHash){throw 'CTS binary mismatch'}
if([IO.Path]::GetFullPath($exe) -ne [IO.Path]::GetFullPath("$tools\deqp-vk.exe")){Copy-Item -LiteralPath $exe -Destination "$tools\deqp-vk.exe"}
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'

function ReadCli([string]$mode,[string]$path) {
 $si=[Diagnostics.ProcessStartInfo]::new()
 $si.FileName='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
 $si.Arguments="$mode read";$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true
 $proc=[Diagnostics.Process]::Start($si)
 try {
  $text=$proc.StandardOutput.ReadToEndAsync()
  if(-not $proc.WaitForExit(4000)){ $proc.Kill();throw "$mode timeout" }
  if($proc.ExitCode -ne 0 -or -not $text.Wait(1000)){throw "$mode failed"}
  [IO.File]::WriteAllText($path,$text.Result)
 } finally {$proc.Dispose()}
}
foreach($mode in @('clock','health')){ReadCli $mode "$out\preflight-$mode.txt"}
if((Get-Content "$out\preflight-health.txt" -Raw) -notmatch 'version=0x00070097'){throw 'KMD151 not active'}
if((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters').EnableNativePteCopy -ne 1){throw 'Native gate not enabled'}
$clock=Get-Content "$out\preflight-clock.txt" -Raw
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
if((Get-Content "$out\preflight-health.txt" -Raw) -notmatch 'flags=15 generation=\d+ epoch=\d+'){throw 'Health/generation gate'}
if((Get-FileHash 'C:\BC250\m12\mesa05-present-wait2\vulkan_radeon.dll').Hash -ne 'A6D11B64FEC1543C06760CE993A308C6288E8F52DA3DEE7BC9F3CD068A63C453'){throw 'ICD hash mismatch'}
@("$out\cts-worker.ps1","$out\cases.txt","$tools\deqp-vk.exe","$env:windir\System32\vulkan-1.dll",'C:\BC250\m12\mesa05-present-wait2\vulkan_radeon.dll') | ForEach-Object {Get-FileHash $_ | Select-Object Path,Hash} | ConvertTo-Json | Set-Content "$out\inputs.json"

function RestoreRegistration {
 $b=ConvertFrom-Json ([IO.File]::ReadAllText("$Out\registration-before.json"))
 New-ItemProperty -Path $b.class_path -Name VulkanDriverName -PropertyType MultiString -Value @($b.driver_names) -Force | Out-Null
 New-ItemProperty -Path $b.global_path -Name $b.old_manifest -PropertyType DWord -Value 0 -Force | Out-Null
 Remove-ItemProperty -Path $b.global_path -Name $b.candidate_manifest -ErrorAction Stop
 @{utc=[DateTime]::UtcNow.ToString('o');driver_names=@((Get-ItemProperty $b.class_path).VulkanDriverName);old_value=(Get-Item $b.global_path).GetValue($b.old_manifest);candidate_present=((Get-Item $b.global_path).GetValueNames() -contains $b.candidate_manifest)} | ConvertTo-Json | Set-Content "$Out\registration-restored.json"
}


$old='C:\BC250\m10\wsi-final\radeon_icd.json'
$new='C:\BC250\m12\mesa05-present-wait2\radeon_icd.json'
$global='HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
$gpu=@(Get-PnpDevice -PresentOnly -Class Display | Where-Object InstanceId -like 'PCI\VEN_1002&DEV_13FE*')
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Adapter status'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$names=@((Get-ItemProperty $class).VulkanDriverName)
if($names.Count -ne 1 -or $names[0] -ne $old){throw 'Unexpected adapter registration'}
$key=Get-Item $global
if($key.GetValueNames().Count -ne 1 -or $key.GetValue($old) -ne 0){throw 'Unexpected global registration'}
if(Test-Path "$out\registration-before.json"){throw 'Existing backup'}
if(-not (Test-Path $new)){throw 'Candidate manifest absent'}
$before=@{utc=[DateTime]::UtcNow.ToString('o');class_path=$class;driver_names=$names;global_path=$global;old_manifest=$old;candidate_manifest=$new;candidate_manifest_hash=(Get-FileHash $new).Hash}
$before | ConvertTo-Json -Depth 4 | Set-Content "$out\registration-before.json"
$Out=$out
try {
 New-ItemProperty -Path $global -Name $old -PropertyType DWord -Value 1 -Force | Out-Null
 New-ItemProperty -Path $global -Name $new -PropertyType DWord -Value 0 -Force | Out-Null
 New-ItemProperty -Path $class -Name VulkanDriverName -PropertyType MultiString -Value $new -Force | Out-Null
 @{utc=[DateTime]::UtcNow.ToString('o');manifest=$new;library_sha256=(Get-FileHash 'C:\BC250\m12\mesa05-present-wait2\vulkan_radeon.dll').Hash;scope='Temporary registration for elevated CTS. Worker restores prior baseline in finally.'} | ConvertTo-Json | Set-Content "$out\registration-during.json"

$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $out\cts-worker.ps1 -Out $out -Tools $tools"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName "BC250-M12-$Run" -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2)) | Out-Null
Start-ScheduledTask "BC250-M12-$Run"
'CTS_STARTED'

}catch{RestoreRegistration;throw}
