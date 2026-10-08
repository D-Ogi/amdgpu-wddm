# Hand recovery of an attempt whose candidate crashed the machine (the boot changed, so the attempt's own phases refuse
# with "Boot/host changed"): the same steps as its restore arm, run once by hand, elevated, with the attempt's own
# helpers, identity and capture. Disable, install the rollback package deferred (registered INF found by its pinned
# hash), write the captured graphics registration and parameters back, reset UnconfirmedStarts (the crashed candidate
# used the guard's budget, and the rollback's start would be refused), the captured desktop switches, enable, and
# wait for a started device. Hang detector, health confirm and the task follow as in the recovery-required steps.
#   target.py ps ops\recover-after-boot.ps1 -Attempt kmd188-deploy001
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}(-(?!1-)[1-9][0-9]*)?-deploy[0-9]{3}$')][string]$Attempt)
$ErrorActionPreference = 'Stop'
$dir = "C:\BC250\m15\$Attempt"; $t = "$dir\kmd-transition"
. "$t\identity.ps1"; . "$t\durable.ps1"; . "$t\registration.ps1"; . "$t\verify-cpu.ps1"; . "$t\parameters.ps1"; . "$t\hang-detector.ps1"
$saved = Get-Content "$dir\baseline.json" -Raw | ConvertFrom-Json
$gpu = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13FE' } | Select-Object -First 1
if (!$gpu) { throw 'GPU device not found' }
$id = $gpu.InstanceId
function Prop($k) { (Get-PnpDeviceProperty -InstanceId $id -KeyName $k).Data }
"utc $([DateTime]::UtcNow.ToString('o')) before: version $(Prop DEVPKEY_Device_DriverVersion) problem $(Prop DEVPKEY_Device_ProblemCode)"
if (Get-ScheduledTask -TaskName $KmdTaskName -ErrorAction SilentlyContinue | Where-Object State -eq 'Running') { throw 'Attempt task still Running' }
$inf = Get-ChildItem C:\Windows\INF\oem*.inf | Where-Object { (Get-FileHash $_.FullName).Hash -eq $KmdRollbackInfSha256 } | Select-Object -First 1
if (!$inf) { throw 'Rollback INF not registered' }
"rollback INF $($inf.Name) $KmdRollbackVersion"

if ((Prop DEVPKEY_Device_ProblemCode) -ne 22) { & pnputil.exe /disable-device $id | Out-Host; if ($LASTEXITCODE -ne 0) { throw 'Disable failed' } }
if ((Prop DEVPKEY_Device_ProblemCode) -ne 22) { throw 'Disable not observed' }
& "$dir\select-driver.exe" --install-deferred $id $inf.FullName $KmdRollbackVersion | Out-Host
"select-driver exit $LASTEXITCODE"
if ((Prop DEVPKEY_Device_DriverVersion) -ne $KmdRollbackVersion) { throw 'Installed version mismatch' }
if ((Prop DEVPKEY_Device_ProblemCode) -ne 22) { throw 'Unexpected automatic enable' }
$image = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath -replace '^\\SystemRoot', $env:windir
if ((Get-FileHash $image).Hash -ne $KmdRollbackSysSha256) { throw "Service image is not the rollback SYS: $image" }
"service image $image"

$classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(('SYSTEM\CurrentControlSet\Control\Class\' + (Prop DEVPKEY_Device_Driver)), $true)
try { $r = Set-KmdGraphicsRegistration -Key $classKey -Saved $saved.graphics_registration; "registration removed: $($r.removed -join ',')" } finally { $classKey.Dispose() }
$reg = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$skip = @('UnconfirmedStarts', 'LastStage', 'StageHistory') + @(Get-KmdHangDetectorNames) + @(Get-KmdInteropWrittenNames)
foreach ($item in $saved.parameters.PSObject.Properties) { if ($item.Name -in $skip) { continue }
  New-ItemProperty $reg -Name $item.Name -Value $item.Value.value -PropertyType $item.Value.kind -Force | Out-Null }
New-ItemProperty $reg -Name UnconfirmedStarts -Value 0 -PropertyType DWord -Force | Out-Null
$switches = (Get-KmdSavedDesktop $saved).switches
Set-DurablePresentGates $switches
"parameters written, UnconfirmedStarts 0, switches $switches"

& pnputil.exe /enable-device $id | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Enable failed' }
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 60 -and (Prop DEVPKEY_Device_ProblemCode) -ne 0) { Start-Sleep -Milliseconds 500 }
"after $([int]$sw.Elapsed.TotalSeconds) s: version $(Prop DEVPKEY_Device_DriverVersion) problem $(Prop DEVPKEY_Device_ProblemCode)"
$cli = Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
& $cli health read
