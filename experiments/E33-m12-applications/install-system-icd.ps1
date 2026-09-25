param([string]$Root='C:\BC250\m12\system-icd')
$ErrorActionPreference='Stop'
$icd='C:\BC250\m10\wsi-final\radeon_icd.json'
$library='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$loader='C:\BC250\m8\vulkan-1.dll'
$global='HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
$old='C:\BC250\m8\radeon_icd.json'
if(Test-Path "$Root\registration-before.json"){throw 'Existing deployment record'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
if(@(Get-ScheduledTask 'BC250-M11-*' | Where-Object State -in @('Running','Queued')).Count){throw 'M11 still active'}
if((Get-FileHash $library).Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'ICD hash'}
if((Get-FileHash $icd).Hash -ne '234175BDF7A764CCEA3827A4C833495E67CAD7AF643CAC25FCC43D0A82203FCE'){throw 'Manifest hash'}
if((Get-FileHash $loader).Hash -ne '5C42CA8EA4BC43EE17FFE8FCDAFBA36E377C2AF29722CB09F400C24D9D80FB73'){throw 'Loader hash'}
$gpu=@(Get-PnpDevice -PresentOnly -Class Display | Where-Object InstanceId -like 'PCI\VEN_1002&DEV_13FE*')
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Adapter identity/status'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$systemLoader=Join-Path $env:windir 'System32\vulkan-1.dll'
if(Test-Path $systemLoader){throw 'System loader now exists; inspect before replacing'}
New-Item -ItemType Directory "$Root\tools" -Force | Out-Null
$before=@{utc=[DateTime]::UtcNow.ToString('o');class_path=$class;driver_names=@((Get-ItemProperty $class).VulkanDriverName);system_loader_existed=$false;global_path=$global;global_values=@()}
if(Test-Path $global){$key=Get-Item $global;$before.global_values=@($key.GetValueNames() | ForEach-Object {@{name=$_;value=$key.GetValue($_);kind=$key.GetValueKind($_).ToString()}})}
$before | ConvertTo-Json -Depth 5 | Set-Content "$Root\registration-before.json"
Copy-Item -LiteralPath $loader -Destination $systemLoader
if((Get-FileHash $systemLoader).Hash -ne (Get-FileHash $loader).Hash){throw 'Loader copy mismatch'}
Copy-Item C:\BC250\m8\vulkaninfo.exe,C:\BC250\m8\vkcompute.exe,C:\BC250\m10\wsi-final\vkcube.exe -Destination "$Root\tools"
if(-not (Test-Path $global)){New-Item -Path $global -Force | Out-Null}
New-ItemProperty -Path $global -Name $icd -PropertyType DWord -Value 0 -Force | Out-Null
if((Get-Item $global).GetValueNames() -contains $old){New-ItemProperty -Path $global -Name $old -PropertyType DWord -Value 1 -Force | Out-Null}
New-ItemProperty -Path $class -Name VulkanDriverName -PropertyType MultiString -Value $icd -Force | Out-Null
@{utc=[DateTime]::UtcNow.ToString('o');icd=$icd;icd_sha256=(Get-FileHash $library).Hash;loader=$systemLoader;loader_sha256=(Get-FileHash $systemLoader).Hash;driver_names=@((Get-ItemProperty $class).VulkanDriverName);old_registration_disabled=((Get-Item $global).GetValue($old) -eq 1)} | ConvertTo-Json | Set-Content "$Root\registration-after.json"
'REGISTERED_X64_PENDING_RUNTIME_VERIFICATION'
