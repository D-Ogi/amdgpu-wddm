# amdgpu-wddm tester uninstaller, v0. Run uninstall.cmd as administrator (from the package folder or from the
# install root). Removes everything install.ps1 added and puts the GPU back on Microsoft Basic Display Adapter.
# -DryRun prints every change without making it. -DisableTestSigning turns test signing off (asked otherwise when
# the installer turned it on). A restart finishes the removal of files that DWM still has loaded.
# -Force removes the release also from a development-lab machine (as install.ps1 -Force installs on one); the lab's
# own folders (C:\BC250\m10, m14, m15) are not ours to remove and stay.
[CmdletBinding()]
param([switch]$DryRun, [switch]$Yes, [switch]$DisableTestSigning, [switch]$KeepTestSigning, [switch]$NoReboot, [switch]$Force)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
. (Join-Path $here 'mft-h264.ps1')
$script:DryRunMode = [bool]$DryRun
# Host tests only: a dry run can read its installer state from a test folder (as install.ps1).
if ($DryRun -and $env:AMDGPU_WDDM_TEST_STATE_DIR) {
    $script:StateDir = $env:AMDGPU_WDDM_TEST_STATE_DIR
    $script:StatePath = Join-Path $script:StateDir 'state.json'
}

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
$lab = @(Get-LabInstallPaths)
if ($lab.Count -and -not $Force) { Write-Fail "a development-lab installation ($($lab -join ', ')) is present: this uninstaller does not change a lab machine (-Force removes the release anyway)"; exit 2 }
if ($lab.Count) { Write-Warn2 "a development-lab installation ($($lab -join ', ')) is present; -Force given: the release is removed, those folders stay" }
if (-not $state -and -not (Test-Path -LiteralPath $root) -and -not @(Get-OurDriverPackages).Count) { Write-Host 'Nothing to remove: no amdgpu-wddm installation found.' -ForegroundColor Green; exit 0 }
if (-not $Yes -and -not (Read-Confirmation -Question 'Remove the amdgpu-wddm driver and return the GPU to Microsoft Basic Display Adapter?' -Expect 'YES')) { Write-Host 'Stopped. Nothing was changed.'; exit 4 }

Write-Step 'Scheduled task, RunOnce entry, shortcut'
# Every task of ours, not only the one this release registers: a task that an older release named differently would
# otherwise stay and run a script that is gone.
$tasks = @(Get-OurScheduledTasks)
Invoke-Change "unregister our scheduled task(s): $(if ($tasks.Count) { $tasks -join ', ' } else { "none on this computer (this release registers '$($script:TaskName)')" })" {
    # Safe Mode runs no Task Scheduler service: then the task stays, and running uninstall.cmd again later removes it.
    foreach ($name in $tasks) {
        try {
            $t = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            if ($t) { if ($t.State -eq 'Running') { Stop-ScheduledTask -TaskName $name }; Unregister-ScheduledTask -TaskName $name -Confirm:$false }
        } catch { Write-Warn2 "scheduled task '$name' not removed ($($_.Exception.Message)); run uninstall.cmd again after a normal start" }
    }
} | Out-Null
Invoke-Change "remove the RunOnce entry '$($script:RunOnceName)'" { Remove-ItemProperty -LiteralPath $script:RunOnceKey -Name $script:RunOnceName -ErrorAction SilentlyContinue } | Out-Null
foreach ($name in 'amdgpu-wddm Control.lnk', 'amdgpu-wddm Control (recovery).lnk') {
    $lnk = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\$name"
    Invoke-Change "remove the Start menu shortcut $lnk" { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue } | Out-Null
}

Write-Step 'Graphics registration and driver'
$dev = @(Get-Bc250Device)
if ($dev.Count -eq 1) {
    $key = Get-DeviceDriverKey -InstanceId $dev[0].DeviceID
    $svc = Get-DeviceServiceName -InstanceId $dev[0].DeviceID
    if ($key -and $svc -eq $script:ServiceName) {
        Invoke-Change "remove UserModeDriverName, UserModeDriverNameWow, VulkanDriverName and VulkanDriverNameWow from $key" {
            foreach ($n in 'UserModeDriverName', 'UserModeDriverNameWow', 'VulkanDriverName', 'VulkanDriverNameWow') { Remove-ItemProperty -LiteralPath $key -Name $n -ErrorAction SilentlyContinue }
        } | Out-Null
    }
}
$icdJson = Join-Path $root 'vulkan\radeon_icd.json'
Invoke-Change "remove '$icdJson' from $($script:KhronosKey)" { Remove-ItemProperty -LiteralPath $script:KhronosKey -Name $icdJson -ErrorAction SilentlyContinue } | Out-Null
$icdJsonWow = Join-Path $root 'wow64\vulkan\radeon_icd.json'
Invoke-Change "remove '$icdJsonWow' from $($script:KhronosKeyWow)" { Remove-ItemProperty -LiteralPath $script:KhronosKeyWow -Name $icdJsonWow -ErrorAction SilentlyContinue } | Out-Null
# An entry of an older release of ours, whose install root was another folder, would keep the Vulkan loader looking
# for a file that is gone. Only a name that carries our release name and ends with radeon_icd.json is ours.
$icdLeft = @(Get-OurVulkanIcdValues | Where-Object { $_.name -ne $icdJson -and $_.name -ne $icdJsonWow })
if ($icdLeft.Count) {
    Invoke-Change "remove the Vulkan entries of older releases of ours: $(@($icdLeft | ForEach-Object { "$($_.key) '$($_.name)'" }) -join ', ')" {
        foreach ($v in $icdLeft) { Remove-ItemProperty -LiteralPath $v.key -Name $v.name -ErrorAction SilentlyContinue }
    } | Out-Null
}
# The policy keys: the 64-bit view that everything of ours writes (the router opens it with KEY_WOW64_64KEY), the
# 32-bit view in case an older release wrote one, and the control application's own preferences of the account that
# runs this uninstaller. Another account keeps its preferences, which are window state only.
$softwareKeyWow = "HKLM:\SOFTWARE\WOW6432Node\$($script:ReleaseName)"
$userKey = "HKCU:\Software\$($script:ReleaseName)"
Invoke-Change "remove $($script:SoftwareKey) (router policy, application profile, release record), $softwareKeyWow and $userKey" {
    foreach ($k in @($script:SoftwareKey, $softwareKeyWow, $userKey)) { Remove-Item -LiteralPath $k -Recurse -Force -ErrorAction SilentlyContinue }
} | Out-Null
# The H.264 encoder MFT (driver/umd/mft-h264/INSTALL.md): the class id key with its InprocServer32, the transform key
# and our membership in the video encoder category. The keys go before the files below, so that no COM registration
# is left pointing at a DLL that is gone. The category key itself stays while another encoder of the machine is in it,
# and MediaFoundation\Transforms is Windows' own. A release that never registered the encoder has nothing here.
$mftKeys = @(Get-MftRegistrationKeysPresent -ClassesKey $script:ClassesKey)
Invoke-Change "remove the H.264 encoder MFT registration ($(if ($mftKeys.Count) { $mftKeys -join ', ' } else { 'no key of ours present' }))" {
    [void](Remove-MftRegistration -ClassesKey $script:ClassesKey)
    # The computer answers, not the return value: a key the access rights kept is named here, because the files below
    # go whatever happens and a key that stays would name a DLL that is gone.
    $left = @(Get-MftRegistrationKeysPresent -ClassesKey $script:ClassesKey)
    if ($left.Count) { Write-Warn2 "the H.264 encoder MFT registration is still on this computer: $($left -join ', '). Remove these keys by hand (regedit, as an administrator): the file they name goes with the install root below." }
} | Out-Null

# The GPU leaves the driver now, under the running desktop: pnputil has no documented way to defer the removal of a
# driver from a started device to the next restart, and a package kept until then could leave the GPU without a
# driver at that start. The restart that ends the uninstall gives a fresh session (BD-060, common.ps1).
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
$serviceKey = Split-Path $script:ParametersKey
Invoke-Change "remove the bc250kmd service entry and its settings in $serviceKey (sc.exe delete; finished at the restart if the driver is still loaded)" {
    if (Get-Service -Name $script:ServiceName -ErrorAction SilentlyContinue) { [void](Invoke-Native sc.exe @('delete', $script:ServiceName)) }
    # The settings of the driver live under this key (Parameters: the gates, the DPM values, CuMode, the tuning the
    # control application wrote). sc.exe delete takes the whole key, but a key without the values that make it a
    # service is not a service to sc.exe, and a value left there would become the "tester's own setting" that the next
    # install keeps (Get-RegistryDefaultPlan). So the key goes here whatever sc.exe did.
    if (Test-Path -LiteralPath $serviceKey) {
        Remove-Item -LiteralPath $serviceKey -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $serviceKey) { Write-Warn2 "$serviceKey is still there: Windows removes it at the restart that ends this uninstall. Run uninstall.cmd again after the restart if a new install reports settings of this one." }
    }
} | Out-Null

Write-Step 'Files'
Invoke-Change "remove $root" { Remove-PathOrSchedule $root } | Out-Null
# The D3D9 stub in System32 and its x86 copy in SysWOW64 (BD-064), each kept when it was there before the install.
foreach ($s in @(@{ dir = 'System32'; flag = 'stub_existed' }, @{ dir = 'SysWOW64'; flag = 'stub_wow_existed' })) {
    $stub = Join-Path $env:windir "$($s.dir)\bc250umd.dll"
    $stubExisted = $false
    if ($state -and $state.($s.flag)) { $stubExisted = $true }
    if (-not $stubExisted) { Invoke-Change "remove $stub" { Remove-PathOrSchedule $stub } | Out-Null }
    else { Write-Info "$stub was there before the install: kept" }
    foreach ($o in @(Get-ChildItem -LiteralPath (Split-Path $stub) -File -Filter 'bc250umd.dll.old-*' -ErrorAction SilentlyContinue)) {
        Invoke-Change "remove $($o.FullName) (old copy of a stub replaced while in use)" { Remove-PathOrSchedule $o.FullName } | Out-Null
    }
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
} else { Write-Warn2 'release certificate thumbprint unknown: the certificate of this release is removed by its subject below' }
# Every test certificate of ours, by subject: the one of this release when its thumbprint was not known, and the one of
# every older release that another install left in the stores. Only a certificate whose subject carries our release
# name is ours, and we sign nothing else with it.
$certsLeft = @(Get-OurCertificates | Where-Object { $_.thumbprint -ne $thumb })
if ($certsLeft.Count) {
    Invoke-Change "remove $($certsLeft.Count) more test certificate(s) of ours: $(@($certsLeft | ForEach-Object { "$($_.store) $($_.thumbprint)" }) -join ', ')" {
        foreach ($c in $certsLeft) { Get-ChildItem "Cert:\LocalMachine\$($c.store)" | Where-Object { $_.Thumbprint -eq $c.thumbprint } | Remove-Item -Force -ErrorAction SilentlyContinue }
    } | Out-Null
} else { Write-Info 'no other test certificate of ours in Root or TrustedPublisher' }

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

Write-Step 'Installer state'
# The control application's backups and action log (%ProgramData%\amdgpu-wddm\control) belong to the tester: kept.
$dataRoot = Split-Path $script:StateDir
$controlData = Join-Path $dataRoot 'control'
# The installer state includes the kept repair sets (installer\packages) and the running-release witness: without it
# the control application names no running release until a new install verifies one.
Invoke-Change "remove the installer state $($script:StateDir) (with the kept repair sets and the running-release witness), $dataRoot\start-confirm.log and $dataRoot\dwm-baseline.json" {
    Remove-PathOrSchedule $script:StateDir
    Remove-Item -LiteralPath (Join-Path $dataRoot 'start-confirm.log') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $dataRoot 'dwm-baseline.json') -Force -ErrorAction SilentlyContinue
    if ((Test-Path -LiteralPath $dataRoot) -and -not @(Get-ChildItem -LiteralPath $dataRoot -Force).Count) { Remove-Item -LiteralPath $dataRoot -Force }
} | Out-Null
if (Test-Path -LiteralPath $controlData) { Write-Info "kept: $controlData (the control application's setting backups and action log; delete it by hand if you do not need them)" }

# What of this release is still on the computer. The tester's clean-slate check, and the one place a host test or a
# support report reads it from. Reads only, and a probe that cannot read says so instead of claiming the item is gone.
Write-Step 'What is left of this release'
$footprint = @(Get-ReleaseFootprint -InstallRoot $root -State $state -MftKeys @(Get-MftRegistrationKeysPresent -ClassesKey $script:ClassesKey))
foreach ($f in $footprint) {
    $mark = 'gone'
    if ($f.present -and $f.kept) { $mark = 'kept' } elseif ($f.present) { $mark = 'LEFT' }
    Write-Host ('   [{0,-4}] {1,-24} {2}' -f $mark, $f.item, $f.detail) -ForegroundColor $(if ($mark -eq 'LEFT') { 'Yellow' } else { 'Gray' })
    Write-Log ('   footprint {0}: {1} {2}' -f $f.item, $mark, $f.detail)
}
$left = @($footprint | Where-Object { $_.present -and -not $_.kept })
if ($DryRun) {
    Write-Host ''
    Write-Host "Dry run complete: nothing was changed, so the list above is this computer as it is now ($($left.Count) item(s) of this release)." -ForegroundColor Green
    exit 0
}
if ($left.Count) { Write-Warn2 "$($left.Count) item(s) above are marked LEFT. A file or a registry key that Windows still holds goes at the restart below; run uninstall.cmd again afterwards if one of them stays." }
else { Write-Info 'nothing of this release is left, apart from the items marked kept' }
Write-Host ''
Write-Host "Uninstall complete. Restart the computer to finish. Log: $($script:LogPath)" -ForegroundColor Green
Write-Host 'Until the restart, some Windows 11 apps (the command bar of File Explorer, Task Manager) can ignore mouse clicks: the GPU changed to Microsoft Basic Display Adapter under the running desktop.' -ForegroundColor Yellow
# A normal, planned restart (common.ps1, Request-PlannedRestart): programs are asked to close and may keep unsaved work.
if (-not $NoReboot -and (Read-Confirmation -Question 'Restart now?' -Expect 'Y')) {
    $why = Request-PlannedRestart
    if ($why) { Write-Warn2 "Windows did not start the restart ($why): restart the computer yourself" }
}
exit 0
