# amdgpu-wddm tester uninstaller, v0. Run uninstall.cmd as administrator (from the package folder or from the
# install root). Removes everything install.ps1 added and puts the GPU back on Microsoft Basic Display Adapter.
# -DryRun prints every change without making it. -DisableTestSigning turns test signing off (asked otherwise when
# the installer turned it on). A restart finishes the removal of files that DWM still has loaded.
[CmdletBinding()]
param([switch]$DryRun, [switch]$Yes, [switch]$DisableTestSigning, [switch]$KeepTestSigning, [switch]$NoReboot)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
$script:DryRunMode = [bool]$DryRun

if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:windir 'sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path @PSBoundParameters
    exit $LASTEXITCODE
}
if (-not $DryRun -and -not (Test-IsAdmin)) { Invoke-SelfElevation -ScriptPath $MyInvocation.MyCommand.Path -Bound $PSBoundParameters }
if (-not $DryRun) { $script:LogPath = Join-Path $env:TEMP ('amdgpu-wddm-uninstall-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.log') }

Write-Host "amdgpu-wddm uninstaller$(if ($DryRun) { ' - DRY RUN, nothing will be changed' })" -ForegroundColor White
$state = Read-InstallState
$root = $null
if ($state -and $state.install_root) { $root = $state.install_root }
if (-not $root) { $root = (Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\Release" -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot }
if (-not $root) { $root = Join-Path $env:ProgramFiles 'amdgpu-wddm' }
Write-Info "install root: $root"
if (Test-LabInstallPresent) { Write-Fail 'a development-lab installation (C:\BC250\m1x) is present: this uninstaller does not change a lab machine'; exit 2 }
if (-not $state -and -not (Test-Path -LiteralPath $root) -and -not @(Get-OurDriverPackages).Count) { Write-Host 'Nothing to remove: no amdgpu-wddm installation found.' -ForegroundColor Green; exit 0 }
if (-not $Yes -and -not (Read-Confirmation -Question 'Remove the amdgpu-wddm driver and return the GPU to Microsoft Basic Display Adapter?' -Expect 'YES')) { Write-Host 'Stopped. Nothing was changed.'; exit 4 }

Write-Step 'Scheduled task, RunOnce entry, shortcut'
Invoke-Change "unregister the scheduled task '$($script:TaskName)'" {
    # Safe Mode runs no Task Scheduler service: then the task stays, and running uninstall.cmd again later removes it.
    try {
        $t = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
        if ($t) { if ($t.State -eq 'Running') { Stop-ScheduledTask -TaskName $script:TaskName }; Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false }
    } catch { Write-Warn2 "scheduled task not removed ($($_.Exception.Message)); run uninstall.cmd again after a normal start" }
} | Out-Null
Invoke-Change "remove the RunOnce entry '$($script:RunOnceName)'" { Remove-ItemProperty -LiteralPath $script:RunOnceKey -Name $script:RunOnceName -ErrorAction SilentlyContinue } | Out-Null
$lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\amdgpu-wddm Control.lnk'
Invoke-Change "remove the Start menu shortcut $lnk" { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue } | Out-Null

Write-Step 'Graphics registration and driver'
$dev = @(Get-Bc250Device)
if ($dev.Count -eq 1) {
    $key = Get-DeviceDriverKey -InstanceId $dev[0].DeviceID
    $svc = Get-DeviceServiceName -InstanceId $dev[0].DeviceID
    if ($key -and $svc -eq $script:ServiceName) {
        Invoke-Change "remove UserModeDriverName and VulkanDriverName from $key" {
            foreach ($n in 'UserModeDriverName', 'VulkanDriverName') { Remove-ItemProperty -LiteralPath $key -Name $n -ErrorAction SilentlyContinue }
        } | Out-Null
    }
}
$icdJson = Join-Path $root 'vulkan\radeon_icd.json'
Invoke-Change "remove '$icdJson' from $($script:KhronosKey)" { Remove-ItemProperty -LiteralPath $script:KhronosKey -Name $icdJson -ErrorAction SilentlyContinue } | Out-Null
Invoke-Change "remove $($script:SoftwareKey) (router policy, application profile, release record)" { Remove-Item -LiteralPath $script:SoftwareKey -Recurse -Force -ErrorAction SilentlyContinue } | Out-Null

$pkgs = @(Get-OurDriverPackages)
if (-not $pkgs.Count) { Write-Info 'no bc250kmd driver package in the driver store' }
foreach ($p in $pkgs) {
    Invoke-Change "pnputil /delete-driver $p /uninstall /force (the GPU falls back to Microsoft Basic Display Adapter)" {
        $n = Invoke-Native pnputil.exe @('/delete-driver', $p, '/uninstall', '/force')
        Write-Log $n.text
        if ($n.code -ne 0 -and $n.code -ne 3010) { Write-Warn2 "pnputil /delete-driver $p exit $($n.code): $($n.text)" }
    } | Out-Null
}
Invoke-Change 'pnputil /scan-devices (bind the GPU to its inbox driver now)' { [void](Invoke-Native pnputil.exe @('/scan-devices')) } | Out-Null
Invoke-Change 'remove the bc250kmd service entry (sc.exe delete; finished at the restart if the driver is still loaded)' {
    if (Get-Service -Name $script:ServiceName -ErrorAction SilentlyContinue) { [void](Invoke-Native sc.exe @('delete', $script:ServiceName)) }
} | Out-Null

Write-Step 'Files'
Invoke-Change "remove $root" { Remove-PathOrSchedule $root } | Out-Null
$stub = Join-Path $env:windir 'System32\bc250umd.dll'
$stubExisted = $false
if ($state -and $state.stub_existed) { $stubExisted = $true }
if (-not $stubExisted) { Invoke-Change "remove $stub" { Remove-PathOrSchedule $stub } | Out-Null }
else { Write-Info "$stub was there before the install: kept" }
foreach ($o in @(Get-ChildItem -LiteralPath (Split-Path $stub) -File -Filter 'bc250umd.dll.old-*' -ErrorAction SilentlyContinue)) {
    Invoke-Change "remove $($o.FullName) (old copy of a stub replaced while in use)" { Remove-PathOrSchedule $o.FullName } | Out-Null
}
$fwExisted = $false
if ($state -and $state.firmware_dir_existed) { $fwExisted = $true }
if (-not $fwExisted) {
    Invoke-Change "remove $($script:FirmwareInstallDir)" { Remove-PathOrSchedule $script:FirmwareInstallDir } | Out-Null
    $bcExisted = $false
    if ($state -and $state.bc250_dir_existed) { $bcExisted = $true }
    if (-not $bcExisted) {
        Invoke-Change 'remove C:\BC250 if it is empty' {
            if ((Test-Path -LiteralPath 'C:\BC250') -and -not @(Get-ChildItem -LiteralPath 'C:\BC250' -Force).Count) { Remove-Item -LiteralPath 'C:\BC250' -Force }
        } | Out-Null
    }
} else { Write-Info "$($script:FirmwareInstallDir) was there before the install: kept" }

Write-Step 'Certificate'
$thumb = $null
if ($state -and $state.cert_thumbprint) { $thumb = $state.cert_thumbprint }
if (-not $thumb) {
    $cer = Join-Path $package 'payload\cert\amdgpu-wddm-release.cer'
    if (Test-Path -LiteralPath $cer) { $thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer).Thumbprint }
}
if ($thumb) {
    Invoke-Change "remove certificate $thumb from LocalMachine Root and TrustedPublisher" {
        foreach ($store in 'Root', 'TrustedPublisher') { Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $thumb } | Remove-Item -Force }
    } | Out-Null
} else { Write-Warn2 'release certificate thumbprint unknown: certificate not removed' }

Write-Step 'Test signing'
$setByUs = ($state -and $state.testsigning_set_by_installer)
$off = [bool]$DisableTestSigning
if (-not $off -and -not $KeepTestSigning -and $setByUs) {
    $off = Read-Confirmation -Question 'The installer turned test signing on. Turn it off again?' -Expect 'YES'
}
if ($off) {
    Invoke-Change 'bcdedit /set {current} testsigning off' {
        $n = Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'off')
        if ($n.code -ne 0) { Write-Warn2 "bcdedit failed: $($n.text)" }
    } | Out-Null
    if ($state -and $state.bitlocker -eq 'Suspend') { Write-Info 'BitLocker: changing the boot options again; if BitLocker is on, have the recovery key ready or suspend it first.' }
} else { Write-Info 'test signing left as it is' }

# The control application's backups and action log (%ProgramData%\amdgpu-wddm\control) belong to the tester: kept.
$dataRoot = Split-Path $script:StateDir
$controlData = Join-Path $dataRoot 'control'
Invoke-Change "remove the installer state $($script:StateDir) and $dataRoot\start-confirm.log" {
    Remove-PathOrSchedule $script:StateDir
    Remove-Item -LiteralPath (Join-Path $dataRoot 'start-confirm.log') -Force -ErrorAction SilentlyContinue
    if ((Test-Path -LiteralPath $dataRoot) -and -not @(Get-ChildItem -LiteralPath $dataRoot -Force).Count) { Remove-Item -LiteralPath $dataRoot -Force }
} | Out-Null
if (Test-Path -LiteralPath $controlData) { Write-Info "kept: $controlData (the control application's setting backups and action log; delete it by hand if you do not need them)" }
if ($DryRun) { Write-Host ''; Write-Host 'Dry run complete: nothing was changed.' -ForegroundColor Green; exit 0 }
Write-Host ''
Write-Host "Uninstall complete. Restart the computer to finish. Log: $($script:LogPath)" -ForegroundColor Green
if (-not $NoReboot -and (Read-Confirmation -Question 'Restart now?' -Expect 'Y')) { Restart-Computer -Force }
exit 0
