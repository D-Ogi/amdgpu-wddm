# amdgpu-wddm tester installer, v0. Run install.cmd as administrator from the unpacked package folder.
#
# Three phases, resumed automatically from %ProgramData%\amdgpu-wddm\installer\state.json:
#   1. preflight, System Restore point, test signing on, then an automatic restart. Skipped when test signing is
#      already active in the running boot.
#   2. certificate, user-mode drivers, firmware, driver-set staging; automatic restart to bind the new KMD.
#      Resume, check the binding, write registry and tasks; automatic configuration restart. No live GPU/DWM restart.
#   3. verify: driver bound, version, start health, D3D12 device at feature level 12_1, Vulkan enumerates the GPU.
# -DryRun runs every check and prints every change without making it. -Verify runs phase 3 only.
# Network: the package does not contain the AMD GPU firmware. Phase 2 downloads it from linux-firmware at the commit
# pinned in manifest.json (kernel.org, GitLab mirror as fallback) and checks each SHA256, or takes it from a local
# folder (-FirmwareDir), checked the same way. A prepared offline folder or a kept repair set carries the files in its
# own firmware\ folder, which is used when -FirmwareDir is not given. Nothing else uses the network.
#
# The setup window (tools\win\amdgpu_wddm_setup) drives this script with -Gui (docs/gui/interfaces-setup.md): the
# engine writes events (-EventsFile) and one terminal result bound to -InvocationId (-ResultFile), decides the action,
# asks the user nothing, never elevates itself and never restarts Windows. -Plan runs the checks and
# reports the decision, the restarts and the settings-impact plan, and changes nothing. Before the
# first restart it asks for, the whole package (and an offline firmware folder) is staged and checked in
# %ProgramData%\amdgpu-wddm\installer\packages\<version>, and RunOnce runs the continuation from there; when phase 2
# completes, that copy stays as the repair set. On the command line a restart starts by itself after a visible 15 s
# countdown (any key puts it off) and is a normal, planned restart (never a forced one).
#
# Nothing in this engine asks the user for a permission: running it IS the consent for the test
# mode the driver needs and for the BitLocker pause that the boot-option change needs (Get-BitLockerPlan). The
# switches that used to carry those answers, -AcceptTestSigning and -BitLocker, are still accepted so that every
# script that passes them keeps running. A preflight check that fails still refuses the install: a machine this
# driver must not change is not a question.
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Plan,                          # checks and the decision only; changes nothing (a dry run that stops early)
    [switch]$Gui,                           # driven by the setup window: no prompt, no elevation, no restart
    [string]$InvocationId,                  # the setup window's id of this run, echoed in every event and the result
    [string]$EventsFile,                    # events, one JSON object per line (appended)
    [string]$ResultFile,                    # the terminal result (written once, at the end)
    [string]$DeadlineUtc,                   # ISO 8601 UTC: stop at the first stop point after this time (interfaces-setup.md 10)
    [switch]$HoldWindow,                    # the RunOnce continuation: keep the console open at the end (no install.cmd pause)
    [switch]$Verify,
    [switch]$Force,                         # install even over a development-lab installation (not supported)
    [switch]$NoReboot,                      # never restart; tell the tester to do it
    [switch]$AcceptTestSigning,             # accepted and not needed any more: no test-signing question is asked
    # Accepted and not needed any more: BitLocker protection is suspended by itself for the restarts of the install.
    # 'HaveKey' still means "do not touch protection, the recovery key is at hand" (Get-BitLockerPlan).
    [ValidateSet('', 'HaveKey', 'Suspend')][string]$BitLocker = '',
    [ValidateRange(1000, 2000)][int]$DpmMaxMHz,  # absent = the default (registry-defaults.json) or the tester's own value
    [ValidateSet(0, 24, 40)][int]$CuMode = 0,  # 0 = not given: the release default of registry-defaults.json
    [string]$InstallRoot = (Join-Path $env:ProgramFiles 'amdgpu-wddm'),
    [switch]$NoControlApp,
    # The shortcuts of the control application. Both places are written by default. The two No
    # switches leave one out, and the two positive ones state the default explicitly, so a later run can put back a
    # shortcut that an earlier one left out (the state keeps the first answer, as it does for the Windows tuning).
    [switch]$NoStartMenuShortcut,
    [switch]$StartMenuShortcut,
    [switch]$NoDesktopShortcut,
    [switch]$DesktopShortcut,
    # The Nagi wallpaper, recorded for Nagi Control App, which sets it once for each person after
    # the install is verified. On by default; -NoWallpaper leaves it out and -Wallpaper states the default, as the
    # shortcut pairs do.
    [switch]$NoWallpaper,
    [switch]$Wallpaper,
    # The picture on the start screen, drawn by the driver itself. On by default; -NoBootLogo
    # writes Parameters\BootLogo = 0 and -BootLogo removes that value, which is the release default. Neither switch
    # keeps what the installed driver already has, so a reinstall or an upgrade keeps an earlier choice.
    [switch]$NoBootLogo,
    [switch]$BootLogo,
    [switch]$ApplySystemTuning,              # optional reversible Windows tuning; absent = keep Windows settings
    [switch]$SkipSystemTuning,               # explicitly clear an earlier opt-in when resuming
    [switch]$Repair,                        # install the same, already verified version again
    [string]$RollbackPackage,               # explicit schema 2 -> supported legacy package, current engine owns every phase
    [string]$FirmwareDir,                   # offline: the 9 firmware files (INSTALL.md, "GPU firmware") instead of a download
    [string[]]$Components,                  # unattended: exactly these optional components (ids of components.json)
    [switch]$NoComponents,                  # unattended: no optional component, and no question about one
    [string[]]$Models,                      # unattended: exactly these models (ids of components.json)
    [switch]$NoModels,                      # unattended: no model, and no question about one
    [string]$ComponentDir,                  # offline: the component and model files from that folder instead of a download
    [switch]$DryRunIgnoreBoard             # host test only, honoured with -DryRun: walk all phases on a PC without a BC-250
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
. (Join-Path $here 'deferred-driver.ps1')
. (Join-Path $here 'dwm-session.ps1')
. (Join-Path $here 'engine.ps1')
. (Join-Path $here 'release-witness.ps1')
# The readers of the KMD's start and stop records, and the automatic diagnostics of a start that did not pass.
. (Join-Path $here 'start-confirm-core.ps1')
. (Join-Path $here 'diagnostics.ps1')
. (Join-Path $here 'compatibility.ps1')
. (Join-Path $here 'mft-h264.ps1')
. (Join-Path $here 'system-tuning.ps1')
. (Join-Path $here 'mesa-temp-cleanup.ps1')
. (Join-Path $here 'components.ps1')
. (Join-Path $here 'state-lifecycle.ps1')
. (Join-Path $here 'state-reader.ps1')
. (Join-Path $here 'legacy-rollback.ps1')
. (Join-Path $here 'app-profiles.ps1')
$enginePackage=$package
$engineHere=$here
$legacyRollback=[bool]$RollbackPackage
if($legacyRollback) {
    $RollbackPackage=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RollbackPackage)
    $PSBoundParameters['RollbackPackage']=$RollbackPackage
    $package=$RollbackPackage
    $here=Join-Path $package 'installer'
    if($Verify -or $Repair -or $PSBoundParameters.ContainsKey('DpmMaxMHz') -or $CuMode) {
        throw 'rollback preserves the captured settings; tuning and repair are separate operations'
    }
}
if ($Plan) { $DryRun = [Management.Automation.SwitchParameter]$true }
$script:DryRunMode = [bool]$DryRun
# Host tests only: a dry run can read its installer state from a test folder (an upgrade over a given state).
if ($DryRun -and $env:AMDGPU_WDDM_TEST_STATE_DIR) {
    $script:StateDir = $env:AMDGPU_WDDM_TEST_STATE_DIR
    $script:StatePath = Join-Path $script:StateDir 'state.json'
}
$script:InPhase2 = $false
# Any step that throws ends here: the step, the reason, and the re-run hint. Phase 2 is idempotent, so a re-run with
# the same package skips what is in place and finishes.
trap {
    Write-StepFailure $_
    if ($script:InPhase2 -and $state -and -not $script:DryRunMode) { try { Save-Phase 'install-incomplete' } catch { } }
    Exit-Engine -Code 6 -Outcome 'failed' -MessageId 'result.step-failed' -Detail ([string]$_.Exception.Message) -Step $script:CurrentStep
}

# ---- 64-bit, elevated ------------------------------------------------------------------------------------------
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $ps = Join-Path $env:windir 'sysnative\WindowsPowerShell\v1.0\powershell.exe'
    & $ps -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path @PSBoundParameters
    exit $LASTEXITCODE
}
Initialize-Engine -Gui ([bool]$Gui) -InvocationId $InvocationId -EventsFile $EventsFile -ResultFile $ResultFile -DeadlineUtc $DeadlineUtc -Mode $(if ($Plan) { 'plan' } elseif ($Verify) { 'verify' } elseif ($DryRun) { 'dry-run' } else { 'run' })
# A relative -FirmwareDir means the folder the tester started from; the elevated copy starts in System32.
if ($FirmwareDir) {
    $FirmwareDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($FirmwareDir)
    $PSBoundParameters['FirmwareDir'] = $FirmwareDir
}
if ($ComponentDir) {
    $ComponentDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ComponentDir)
    $PSBoundParameters['ComponentDir'] = $ComponentDir
}
$script:HoldWindow = [bool]$HoldWindow
# A setup-window run that may change the system needs its kill-on-close job (interfaces-setup.md section 10): without
# it nothing bounds the processes it starts, so it refuses before anything else. A plan, a dry run and verify change
# nothing that needs it.
if ($script:GuiMode -and -not $DryRun -and -not $Verify -and $script:EngineJob -ne 'kill-on-close') {
    Write-EngineEvent 'start' ([ordered]@{ mode = $script:EngineMode; gui = $true; dry_run = $false; package = $package; contract = $script:EngineContract; phase = $null; deadline_utc = $script:EngineDeadlineText; job = $script:EngineJob })
    Write-Host "No job object for the installer's child processes ($($script:EngineJob)). Nothing was changed." -ForegroundColor Red
    Exit-Engine -Code 2 -Outcome 'refused' -MessageId 'result.preflight-error' -Detail "the engine cannot contain its child processes ($($script:EngineJob)); a changing run refuses before any change"
}
if (-not $DryRun -and -not (Test-IsAdmin)) {
    # The setup window runs elevated and starts the engine from there; it never hands a run to another window.
    if ($script:GuiMode) { Exit-Engine -Code 2 -Outcome 'refused' -MessageId 'result.needs-admin' -Detail 'the engine runs without administrator rights' }
    Invoke-SelfElevation -ScriptPath $MyInvocation.MyCommand.Path -Bound $PSBoundParameters
}
if (-not $DryRun) {
    [void][IO.Directory]::CreateDirectory($script:StateDir)
    Set-StateDirAccess
    $script:LogPath = Join-Path $script:StateDir ('install-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.log')
    if (-not (Enter-EngineLock)) {
        Write-Host 'Another run of the installer is changing this computer now. Wait until it ends, then run install.cmd again.' -ForegroundColor Yellow
        Exit-Engine -Code 9 -Outcome 'refused' -MessageId 'result.busy' -Detail "$($script:StateDir)\engine.lock is held by another process"
    }
}

Write-Host "amdgpu-wddm tester installer$(if ($Plan) { ' - PLAN, nothing will be changed' } elseif ($DryRun) { ' - DRY RUN, nothing will be changed' })" -ForegroundColor White
Write-Info "package: $package"
$early = Read-InstallState
if ($early -and $early.install_root) { $InstallRoot = $early.install_root }
Write-Info "install root: $InstallRoot"
$script:EnginePhaseBefore = $(if ($early) { [string]$early.phase } else { $null })
Write-EngineEvent 'start' ([ordered]@{ mode = $script:EngineMode; gui = [bool]$Gui; dry_run = [bool]$DryRun; package = $package; contract = $script:EngineContract; phase = $script:EnginePhaseBefore; deadline_utc = $script:EngineDeadlineText; job = $script:EngineJob })
Start-EngineTestChild
# A stage of the run, for the log, the console and the setup window's progress list. In a run that changes nothing
# (plan, dry run, verify) a stage boundary is a stop point while Cancel is offered, so a dry run stops where a real
# install would and nowhere it said Cancel cannot work; in a real install the deadline is only announced here and acted
# on at the next stop point.
function Enter-Stage([string]$Id, [string]$Text) {
    Invoke-StageStop "stage:$Id" ($script:DryRunMode -or $script:EngineMode -eq 'verify')
    Write-Step $Text
    Write-EngineEvent 'stage' ([ordered]@{ id = $Id; text = $Text })
}
$script:OnChange = { param($d) Write-EngineEvent 'step' ([ordered]@{ description = $d; dry_run = [bool]$script:DryRunMode }) }

# ---- preflight -----------------------------------------------------------------------------------------------
# Every check runs, in a dry run too. Severity: fail stops the install, warn needs attention, ok is fine.
function Invoke-Preflight {
    $checks = New-Object System.Collections.ArrayList
    # Each check has a stable id for the setup window (docs/gui/interfaces-setup.md): <area>.<finding>. The detail is
    # technical and goes to the console and the log only.
    function Add-Check([string]$Name, [string]$Severity, [string]$Detail, [string]$Id) { [void]$checks.Add([pscustomobject]@{ check = $Name; result = $Severity; detail = $Detail; id = $Id }) }

    $integrity = Test-PackageManifest -PackageRoot $package
    if ($integrity.ok) { Add-Check 'package integrity' 'ok' $integrity.detail 'package.ok' } else { Add-Check 'package integrity' 'fail' $integrity.detail 'package.damaged' }
    $script:Manifest = $integrity.manifest

    if (Test-IsAdmin) { Add-Check 'administrator' 'ok' 'elevated' 'admin.ok' }
    elseif ($DryRun) { Add-Check 'administrator' 'warn' 'not elevated: Secure Boot, BitLocker and boot options read as unknown in this dry run' 'admin.dry-run' }
    else { Add-Check 'administrator' 'fail' 'not elevated' 'admin.missing' }

    $os = Get-CimInstance Win32_OperatingSystem
    # The Windows support policy (n-2; an older Windows 11 gets a line, not a refusal).
    # Get-WindowsSupportFindings in common.ps1 decides; the ids below are the whole set it can return.
    $win = Get-WindowsSupportState -Os $os
    foreach ($f in @($win.findings)) {
        switch ($f.id) {
            'windows.not-x64' { Add-Check 'Windows' 'fail' $f.detail 'windows.not-x64' }
            'windows.too-old' { Add-Check 'Windows' 'fail' $f.detail 'windows.too-old' }
            'windows.below-supported' { Add-Check 'Windows' 'warn' $f.detail 'windows.below-supported' }
            'windows.insider' { Add-Check 'Windows channel' 'warn' $f.detail 'windows.insider' }
            'windows.ltsc' { Add-Check 'Windows edition' 'warn' $f.detail 'windows.ltsc' }
            # An id this list does not know keeps its own severity: a finding that refuses the install must never
            # read as a passed check because the list above was not extended with it.
            default {
                if ([string]$f.severity -eq 'fail') { Add-Check 'Windows' 'fail' "$($f.detail) ($($f.id))" $f.id }
                elseif ([string]$f.severity -eq 'warn') { Add-Check 'Windows' 'warn' "$($f.detail) ($($f.id))" $f.id }
                else { Add-Check 'Windows' 'ok' $f.detail 'windows.ok' }
            }
        }
    }

    $dev = @(Get-Bc250Device)
    $board = (Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue)
    $boardText = if ($board) { "$($board.Manufacturer) $($board.Product)".Trim() } else { 'unknown board' }
    if ($dev.Count -eq 0 -and $DryRun -and $DryRunIgnoreBoard) { Add-Check 'BC-250 GPU' 'warn' "no device $($script:HardwareIdPrefix) ($boardText); -DryRunIgnoreBoard: the dry run continues to show every phase" 'gpu.ignored' }
    elseif ($dev.Count -eq 0) { Add-Check 'BC-250 GPU' 'fail' "no device $($script:HardwareIdPrefix) on this computer ($boardText). This package is for the ASRock BC-250 only." 'gpu.missing' }
    elseif ($dev.Count -gt 1) { Add-Check 'BC-250 GPU' 'fail' "$($dev.Count) matching devices; exactly one is supported" 'gpu.several' }
    else {
        $script:Device = $dev[0]
        $svc = Get-DeviceServiceName -InstanceId $dev[0].DeviceID
        $script:DeviceService = $svc
        Add-Check 'BC-250 GPU' 'ok' "found ($boardText), current driver service: $(if ($svc) { $svc } else { 'none' })" 'gpu.ok'
    }

    # The detail names the source that decided; a firmware without Secure Boot reads "off (not built into the firmware)".
    $sbRead = Read-SecureBootState
    $sb = $sbRead.state
    $script:SecureBoot = $sb
    if ($sb -eq 'on') { Add-Check 'Secure Boot' 'fail' "$($sbRead.detail): test signing cannot be enabled. Turn Secure Boot off in the BIOS setup first (see INSTALL.md, section Secure Boot)." 'secureboot.on' }
    elseif ($sb -eq 'unknown') { Add-Check 'Secure Boot' 'warn' "state unknown: $($sbRead.detail)" 'secureboot.unknown' }
    else { Add-Check 'Secure Boot' 'ok' $sbRead.detail 'secureboot.off' }

    $tsActive = Get-TestSigningActive
    $tsNext = Get-TestSigningConfigured
    $script:TestSigningActive = $tsActive
    $script:TestSigningConfigured = $tsNext
    Add-Check 'test signing' 'ok' ("active in this boot: $tsActive; explicit setting on this boot entry: $(if ($null -eq $tsNext) { 'unknown' } else { $tsNext })") $(if ($tsActive) { 'testsigning.active' } else { 'testsigning.inactive' })

    $bl = Get-BitLockerState
    $script:BitLockerState = $bl
    if ($bl -eq 'on') { Add-Check 'BitLocker' 'warn' "protection on for $($env:SystemDrive): the installer suspends it for the restarts of this installation before it changes the boot options, so Windows does not ask for the recovery key (Get-BitLockerPlan)" 'bitlocker.on' }
    elseif ($bl -eq 'unknown') { Add-Check 'BitLocker' 'warn' 'state unknown: the installation goes on; keep the recovery key at hand in case Windows asks for it after a restart' 'bitlocker.unknown' }
    else { Add-Check 'BitLocker' 'ok' $bl 'bitlocker.ok' }

    $hvci = Get-MemoryIntegrityState
    if ($hvci -eq 'on') { Add-Check 'Memory integrity' 'warn' 'on: if the driver does not start, turn off Core isolation > Memory integrity (see INSTALL.md)' 'hvci.on' }
    else { Add-Check 'Memory integrity' 'ok' $hvci 'hvci.off' }

    # Smart App Control on: Windows blocks the test-signed setup exe and our user-mode driver DLLs in every process,
    # DWM included (Code Integrity 3033/3077, measured on 26100), so the run stops here, before its first change (the
    # class of BD-192). Evaluation can turn into on by itself: a warning that asks to turn it off now.
    $sac = Read-SmartAppControlState
    $sacSteps = 'open Windows Security > App & browser control > Smart App Control settings, select Off, then "Yes, I''m sure", then Yes in the administrator prompt (see INSTALL.md, section Smart App Control and SmartScreen)'
    if ($sac.state -eq 'on') { Add-Check 'Smart App Control' 'fail' "$($sac.detail): while it is on, Windows blocks the test-signed setup program and the programs of the graphics driver, so the desktop and games cannot use the driver. Turn Smart App Control off first: $sacSteps." 'smartappcontrol.on' }
    elseif ($sac.state -eq 'evaluation') { Add-Check 'Smart App Control' 'warn' "$($sac.detail): if Windows turns Smart App Control on later, Windows blocks the programs of the graphics driver, and the desktop and games lose the driver. Turn it off now: $sacSteps." 'smartappcontrol.evaluation' }
    elseif ($sac.state -eq 'unknown') { Add-Check 'Smart App Control' 'warn' "state unknown: $($sac.detail). If Windows blocks the setup or the driver, $sacSteps." 'smartappcontrol.unknown' }
    else { Add-Check 'Smart App Control' 'ok' $sac.detail 'smartappcontrol.off' }

    # The Microsoft Visual C++ runtime our programs need. A PC without it is no longer refused: the package carries Microsoft's own installer and phase 2 runs it. The only
    # refusal left is a package that does not carry that installer, or carries a changed copy of it.
    $vc = Get-VcRuntimeState
    $vcExe = Join-Path $package $script:VcRuntimePackagePath
    if (-not $vc.install) { Add-Check 'Visual C++ runtime' 'ok' $vc.detail 'vcruntime.ok' }
    else {
        $vcFile = Test-VcRuntimeInstaller -Path $vcExe
        if ($vcFile.ok) { Add-Check 'Visual C++ runtime' 'warn' "$($vc.detail); the installer installs $($script:VcRuntimeVersion) from $($script:VcRuntimePackagePath) (SHA256 $($vcFile.sha256.Substring(0, 16)), signature $($vcFile.status), $($vcFile.signer))" 'vcruntime.missing' }
        else { Add-Check 'Visual C++ runtime' 'fail' "$($vc.detail); $($vcFile.problem)" 'vcruntime.no-installer' }
    }

    $drive = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($env:SystemDrive)'"
    $freeGb = [math]::Round($drive.FreeSpace / 1GB, 1)
    if ($freeGb -lt 2) { Add-Check 'free space' 'fail' "$freeGb GB free on $($env:SystemDrive); 2 GB needed" 'space.low' } else { Add-Check 'free space' 'ok' "$freeGb GB free on $($env:SystemDrive)" 'space.ok' }

    $fw = if ($script:Manifest) { $script:Manifest.firmware } else { $null }
    if (-not $fw) { Add-Check 'GPU firmware' 'fail' 'manifest.json has no firmware list' 'firmware.no-list' }
    elseif ($FirmwareDir) {
        $bad = Test-FirmwareFolder $fw $FirmwareDir
        if ($bad.Count) { Add-Check 'GPU firmware' 'fail' "-FirmwareDir $FirmwareDir`: $($bad -join '; '). Get the files from the addresses in INSTALL.md, section 'GPU firmware'." 'firmware.folder-bad' }
        else { Add-Check 'GPU firmware' 'ok' "$(@($fw.files).Count) files in $FirmwareDir match the pinned SHA256" 'firmware.folder-ok' }
    } else {
        $h = Test-FirmwareHosts -Firmware $fw
        if ($h.ok.Count) { Add-Check 'GPU firmware' 'ok' ("download from linux-firmware $($fw.commit): $($h.ok -join ', ') reachable" + $(if ($h.bad.Count) { "; not reachable: $($h.bad -join '; ')" } else { '' })) 'firmware.download-ok' }
        else { Add-Check 'GPU firmware' 'fail' "no download host answers ($($h.bad -join '; ')). The installer downloads the GPU firmware: connect this computer to the internet, or download the 9 files on another computer (INSTALL.md, section 'GPU firmware') and run install.cmd -FirmwareDir <folder>." 'firmware.unreachable' }
    }

    $lab = @(Get-LabInstallPaths)
    if ($lab.Count) {
        if ($Force) { Add-Check 'existing installation' 'warn' "a development-lab installation ($($lab -join ', ')) is present; -Force given" 'lab-install.forced' }
        else { Add-Check 'existing installation' 'fail' "a development-lab installation ($($lab -join ', ')) is present. The release installer does not change a lab machine." 'lab-install.present' }
    }
    $st = Read-InstallState
    $script:state = $st
    if ($st) { Add-Check 'installer state' 'ok' "phase $($st.phase) since $($st.updated_utc)" 'state.ok' }
    elseif ($script:DeviceService -eq $script:ServiceName -and -not $Force) { Add-Check 'existing installation' 'fail' 'bc250kmd is already installed without installer state; run uninstall.cmd first' 'existing.no-state' }

    # BD-227: an install that stages a driver set needs Windows to move the GPU to it at the restart. -Force does not
    # pass this check: the state handoff would leave the GPU on a KMD that does not start.
    $rank = Test-DriverSetRank -InstallState $st
    if ($rank) { Add-Check 'driver package rank' $(if ($rank.ok) { 'ok' } else { 'fail' }) $rank.detail "driver-rank.$($rank.id)" }

    return $checks
}

# BD-227: the rank decision (deferred-driver.ps1) for a run that stages this package's driver set, else $null: no
# device, a rollback (it removes the other driver ranks itself while the device stays disabled), an action that does
# not install, or a driver set that is staged already (the binding check after the restart owns it).
function Test-DriverSetRank($InstallState) {
    if ($legacyRollback -or -not $script:Device -or $action.action -notin @('install', 'upgrade', 'repair', 'resume')) { return $null }
    $inf = Join-Path $package 'payload\kmd\bc250kmd.inf'
    if (-not (Test-Path -LiteralPath $inf -PathType Leaf)) { return $null }
    $sha = Get-Sha256 $inf
    $receipt = $null
    if ($InstallState -and $InstallState.PSObject.Properties['driver_set']) { $receipt = $InstallState.driver_set }
    if ((Get-DriverSetAction $receipt $sha ([string](Get-BootIdentity).boot_id)) -ne 'stage') { return $null }
    return (Get-DriverSetRankDecision -StagedInfText ([IO.File]::ReadAllText($inf)) -StagedInfSha $sha `
        -Bound (Read-DriverSetBinding -InstanceId $script:Device.DeviceID -ServiceName $script:ServiceName))
}

# RunOnce runs the continuation at the next logon (GUI plan A1): the installer of the staged closure ('continue') or
# the installed verify.cmd ('verify'); a run of the setup window continues in the closure's setup window. The program
# and each argument are quoted on their own (Format-CommandLine).
$script:Closure = $null
$script:Continuation = $null
function Set-ResumeAtLogon([ValidateSet('continue', 'verify')][string]$Kind) {
    $c = Get-ContinuationCommand -Kind $Kind -Closure $script:Closure -InstallRoot $InstallRoot -Gui ([bool]$script:GuiMode)
    if($legacyRollback -and $Kind -eq 'continue') {
        if(-not $script:BridgeClosure){throw 'rollback continuation has no retained current engine'}
        $c=[pscustomobject]@{exe=(Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe');arguments=@(
            '-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $script:BridgeClosure 'installer\install.ps1'),
            '-RollbackPackage',$script:Closure,'-HoldWindow')}
    }
    $command = Format-CommandLine $c.exe $c.arguments
    $script:Continuation = [ordered]@{ kind = $Kind; exe = $c.exe; arguments = @($c.arguments); command = $command; closure = $script:Closure }
    Invoke-Change "RunOnce entry '$($script:RunOnceName)' -> $command (runs at the next logon)" {
        Initialize-RegistryKey $script:RunOnceKey
        Set-ItemProperty -LiteralPath $script:RunOnceKey -Name $script:RunOnceName -Value $command
    } | Out-Null
}
# The whole package, and the firmware folder of an offline install, staged and checked in the closure before the first
# restart this install asks for (common.ps1, Save-ContinuationClosure). Once per run; a run that starts from its closure
# only checks it. From then on an offline install reads its firmware from the closure, also after the restart.
function Initialize-Closure {
    if ($script:Closure) { return }
    if($legacyRollback) {
        $engineIntegrity=Test-PackageManifest -PackageRoot $enginePackage
        if(-not $engineIntegrity.ok){throw "rollback engine package: $($engineIntegrity.detail)"}
        $script:BridgeClosure=Get-ClosureDir $engineIntegrity.manifest.version
        if(Test-SamePath $script:BridgeClosure (Get-ClosureDir $packageVersion)){throw 'rollback target must differ from current engine package'}
        Invoke-Change 'retain the complete current engine and SYSTEM worker for the rollback continuation' {
            [void](Save-ContinuationClosure -PackageRoot $enginePackage -Manifest $engineIntegrity.manifest -Destination $script:BridgeClosure)
        } | Out-Null
    }
    $script:Closure = Get-ClosureDir $packageVersion
    if (Test-SamePath $package $script:Closure) { Write-Info "this run starts from its continuation closure $($script:Closure)" }
    Invoke-Change "stage the continuation closure: every file of this package and $(if ($FirmwareDir) { "the firmware folder $FirmwareDir" } else { 'no firmware folder (downloaded in phase 2)' }) into $($script:Closure), each SHA256 checked there" {
        [void](Save-ContinuationClosure -PackageRoot $package -Manifest $script:Manifest -FirmwareDir $FirmwareDir -Destination $script:Closure)
    } | Out-Null
    if ($FirmwareDir -and -not $script:DryRunMode) {
        $script:FirmwareDir = Join-Path $script:Closure 'firmware'
        $installInputs.firmware_dir = $script:FirmwareDir
        Set-StateValue $state 'firmware_source_dir' $script:FirmwareDir
        Save-InstallState $state
    }
    Invoke-CancelPoint 'after-staging'
}

# What this run does with what is installed (Get-InstallAction): the package's version against the installed one.
$packageVersion = $null
try { $packageVersion = [string](Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json).version } catch { }
$installedVersion = $null
if ($early -and $early.package_version) { $installedVersion = [string]$early.package_version }
if (-not $installedVersion -and -not ($DryRun -and $env:AMDGPU_WDDM_TEST_STATE_DIR)) {
    $installedVersion = (Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\Release" -Name Version -ErrorAction SilentlyContinue).Version
}
$action = Get-InstallAction -State $early -PackageVersion $packageVersion -InstalledVersion $installedVersion -Repair $Repair
if($legacyRollback) {
    $action=[ordered]@{action=$(if($early.legacy_rollback -and $early.package_version -eq $packageVersion){'resume'}else{'upgrade'});
        message='Explicit legacy rollback: device stays disabled until the current state has been restored'}
}
if ($Verify) { $action = [ordered]@{ action = 'verify'; message = $null } }
if ($early) { Write-Info "installed: $(if ($installedVersion) { $installedVersion } else { 'unknown version' }), phase $($early.phase); package: $packageVersion; action: $($action.action)" }
$script:EngineAction = $action.action
$script:EnginePackageVersion = $packageVersion
# A boundary advances only in a new boot (GUI plan A2, common.ps1 Get-PendingRestart), for the run that continues
# after the test-signing or the driver package's restart and for every verification: a pending phase (test signing set, driver package
# waiting, installation complete) needs positive evidence that Windows restarted since it was saved. A same or unknown
# boot keeps the pending phase, changes nothing and ends with exit 7. The continuation after the test-signing restart
# is a run of the same package at testsigning-pending; another package at that phase takes the installation over
# (Set-InstallPackage below) and stops for the same restart itself.
$continuesTestSigning = ($action.action -eq 'install') -and $early -and ([string]$early.phase -eq 'testsigning-pending') -and ([string]$early.package_version -eq $packageVersion)
if ($early -and ($action.action -eq 'resume' -or $continuesTestSigning -or ($action.action -eq 'verify' -and -not ($DryRun -and -not $Verify)))) {
    $pendingPhase = [string]$early.phase
    $why = Get-PendingRestart $early (Get-BootIdentity)
    if ($why) {
        if ($why -eq 'saved-unknown' -and -not $script:DryRunMode -and $null -ne (Get-BootIdentity).boot_id) {
            # A state saved by an engine that did not record the boot: this boot becomes the boundary, so the next start
            # passes it. One restart more at worst, never a skipped one.
            Set-StateValue $early 'restart_boot_id' (Get-BootIdentity).boot_id
            Save-InstallState $early
        }
        $reason = switch ($pendingPhase) { 'testsigning-pending' { 'restart.test-signing' } 'driver-pending-restart' { 'restart.driver-package' } default { 'restart.complete' } }
        $text = switch ($why) { 'same-boot' { 'this is still the boot in which it was asked for' } 'saved-unknown' { 'the state does not say in which boot it was asked for' } default { 'the current boot cannot be identified' } }
        Write-Host "The restart that the installation waits for ($pendingPhase) has not been confirmed: $text. Restart the computer; the installer continues after you log on again." -ForegroundColor Yellow
        $script:EngineRestart = [ordered]@{ required = $true; reason_id = $reason; still_pending = $true; continuation = $null }
        Exit-Engine -Code 7 -Outcome 'restart-required' -MessageId $(if ($pendingPhase -eq 'installed') { 'result.verify-before-restart' } else { 'result.restart-still-pending' }) -Detail "phase $pendingPhase, restart not confirmed: $why"
    }
    # Verify checks a finished installation only: after the restart, an unfinished one continues with install, never
    # by a verification that would overwrite its phase.
    if ($action.action -eq 'verify' -and $pendingPhase -in @('new', 'testsigning-pending', 'testsigning-active', 'files-copied', 'driver-pending-restart', 'driver-installed', 'install-incomplete')) {
        Write-Host "The installation is not finished (phase $pendingPhase): run install.cmd to finish it, then verify." -ForegroundColor Yellow
        Exit-Engine -Code 2 -Outcome 'refused' -MessageId 'result.install-unfinished' -Detail "verify of an unfinished installation (phase $pendingPhase)"
    }
}
# The run after a restart that the installer asked for starts from RunOnce without arguments: it takes the first
# run's -FirmwareDir, driver settings and switches from the state (common.ps1, Get-InstallInputs). Driver settings
# given on the command line are written even over a tester's own value.
$installInputs = Get-InstallInputs $PSBoundParameters $early $packageVersion
if (@($installInputs.restored).Count) { Write-Info "from the first run of this install ($($early.phase)): $($installInputs.restored -join ' ')" }
# A prepared offline folder or a kept repair set carries the firmware in its own firmware\ folder.
if (-not $installInputs.firmware_dir) {
    $own = Get-PackageFirmwareDir $package
    if ($own) { $installInputs.firmware_dir = $own; Write-Info "firmware: the folder of this package, $own (no download)" }
}
$FirmwareDir = $installInputs.firmware_dir
# The optional components (components.ps1): this release's pinned list, and a prepared offline folder or a repair set
# that carries the files already.
$script:ComponentsManifest = $null
$script:ComponentProblems = @()
try {
    $script:ComponentsManifest = Read-ComponentsManifest $here
    if ($null -ne $script:ComponentsManifest) {
        $script:ComponentProblems = @(Test-ComponentsManifest $script:ComponentsManifest)
        if ($script:ComponentProblems.Count) { $script:ComponentsManifest = $null }
    }
} catch { $script:ComponentProblems = @("installer\components.json does not parse: $($_.Exception.Message)") }
$ComponentDir = $installInputs.component_dir
if (-not $ComponentDir) {
    $own = Get-PackageComponentDir $package
    if ($own) { $installInputs.component_dir = $own; $ComponentDir = $own; Write-Info "components: the folder of this package, $own (no download)" }
} else {
    # -ComponentDir may name a prepared folder, whose zips are in its own components\ subfolder, or a folder that
    # holds the zips themselves.
    $ComponentDir = Get-ComponentSourceDir $ComponentDir
    Write-Info "components: the folder $ComponentDir (no download for a file that is there)"
}
# The model files are not in the folder the component zips come from: a prepared folder keeps components\ and
# models\ side by side (prepare-offline.ps1). A folder given by hand may hold both together. Both spellings work.
$ModelDir = $null
if ($ComponentDir) { $ModelDir = Get-ModelSourceDir $ComponentDir }
if (-not $ModelDir) { $ModelDir = Get-PackageModelDir $package }
if ($ModelDir -and $ModelDir -ne $ComponentDir) { Write-Info "models: the folder $ModelDir (no download for a model that is there)" }
$commandLineParameters = $installInputs.parameters
$freshCommandLineParameters = @($PSBoundParameters.Keys | Where-Object {
    $_ -eq 'DpmMaxMHz' -or ($_ -eq 'CuMode' -and [int]$PSBoundParameters['CuMode'] -ne 0)
})
$freshRepair = $PSBoundParameters.ContainsKey('Repair') -and [bool]$PSBoundParameters['Repair']
$NoControlApp = [switch](@($installInputs.switches) -contains 'NoControlApp')
$NoReboot = [switch](@($installInputs.switches) -contains 'NoReboot')
$Force = [switch](@($installInputs.switches) -contains 'Force')
$Repair = [switch](@($installInputs.switches) -contains 'Repair')
$ApplySystemTuning = [switch](@($installInputs.switches) -contains 'ApplySystemTuning')
# The run after a restart keeps the shortcut answers of the first run, and a positive switch on this command line
# (-StartMenuShortcut, -DesktopShortcut) clears the saved negative one (Get-InstallInputs).
$NoStartMenuShortcut = [switch](@($installInputs.switches) -contains 'NoStartMenuShortcut')
$NoDesktopShortcut = [switch](@($installInputs.switches) -contains 'NoDesktopShortcut')
$NoWallpaper = [switch](@($installInputs.switches) -contains 'NoWallpaper')
# The start-screen picture is the one answer of the three with a "nobody stated one" state of its own: the value it
# writes is absent by default, so a run that states nothing must leave the installed driver's value alone instead of
# writing the default over it. $null means nothing stated (Get-BootLogoPlan in common.ps1).
$bootLogoStated = $null
if (@($installInputs.switches) -contains 'NoBootLogo') { $bootLogoStated = $false }
elseif ([bool]$BootLogo) { $bootLogoStated = $true }
# BD-069: a repair that the tester asked for writes the release default again over a switch that the driver closed
# itself (the tester asked for the release as it ships). Only the -Repair switch asks for that. Every other install
# keeps the closure and reports it with its remedy, and so does the automatic 'repair' action of an unfinished or
# failed phase (Get-InstallAction): nobody asked for the closure to go, and what made the driver close it may still
# be there. The run after a restart gets -Repair back from the state, so a repair that waits for the driver package
# still reopens.
$script:ReopenClosures = [bool]$Repair

# Verification needs no preflight. It checks the installed release against one manifest, which also names it in the
# verify report: <install root>\manifest.json, read once (its bytes are hashed and parsed together), whatever package
# started the verification; the package's own manifest only when nothing is installed there.
if ($action.action -eq 'verify' -and -not ($DryRun -and -not $Verify)) {
    $script:VerifyManifestSource = 'install-root'
    $m = Join-Path $InstallRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $m)) { $m = Join-Path $package 'manifest.json'; $script:VerifyManifestSource = 'package' }
    $script:VerifyManifestSha256 = $null
    if (Test-Path -LiteralPath $m) {
        $bytes = [IO.File]::ReadAllBytes($m)
        $script:VerifyManifestSha256 = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', '')
        $script:Manifest = (New-Object Text.UTF8Encoding $false).GetString($bytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
    }
    Write-Info "verifies against $m ($($script:VerifyManifestSource)): $(if ($script:Manifest) { $script:Manifest.version } else { 'no manifest' })"
    $state = $early
    if (-not $state) { $state = [pscustomobject]@{ schema = 1; phase = 'unknown' } }
    $script:VerifyOnly = $true
} else {
Enter-Stage 'preflight' 'Preflight'
try { $checks = Invoke-Preflight }
catch {
    Write-Fail "preflight error: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage)"
    Write-Host 'Preflight could not finish. Nothing was changed.' -ForegroundColor Red
    Exit-Engine -Code 2 -Outcome 'refused' -MessageId 'result.preflight-error' -Detail ([string]$_.Exception.Message)
}
foreach ($c in $checks) {
    $color = switch ($c.result) { 'ok' { 'Green' } 'warn' { 'Yellow' } default { 'Red' } }
    Write-Host ('   [{0,-4}] {1,-22} {2}' -f $c.result, $c.check, $c.detail) -ForegroundColor $color
    Write-Log ('   [{0}] {1}: {2}' -f $c.result, $c.check, $c.detail)
    Write-EngineEvent 'check' ([ordered]@{ id = $c.id; result = $c.result; name = $c.check; detail = $c.detail })
}
$failed = @($checks | Where-Object { $_.result -eq 'fail' })
if ($failed.Count) {
    $script:EngineFailedChecks = @($failed | ForEach-Object { $_.id })
    Write-Host ''
    Write-Host "Preflight refused the installation: $($failed.Count) check(s) failed. Nothing was changed." -ForegroundColor Red
    Exit-Engine -Code 2 -Outcome 'refused' -MessageId 'result.preflight-refused' -Detail (($failed | ForEach-Object { "$($_.check): $($_.detail)" }) -join ' | ')
}

$state = $script:state
if (-not $state) { $state = [pscustomobject]@{ schema = 1; phase = 'new'; created_utc = [DateTime]::UtcNow.ToString('o'); updated_utc = $null } }
}
function Save-Phase([string]$Phase) {
    Set-StateValue $state 'phase' $Phase
    # A pending phase records its boot: it advances only in another one (Get-PendingRestart).
    if ($Phase -in $script:PendingPhases) { Set-StateValue $state 'restart_boot_id' (Get-BootIdentity).boot_id }
    Set-StateValue $state 'updated_utc' ([DateTime]::UtcNow.ToString('o'))
    Save-InstallState $state
}
# A restart that this install needs. The boot it was asked in goes into the state first (a boundary advances only in
# a new boot). A setup-window run returns the restart to the window, which starts its countdown. The command line
# starts its own countdown and requests a normal, planned restart:
# programs may keep their unsaved work, and no restart is forced.
function Request-Restart([string]$Why, [string]$ReasonId) {
    $boot = (Get-BootIdentity).boot_id
    Set-StateValue $state 'restart_boot_id' $boot
    Save-InstallState $state
    $script:EngineRestart = [ordered]@{ required = $true; reason_id = $ReasonId; boot_id = $boot; continuation = $script:Continuation }
    Write-EngineEvent 'restart-required' ([ordered]@{ reason_id = $ReasonId; continuation = $script:Continuation })
    Write-Host ''
    Write-Host "A restart is needed: $Why" -ForegroundColor White
    if ($script:GuiMode) { Write-Info 'the setup window offers the restart'; return }
    if ($script:DryRunMode -or $NoReboot) { Write-Info 'restart the computer yourself; the installer continues after you log on again'; return }
    # The restart starts by itself after a visible countdown: nothing waits for an answer here
    # either. A key press during the countdown puts the restart off, Enter starts it at once.
    $go = (Wait-RestartCountdown -Seconds $script:RestartCountdownSeconds) -eq 'now'
    if ($go) {
        $failure = Request-PlannedRestart
        if ($failure) { Write-Warn2 "Windows did not start the restart ($failure): restart the computer yourself; the installer continues after you log on again" }
    } else { Write-Info 'restart later; the installer continues after you log on again' }
}

# ---- automatic diagnostics of a start that did not pass -------------------------------------
# "The installer must collect the logs itself when the install does not pass": the readings verify has already taken
# decide it (diagnostics.ps1 Get-StartFailureDecision), and when they show a driver that did not start, ONE
# diagnostics file is written for this run, next to the installer's log. The engine names it in result.json, the
# setup window puts it into the support file, and the last console lines print its path.
# It never changes a verify result, a message id or an exit code. A collection that fails, in part or altogether,
# is a line inside the file and an [info] line here, and verify ends exactly as it did before.
# The KMD's StartRecord through the one reader of start-confirm-core.ps1: the registry, with no gate and without
# the driver, which is the point - a device at problem code 43 answers no escape.
function Read-VerifyStartRecord {
    try {
        $k = Open-DriverStateRecordView -Path $script:StartRecordKey -Cli (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe')
        if (-not $k) { return $null }
        try { return Read-StartRecordFromKey $k } finally { $k.Dispose() }
    } catch { return [ordered]@{Result='unreadable';Detail=[string]$_.Exception.Message} }
}
function Invoke-VerifyDiagnostics {
    # $BootIdSource names where $BootId came from, so the file says whether this boot's number is the one the
    # driver writes (KUSER_SHARED_DATA) or the registry copy that can lag it (BD-130).
    param($DeviceState, $StartRecord, [AllowNull()]$BootUtc, [AllowNull()]$BootId, [string]$Context = 'verify', [scriptblock]$Info,
          [AllowNull()][string]$BootIdSource = (Get-StartRecordBootIdSource))
    $decision = $null
    try {
        $decision = Get-StartFailureDecision -ProblemCode $DeviceState.problem_code -DevicePresent $DeviceState.present `
            -DeviceStatus ([string]$DeviceState.status) -Record $StartRecord -BootUtc $BootUtc `
            -HealthFlags $DeviceState.health_flags -LastStage $DeviceState.last_stage
        Write-Log "   diagnostics: $($decision.summary)"
        if (-not $decision.collect) { return }
        if ($script:DryRunMode) { Write-Log '   diagnostics: a dry run collects nothing'; return }
        $path = Join-Path $script:StateDir ('diagnostics-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.txt')
        $r = Write-StartFailureDiagnostics -Path $path -Decision $decision -DeviceState $DeviceState -BootUtc $BootUtc -BootId $BootId `
            -BootIdSource $BootIdSource `
            -ParametersSubKey "SYSTEM\CurrentControlSet\Services\$($script:ServiceName)\Parameters" `
            -HardwareId $script:HardwareIdPrefix -Context $Context
        Write-Log "   diagnostics: written $($r.written), $($r.bytes) bytes in $($r.seconds) s, sections: $(@($r.sections) -join ' | ')"
        foreach ($p in @($r.problems)) { Write-Log "   diagnostics problem: $p" }
        if (-not $r.written) { if ($Info) { & $Info 'diagnostics' "the driver did not start, and the details could not be written: $(@($r.problems) -join '; ')" }; return }
        $script:EngineDiagnostics = $r.path
        Write-EngineEvent 'diagnostics' ([ordered]@{ collected = $true; path = $r.path; bytes = $r.bytes; seconds = $r.seconds; cut = [bool]$r.cut; reasons = @($decision.reasons) })
        if ($Info) { & $Info 'diagnostics' "the driver did not start; the details of this run are in $($r.path) (and in the support file of the setup window)" }
    } catch {
        Write-Log "   diagnostics: not collected, $($_.Exception.Message)"
        if ($Info) { & $Info 'diagnostics' "the details of the failed start could not be collected: $($_.Exception.Message)" }
    }
}

# ---- phase 3: verify -------------------------------------------------------------------------------------------
$script:VerifyComplete = $false
function Invoke-Verify {
    Write-Step 'Verify'
    $results = New-Object System.Collections.ArrayList
    function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
        [void]$results.Add([pscustomobject]@{ check = $Name; pass = $Ok; detail = $Detail })
        if ($Ok) { Write-Host ('   [pass] {0,-20} {1}' -f $Name, $Detail) -ForegroundColor Green } else { Write-Host ('   [FAIL] {0,-20} {1}' -f $Name, $Detail) -ForegroundColor Red }
        Write-Log ('   verify {0}: {1} {2}' -f $Name, $Ok, $Detail)
    }
    # A warning names a state that needs the tester's action: it does not fail verify, and it does not replace a bug
    # report. An info line says what verify could not establish.
    function Add-Warning([string]$Name, [string]$Detail) {
        [void]$results.Add([pscustomobject]@{ check = $Name; pass = $true; warning = $true; detail = $Detail })
        Write-Host ('   [warn] {0,-20} {1}' -f $Name, $Detail) -ForegroundColor Yellow
        Write-Log ('   verify {0}: warning {1}' -f $Name, $Detail)
    }
    function Add-Info([string]$Name, [string]$Detail) {
        [void]$results.Add([pscustomobject]@{ check = $Name; pass = $true; info = $true; detail = $Detail })
        Write-Host ('   [info] {0,-20} {1}' -f $Name, $Detail)
        Write-Log ('   verify {0}: info {1}' -f $Name, $Detail)
    }
    # A probe that cannot answer on a device whose driver did not start (Get-VerifyStartState). It keeps its own name
    # in the report, with the reason, and it is not a failure of its own: the failed start is already the verdict of
    # the 'driver bound' and 'driver start' results. Without this a failed start reported several checks that only
    # restated it, and spent minutes doing so (GitHub #38).
    function Add-Skipped([string]$Name, [string]$Detail) {
        Add-Info $Name ('skipped: the driver did not start' + $(if ($Detail) { "; $Detail" } else { '' }))
    }
    # Every verify run ends with its report (Write-VerifyReport), also when a check throws or the GPU is missing: the
    # control application counts a verification only from a report with a non-empty result list for the installed
    # package, never from the phase label.
    try {
    if (-not $script:DryRunMode) {
        # Files replaced while in use: the restart deletes their old copies; this catches any it could not.
        $n = (Remove-OldCopies -Directory $InstallRoot -Recurse) + (Remove-OldCopies -Directory (Join-Path $env:windir 'System32') -Filter 'bc250umd.dll.old-*') + (Remove-OldCopies -Directory (Join-Path $env:windir 'SysWOW64') -Filter 'bc250umd.dll.old-*')
        if ($n) { Write-Info "removed $n old copies of replaced files" }
    }
    $dev = @(Get-Bc250Device)
    if ($dev.Count -ne 1) {
        Add-Result 'device' $false 'BC-250 GPU not found'
        # A GPU that is not there after an installation is a start failure too: the driver's own record of the start
        # and the logs of this boot are collected here as well, because nothing below this line runs.
        Invoke-VerifyDiagnostics -DeviceState ([ordered]@{ present = $false; devices = $dev.Count }) `
            -StartRecord (Read-VerifyStartRecord) -BootUtc (Get-StartRecordBootUtc) -BootId (Get-StartRecordBootId) -Context 'verify: no device with our hardware id' -Info ${function:Add-Info}
        return $results
    }
    $id = $dev[0].DeviceID
    $svc = Get-DeviceServiceName -InstanceId $id
    $pnp = Get-PnpDevice -InstanceId $id
    Add-Result 'driver bound' (($svc -eq $script:ServiceName) -and ($pnp.Status -eq 'OK')) "service $svc, status $($pnp.Status), problem code $($dev[0].ConfigManagerErrorCode)"
    # Everything below that needs a running driver asks this first.
    $startState = Get-VerifyStartState -ProblemCode ([int]$dev[0].ConfigManagerErrorCode) -Status ([string]$pnp.Status)
    $drv = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID='$($id -replace '\\', '\\')'" -ErrorAction SilentlyContinue
    $want = $null
    if ($script:Manifest) { $want = Get-ManifestDriverQuad $script:Manifest }
    # The Windows INF quad names the package independently of the embedded KMD build.
    $inf = $null
    if ($drv) { $inf = $drv.InfName }
    # Win32_PnPSignedDriver leaves DriverProviderName empty for this package; the device property has the INF's Provider.
    $provider = $null
    try { $provider = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverProvider -ErrorAction Stop).Data } catch { }
    if (-not $provider -and $drv) { $provider = $drv.DriverProviderName }
    $versionTags=$null
    if(-not $script:Manifest.version_identity) {
        $versionKey=Get-DeviceDriverKey -InstanceId $id
        if($versionKey){$versionTags=Get-ItemProperty -LiteralPath $versionKey -ErrorAction Stop}
    }
    $verCheck=Test-InstalledDriverVersion -Installed ([string]$drv.DriverVersion) -Expected $want `
        -Original ([string]$versionTags.Bc250DriverVersion) -Reported ([string]$versionTags.Bc250ReportedDriverVersion) `
        -InstallationOwned ([bool]$script:Manifest.version_identity)
    Add-Result 'driver version' $verCheck.ok "installed $($drv.DriverVersion) from $inf (provider $provider), package $want$($verCheck.note)"
    # BD-150: the version names the release family; the loaded image names the KMD. An equal DriverVer let 29v2 verify
    # pass on another KMD.
    $kmdComp = $null
    if ($script:Manifest) { $kmdComp = @($script:Manifest.components | Where-Object { $_ -and $_.package_path -eq 'payload/kmd/bc250kmd.sys' }) | Select-Object -First 1 }
    if (-not $startState.started) {
        Add-Skipped 'driver image' "$($startState.reason); no bc250kmd.sys image is loaded"
    } elseif (-not $kmdComp -or -not $kmdComp.sha256) {
        Add-Info 'driver image' 'the manifest of this release names no KMD image hash'
    } else {
        $loaded = (Get-RunningKmdReading -Cli $null).image
        $wantImage = ([string]$kmdComp.sha256).ToUpperInvariant()
        if (-not $loaded -or -not $loaded.sha256) {
            Add-Result 'driver image' $false "no readable bc250kmd.sys among the loaded drivers; package $($wantImage.Substring(0, 8))"
        } else {
            Add-Result 'driver image' ($loaded.sha256 -eq $wantImage) "loaded $($loaded.path) $($loaded.sha256.Substring(0, 8)), package $($wantImage.Substring(0, 8))"
            if ($script:Manifest.version_identity -and $loaded.sha256 -eq $wantImage) {
                try {
                    $tags = Remove-LegacyDriverVersionTags -InstanceId $id -Manifest $script:Manifest -LoadedSha256 $loaded.sha256
                    Add-Result 'package version identity' $true "INF, Windows properties and loaded KMD agree; $tags obsolete private tags removed"
                } catch { Add-Result 'package version identity' $false $_.Exception.Message }
            }
        }
    }
    # One read-only reading of the start-confirm task's inputs, in a child process (start-confirm.ps1 -Probe): the
    # driver version and LastStage through bc250kmd_cli, the KMD start health through bc250control.dll, the DPM
    # registry state. The task itself runs at the same logon and may still be waiting for its 60 s.
    # Verify starts at the same logon as the start-confirm task, which needs about a minute: wait for this boot's run
    # of the task to end (at most 120 s), so that the confirmation and DPM lines below are final.
    #
    # Not on a device that did not start: the task confirms a start, it has nothing to confirm here, and the wait
    # would spend its whole bound before saying so (GitHub #38: 110 s of a 2 min 43 s verification). Its last result
    # is read at once instead, and the line below says why nothing was waited for.
    $taskNote = 'start-confirm task: not registered'
    $task = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
    if ($task) {
        $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
        $waited = [Diagnostics.Stopwatch]::StartNew()
        $announced = $false
        $noWaitWhy = $(if ($startState.started) { $null } else { $startState.reason })
        while ($null -eq $noWaitWhy) {
            $ti = Get-ScheduledTaskInfo -TaskName $script:TaskName -ErrorAction SilentlyContinue
            $running = (Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue).State -eq 'Running'
            $ranThisBoot = $ti -and $ti.LastRunTime -and ($ti.LastRunTime -ge $boot)
            if (($ranThisBoot -and -not $running) -or $script:DryRunMode) { break }
            if (Test-DeadlinePassed) { Write-DeadlineNotice 'verify:start-confirm-wait'; break }
            if ($waited.Elapsed.TotalSeconds -ge 120) { break }
            if (-not $announced) { Write-Info 'waiting for the start-confirm task of this logon to finish (at most 120 s)...'; $announced = $true }
            Start-Sleep -Seconds 2
        }
        if ($null -ne $noWaitWhy) { $ti = Get-ScheduledTaskInfo -TaskName $script:TaskName -ErrorAction SilentlyContinue }
        $code = $ti.LastTaskResult
        $meaning = switch ($code) { 0 { 'confirmed through the KMD start health' } 6 { 'fallback: boot-loop guard reset only, DPM not confirmed' } 1 { 'bound reached unconfirmed' } 5 { 'no start-health reading' } 267009 { 'still running' } default { 'see C:\ProgramData\amdgpu-wddm\start-confirm.log' } }
        $taskNote = "start-confirm task: last run $($ti.LastRunTime), result $code ($meaning), waited $([int]$waited.Elapsed.TotalSeconds) s"
        if ($null -ne $noWaitWhy) {
            $taskNote = "$taskNote; the wait was skipped because $noWaitWhy, so this logon's task cannot confirm a start that did not happen"
        }
    }
    Write-Info $taskNote
    $sc = Join-Path $InstallRoot 'tools\start-confirm.ps1'
    $probe = ''
    if (Test-Path -LiteralPath $sc) { $probe = (Invoke-Native powershell.exe @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $sc, '-Probe')).text }
    Write-Log $probe
    # Each reading is taken from one whole line of the probe's log, it keeps the 'probe: ' prefix the probe writes,
    # and it refuses any line that carries driver log text. BD-122 puts up to 64 raw driver log lines in this
    # output, every one of them prefixed 'start record log: ', and a pattern that matched anywhere in the text
    # could otherwise take its answer from a line the driver wrote (start-confirm.ps1 Write-StartRecordLog).
    $view = $null; if ($probe -match '(?m)^(?!.*start record log: ).*probe: fallback view: (.+)$') { $view = $Matches[1].Trim() }
    # BD-122: the KMD's own reason for a start that did not reach a started device, which the probe reads from
    # Parameters\StartRecord with no gate and without the driver. It names the step, the NTSTATUS and the last
    # driver log lines, which a device with Code 43 can no longer answer for itself. The verdict below is the one
    # it always was: this is the detail a tester sends us, not a new gate.
    $record = $null; if ($probe -match '(?m)^(?!.*start record log: ).*\bstart record: (.+)$') { $record = $Matches[1].Trim() }
    $want = [string]$script:Manifest.kmd_abi
    $driverOk = ($null -ne $view) -and ($view -match 'device problem 0,') -and ($view -match 'LastStage 50$') -and ($view -match "driver version $want ")
    $detail = $(if ($view) { $view } else { "no reading from $sc" })
    if (-not $driverOk -and $record) { $detail = "$detail; $record" }
    Add-Result 'driver start' $driverOk $detail
    $health = $null; if ($probe -match '(?m)^(?!.*start record log: ).*probe: start health (.+)$') { $health = $Matches[1].Trim() }
    $hflags = $null; if ($health -match 'flags (\d+)') { $hflags = [int]$Matches[1] }
    Add-Result 'start health' (($null -ne $hflags) -and (($hflags -band 7) -eq 7)) $(if ($health) { "$health (15 = confirmed; 7 = full, ready, visible)" } else { 'no reading' })
    # Everything the decision of diagnostics.ps1 needs is now read, and nothing of it is read twice: the device and
    # the driver from the checks above, LastStage and the driver's reply from the probe's own view line.
    $lastStage = $null; if ($view -match 'LastStage (\d+)') { $lastStage = [int]$Matches[1] }
    $replyVersion = $null; if ($view -match 'driver version (0x[0-9A-Fa-f]{8})') { $replyVersion = $Matches[1] }
    Invoke-VerifyDiagnostics -DeviceState ([ordered]@{
            present = $true; instance_id = $id; problem_code = $dev[0].ConfigManagerErrorCode; status = [string]$pnp.Status
            service = $svc; driver_version = $(if ($drv) { [string]$drv.DriverVersion } else { $null }); inf = $inf
            provider = [string]$provider; package_version = (Get-ManifestDriverQuad $script:Manifest)
            reply_version = $replyVersion; last_stage = $lastStage; health_flags = $hflags
        }) -StartRecord (Read-VerifyStartRecord) -BootUtc (Get-StartRecordBootUtc) -BootId (Get-StartRecordBootId) -Context 'verify' -Info ${function:Add-Info}
    $stateReadError=$null;$p=$null
    try {$p=[pscustomobject](Read-DriverStateValues -Cli (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe'))}
    catch {$stateReadError=[string]$_.Exception.Message}
    Add-Result 'shared state' ($null -eq $stateReadError) $(if($stateReadError){$stateReadError}else{'validated catalog and active generation'})
    $uc=$p.UnconfirmedStarts
    Add-Result 'boot-loop guard' (($null -eq $stateReadError) -and ($null -ne $uc) -and ([int64]$uc -lt 2)) "UnconfirmedStarts $uc (the start-confirm task resets it after each logon)"
    # The full WDDM gate: 0 (the INF's value) starts the KMD display-only, with no GPU work at all.
    $closed = @($p.PSObject.Properties | Where-Object { $_.Name -like 'Enable*' -and $_.Value -is [int] -and $_.Value -eq 0 } | ForEach-Object { $_.Name })
    Add-Result 'full WDDM gate' (($p.EnableFullWddm -eq 1) -or ($p.EnableFullWddm -eq 2)) "EnableFullWddm $($p.EnableFullWddm) (2 = open at every start)$(if ($closed.Count) { '; gates at 0: ' + ($closed -join ', ') })"
    # A tester who set DpmMode 0 asked for fixed clocks: that start is as configured, not a failure.
    Add-Result 'DPM' ((($p.DpmMode -eq 1) -and ($p.DpmLastMode -eq 1)) -or (($null -ne $p.DpmMode) -and ($p.DpmMode -ne 1))) "DpmMode $($p.DpmMode), this start ran $(if ($p.DpmLastMode -eq 1) { 'DPM' } else { "fixed (reason $($p.DpmLastReason))" }), DpmMaxMHz $($p.DpmMaxMHz), confirmed $(if ($null -ne $p.DpmConfirmed) { 'yes' } else { 'not yet' })"

    # The GPU desktop path: the KMD's interop switches are effective (blit+cdd) and the last boot did not die inside a
    # session (BD-059); with DwmForceCpu 0 the DWM of this logon session runs the zink UMD. RequireKmdSwitches sends
    # DWM to the CPU route by itself when the switches are off, so a closed path shows here, not as a black desktop.
    $cli = Join-Path $InstallRoot 'tools\bc250kmd_cli.exe'
    $io = ''
    # The driver answers for its own interop state. A device that did not start cannot, so it is not asked.
    if ((Test-Path -LiteralPath $cli) -and $startState.started) { $io = [string](Invoke-Native $cli @('interop')).text }
    Write-Log $io
    $ioFirst = (($io -split "`n") | Select-Object -First 1)
    $ioOk = ($io -match 'effective blit\+cdd') -and ($io -notmatch 'died in a session')
    $forceCpu = (Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\DesktopRouter" -Name DwmForceCpu -ErrorAction SilentlyContinue).DwmForceCpu
    $dwmNote = "DwmForceCpu $forceCpu (desktop on the CPU route as set)"
    $dwmOk = $true
    if ($forceCpu -eq 0) {
        $zink = $false
        $mySession = Get-DesktopSessionId
        foreach ($d in @(Get-Process dwm -ErrorAction SilentlyContinue | Where-Object { $mySession -gt 0 -and $_.SessionId -eq $mySession })) {
            try { if ($d.Modules | Where-Object { $_.FileName -match 'bc250d3d_zink\.dll$' }) { $zink = $true } } catch { }
        }
        $dwmOk = $zink
        $dwmNote = "DwmForceCpu 0, DWM of session $mySession has bc250d3d_zink.dll loaded: $(if ($zink) { 'yes' } else { 'no (CPU route)' })"
        if ($mySession -le 0) { $dwmNote = 'DwmForceCpu 0, no interactive desktop session is available; GPU desktop verification failed' }
    }
    # BD-069: the driver closed the path itself, so the remedy is a repair install, not a setting the tester has to
    # find. The repair reads the driver's own record of the closure (InteropClosedReason), so only the line that
    # proves the record is there gets the remedy. A boot that died with both switches already at 0 leaves no record,
    # a repair would change nothing, and the reading says so without naming one.
    $ioClosed = ''; $ioRemedy = ''
    if ($io -match 'closed by the driver: (\S+)') {
        $ioClosed = "; closed by the driver: $($Matches[1])"
        $ioRemedy = '; remedy: run install.cmd -Repair, which opens it again'
    }
    if ($io -match 'died in a session') {
        $ioClosed += '; last boot died in a session'
        if (-not $ioRemedy) { $ioRemedy = '; a repair changes nothing here, because the driver left no record of a closure: open the GPU desktop path in Nagi Control App' }
    }
    $ioClosed += $ioRemedy
    if (-not $startState.started) {
        Add-Skipped 'GPU desktop path' "$($startState.reason); the driver cannot be asked for its interop state. $dwmNote"
    } else {
        Add-Result 'GPU desktop path' ($ioOk -and $dwmOk) $(if ($io) { "$($ioFirst.Trim())$ioClosed; $dwmNote" } else { "no reading from $cli; $dwmNote" })
    }

    # BD-060: a replacement of the session's DWM that was observed against the start-confirm task's record of this
    # logon (dwm-session.ps1). A warning with its remedy that helps to attribute a failure; unknown history is reported
    # as unknown, neither as a restart nor as healthy.
    try {
        $epoch = Get-DwmEpoch
        $dwmFinding = Get-DwmReplacementFinding (Find-DwmBaseline (Read-DwmBaseline) $epoch) (Get-SessionDwm $epoch.session) $epoch
    } catch { $dwmFinding = [pscustomobject]@{ state = 'unknown'; detail = "unknown: $($_.Exception.Message)" } }
    switch ($dwmFinding.state) {
        'observed' { Add-Warning 'DWM restarted in this session' $dwmFinding.detail }
        'same' { Add-Result 'DWM restarted in this session' $true $dwmFinding.detail }
        default { Add-Info 'DWM restarted in this session' $dwmFinding.detail }
    }

    # D3D12 through the system runtime, as an application sees it. Not on a device that did not start: no D3D12
    # device can be created on an adapter whose driver is not running, so the probe would only restate the start.
    $caps = Join-Path $InstallRoot 'tools\amdgpu_wddm_d3d12caps.exe'
    $vdir = Join-Path $script:StateDir 'verify'
    [void][IO.Directory]::CreateDirectory($vdir)
    $index = $null
    if (-not $startState.started) {
        Add-Skipped 'D3D12 FL 12_1' "$($startState.reason); no D3D12 device can be created on an adapter without a running driver"
    } else {
        try {
            $first = Join-Path $vdir 'd3d12caps-0.json'
            [void](Invoke-Native $caps @('0', $first))
            $j = Get-Content -LiteralPath $first -Raw | ConvertFrom-Json
            foreach ($p in $j.dxgi.adapters.PSObject.Properties) {
                $d = $p.Value.GetDesc1
                if ($d.VendorId -eq '0x1002' -and $d.DeviceId -eq '0x13FE') { $index = [int]$p.Name; break }
            }
            if ($null -eq $index) { Add-Result 'D3D12 FL 12_1' $false 'no DXGI adapter 1002:13FE' }
            else {
                $out = Join-Path $vdir "d3d12caps-$index.json"
                if ($index -ne 0) { [void](Invoke-Native $caps @([string]$index, $out)) } else { $out = $first }
                $j = Get-Content -LiteralPath $out -Raw | ConvertFrom-Json
                $fl = $j.device.features.FEATURE_LEVELS.list_all.MaxSupportedFeatureLevel
                $created = $j.device.created_at
                $ok = ($created -and ($fl -eq '12_1' -or $fl -eq '12_2'))
                Add-Result 'D3D12 FL 12_1' $ok "adapter $index, device created at $created, max feature level $fl"
            }
        } catch { Add-Result 'D3D12 FL 12_1' $false $_.Exception.Message }
    }

    # Vulkan through the system loader, in a child process so that a failing ICD cannot take the installer down.
    # Not on a device that did not start: the loader can enumerate no physical device of an adapter without a driver.
    $vk = Join-Path $here 'vk-check.ps1'
    if (-not $startState.started) {
        Add-Skipped 'Vulkan' "$($startState.reason); the loader enumerates no device of an adapter without a running driver"
    } else {
        $vkOut = (Invoke-Native powershell.exe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $vk)).text.Trim()
        if ($vkOut -match '^SKIP') { Write-Host "   [skip] Vulkan               $vkOut" -ForegroundColor Yellow; Write-Log "   verify Vulkan: $vkOut" }
        else { Add-Result 'Vulkan' ($vkOut -match '(?m)^device 0x1002:0x13FE') $vkOut }
    }
    # The two Vulkan loaders that Vulkan and OpenGL programs load (common.ps1, "the Vulkan loader"): present, the
    # version, who made and signed the file, and whether the installer of this driver installed it
    # (Get-VulkanLoaderVerdict). A missing loader fails only when the loader step reported success: then the step and
    # the files disagree. Every other missing or older loader is a warning with its remedy, because the driver itself
    # is installed and works for Direct3D and the desktop.
    $vkReadings = @{}
    foreach ($arch in $script:VulkanLoaderFiles.Keys) { $vkReadings[$arch] = Get-VulkanLoaderReading (Join-Path $env:windir $script:VulkanLoaderFiles[$arch]) -WithSigner }
    $vkState = Read-InstallState
    $vkRecord = $(if ($vkState -and $vkState.PSObject.Properties['vulkan_loader']) { $vkState.vulkan_loader } else { $null })
    $vkVerdict = Get-VulkanLoaderVerdict -Readings $vkReadings -Record $vkRecord
    switch ($vkVerdict.level) {
        'fail' { Add-Result 'Vulkan loader' $false $vkVerdict.text }
        'warn' { Add-Warning 'Vulkan loader' $vkVerdict.text }
        default { Add-Result 'Vulkan loader' $true $vkVerdict.text }
    }
    # 32-bit Vulkan through SysWOW64\vulkan-1.dll and the WOW6432Node registration: the same check in the 32-bit
    # powershell.exe, whose System32 is SysWOW64. A failure is a warning: a problem of 32-bit enumeration does not
    # make the verify of the driver installation fail.
    $ps32 = Join-Path $env:windir 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    if (-not $startState.started) {
        Add-Skipped 'Vulkan (32-bit)' "$($startState.reason); the loader enumerates no device of an adapter without a running driver"
    } elseif (-not (Test-Path -LiteralPath $ps32 -PathType Leaf)) {
        Add-Info 'Vulkan (32-bit)' "not checked: $ps32 is not on this computer"
    } else {
        $vkOut32 = (Invoke-Native $ps32 @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $vk)).text.Trim()
        if ($vkOut32 -match '^SKIP') { Add-Warning 'Vulkan (32-bit)' ($vkOut32 -replace 'System32', 'SysWOW64') }
        elseif ($vkOut32 -match '(?m)^device 0x1002:0x13FE') { Add-Result 'Vulkan (32-bit)' $true $vkOut32 }
        else { Add-Warning 'Vulkan (32-bit)' "$vkOut32. 32-bit Vulkan programs and games may not find the GPU: report it with the logs of this verify" }
    }

    # The 64-bit registration: the four UserModeDriverName slots (the D3D9 slot holds '<>', so that D3D9 goes
    # through D3D9On12) and VulkanDriverName. Registration only: the D3D12 and Vulkan checks above create the devices.
    $gpuSwKey = Get-DeviceDriverKey -InstanceId $id
    $verifiedGraphicsRoot = $InstallRoot
    if ($gpuSwKey -and (Test-DriverStoreUmdPackage $script:Manifest)) {
        try {
            $verifiedGraphicsRoot = Get-DriverPackageRoot -ClassKey $gpuSwKey
            if (-not $verifiedGraphicsRoot) { throw 'bound adapter has no DriverStore UMD package identity' }
            $packageProblems = @(Test-DriverPackageRegistration -Manifest $script:Manifest -ClassKey $gpuSwKey -Root $verifiedGraphicsRoot)
            foreach ($adlPath in @(Get-AdlSearchPaths -Package $script:Manifest.driver_package -Root $verifiedGraphicsRoot)) {
                if (-not (Test-PathEntry (Get-MachinePath).value $adlPath)) { $packageProblems += 'ADL directory missing from machine PATH' }
            }
            $packageProblems += @(Get-LegacyUmdValues -InstallRoot $InstallRoot | ForEach-Object { "legacy UMD override or duplicate ICD: $($_.name)" })
            Add-Result 'DriverStore UMD set' ($packageProblems.Count -eq 0) $(if ($packageProblems.Count) { $packageProblems -join '; ' } else { 'INF registration and every installed member match the package manifest' })
        } catch { Add-Result 'DriverStore UMD set' $false $_.Exception.Message; $verifiedGraphicsRoot = $InstallRoot }
    }
    if (-not $gpuSwKey) { Add-Result 'D3D registration' $false 'no software key for the GPU' }
    else {
        $reg = @(Test-UmdRegistration -InstallRoot $InstallRoot -ClassKey $gpuSwKey)
        Add-Result 'D3D registration' ($reg.Count -eq 0) $(if ($reg.Count) { $reg -join '; ' } else { 'UserModeDriverName (D3D9: <> for D3D9On12; D3D10/11 router; D3D12), VulkanDriverName' })
    }
    # 32-bit processes (BD-064): the Wow registration, the router's Wow paths and x86 images in place. Registration
    # only: no 32-bit device is created here.
    if (-not $gpuSwKey) { Add-Result '32-bit D3D/Vulkan' $false 'no software key for the GPU' }
    else {
        $wow = @(Test-WowRegistration -InstallRoot $InstallRoot -ClassKey $gpuSwKey)
        Add-Result '32-bit D3D/Vulkan' ($wow.Count -eq 0) $(if ($wow.Count) { $wow -join '; ' } else { 'UserModeDriverNameWow (D3D9: <> for D3D9On12; D3D10/11 router; D3D12), VulkanDriverNameWow, WOW6432Node Khronos entry, router Wow paths; x86 images in place' })
    }

    # The OpenGL ICD (BD-076): the values of the GPU's software key and the ICD file of every architecture this
    # release registers (manifest.json "opengl_icd": "architectures"). Registration and files only. No OpenGL context
    # is created here: the installer runs in session 0, where there is no desktop to give a window a pixel format, so
    # a context test belongs to a client of ours on a logged-on desktop (tools/win/gltest). A release that does not
    # register the ICD says so and checks nothing.
    # An architecture outside the list gets one information line and no check: a program of that architecture keeps
    # the renderer Windows gives it, which is the software OpenGL 1.1 of opengl32.dll. That is a statement about this
    # release, not a fault of the computer, so it is an information line and never a failing check.
    try {
        $glSwitch = Get-OpenGlReleaseSwitch -Manifest $script:Manifest -PackageRoot $null
        if (-not $glSwitch.register) {
            Add-Info 'OpenGL ICD' 'not registered by this release'
        } elseif (-not $gpuSwKey) {
            Add-Result 'OpenGL ICD' $false 'no software key for the GPU'
        } else {
            $glProblems = @(Test-OpenGlRegistration -InstallRoot $InstallRoot -ClassKey $gpuSwKey -Switch $glSwitch)
            $glPlanNames = @((Get-OpenGlRegistrationPlan -Switch $glSwitch -InstallRoot $InstallRoot) | ForEach-Object { $_.name })
            Add-Result 'OpenGL ICD' ($glProblems.Count -eq 0) $(if ($glProblems.Count) { $glProblems -join '; ' } else {
                "$($glPlanNames -join ', ') ($($glSwitch.value_type), version $($glSwitch.version), flags $($glSwitch.flags)); the $(@($glSwitch.architectures) -join ' and ') ICD in place with the Drv entry points" })
            if (-not $glSwitch.register_x86) {
                Add-Info 'OpenGL ICD (32-bit)' 'this release registers no 32-bit OpenGL driver: 32-bit OpenGL programs keep the OpenGL renderer of Windows itself'
            }
        }
    } catch { Add-Result 'OpenGL ICD' $false $_.Exception.Message }

    # The H.264 encoder MFT (M15.11). The result of this check is the only place a support report can read whether the
    # encoder is registered, which DLL the registration names and which bytes that DLL has (BugReport.cs collects
    # every verify report). The encoder is an extra, never a condition of the driver, so a registration that is not
    # right is a warning with its remedy, not a failed verification.
    try {
        $mftSwitch = Get-MftReleaseSwitch -Manifest $script:Manifest -PackageRoot $null
        $mftInstalled = Join-Path $verifiedGraphicsRoot $mftSwitch.install_path
        if (-not $mftSwitch.register) {
            Add-Info 'H.264 encoder' "not registered by this release; keys of ours on this computer: $(@(Get-MftRegistrationKeysPresent -ClassesKey $script:ClassesKey).Count)"
        } elseif ($script:DryRunMode) {
            Add-Info 'H.264 encoder' "registered by this release: $($mftSwitch.clsid), $mftInstalled"
        } else {
            # The values of the installed DLL when it can be read, so that the check compares the registration with
            # the file it names; the keys and the names alone when it cannot.
            $mftBlobs = $null
            $mftWhy = ''
            try { $mftBlobs = Get-MftRegistrationBlobs -DllPath $mftInstalled } catch { $mftWhy = " (the values of $($mftSwitch.install_path) could not be read: $($_.Exception.Message))" }
            $mftState = Test-MftRegistration -ClassesKey $script:ClassesKey -DllPath $mftInstalled -Blobs $mftBlobs
            $mftSha = 'no file'
            if (Test-Path -LiteralPath $mftInstalled -PathType Leaf) { $mftSha = (Get-Sha256 $mftInstalled).Substring(0, 16) }
            $mftWhere = "InprocServer32 = $mftInstalled (SHA256 $mftSha), $(@($mftState.keys).Count) of our 3 keys present$mftWhy"
            if ($mftState.ok) { Add-Result 'H.264 encoder' $true "registered, $mftWhere" }
            else { Add-Warning 'H.264 encoder' "the registration is not right: $($mftState.detail). $mftWhere. Remedy: run install.cmd -Repair, and send the support report if it stays. The driver itself is not affected." }
        }
    } catch { Add-Info 'H.264 encoder' "not checked: $($_.Exception.Message)" }
    $script:VerifyComplete = $true
    return $results
    } catch {
        Add-Result 'verify' $false "verify stopped: $($_.Exception.Message)"
        throw
    } finally { Write-VerifyReport $results }
}

# The verify report, %ProgramData%\amdgpu-wddm\installer\verify\verify-<utc>.json (docs/gui/interfaces-setup.md
# section 9): bound to one installed package by release, version and the SHA256 of <install root>\manifest.json, with
# the boot and a non-empty result list (results[].check, .pass, .detail). outcome 'failed' when any result fails, the
# list is empty or the run stopped before its last check. Written through a temporary file; never throws.
function Write-VerifyReport($Results) {
    try {
        $list = @($Results)
        # The manifest the checks used (read once at the start of verification), never a second reading.
        $mf = $script:Manifest; $msha = $script:VerifyManifestSha256; $source = $script:VerifyManifestSource
        $failed = @($list | Where-Object { -not $_.pass }).Count
        $complete = [bool]$script:VerifyComplete
        $report = [ordered]@{
            schema = 'amdgpu-wddm.verify-report/1'
            utc = [DateTime]::UtcNow.ToString('o')
            invocation = $script:EngineInvocationId
            boot_id = (Get-BootIdentity).boot_id
            boot_elapsed_ms = Get-BootElapsedMilliseconds
            dry_run = [bool]$script:DryRunMode
            release = $(if ($mf) { [string]$mf.name } else { $null })
            package_version = $(if ($mf) { [string]$mf.version } else { $null })
            manifest_sha256 = $msha
            manifest_source = $source
            kmd_abi = $(if ($mf) { [string]$mf.kmd_abi } else { $null })
            install_root = $InstallRoot
            outcome = $(if ($failed -eq 0 -and $list.Count -gt 0 -and $complete) { 'passed' } else { 'failed' })
            complete = $complete
            passed = ($list.Count - $failed)
            failed = $failed
            results = $list
        }
        $vdir = Join-Path $script:StateDir 'verify'
        # UTC may repeat after time sync. Keep each report instead of overwriting a previous run's evidence.
        Write-FileAtomic (Join-Path $vdir ('verify-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [guid]::NewGuid().ToString('N') + '.json')) ($report | ConvertTo-Json -Depth 5)
    } catch { Write-Log "   verify report not written: $($_.Exception.Message)" }
}

if ($script:VerifyOnly) {
    # A verification in the boot that completed the installation was stopped above (Get-PendingRestart): the driver
    # runs with the old settings until the restart.
    Enter-Stage 'verify' 'Verify'
    $r = Invoke-Verify
    $bad = @($r | Where-Object { -not $_.pass })
    if (-not $DryRun) { Save-Phase $(if ($bad.Count) { 'verify-failed' } else { 'verified' }) }
    # The running-release witness of this boot (release-witness.ps1): written only when the loaded KMD image and the
    # driver's reply match this release; the control application reads it.
    if (-not $DryRun) {
        $why = Write-RunningReleaseWitness -InstallRoot $InstallRoot -RecordedBy 'verify' -Boot (Get-BootIdentity) -StatePath $script:StatePath -LockHeld
        if ($why) { Write-Log "   running-release witness not written: $why" } else { Write-Log "   running-release witness written: $($script:WitnessPath)" }
        Write-EngineEvent 'witness' ([ordered]@{ written = (-not $why); reason = $why })
    }
    if ($bad.Count) {
        Write-Host "Verification: $($bad.Count) check(s) failed. See INSTALL.md, section 'If something fails'." -ForegroundColor Red
        # The automatic diagnostics of this run, when the driver did not start: the console route has no support
        # file, so the last thing a tester reads is the path of the file to send.
        if ($script:EngineDiagnostics) {
            Write-Host "The driver did not start. Send these two files with the report:" -ForegroundColor Yellow
            Write-Host "   $($script:EngineDiagnostics)" -ForegroundColor Yellow
            Write-Host "   $($script:LogPath)" -ForegroundColor Yellow
        }
        Exit-Engine -Code 3 -Outcome 'verify-failed' -MessageId 'result.verify-failed' -Detail (($bad | ForEach-Object { "$($_.check): $($_.detail)" }) -join ' | ')
    }
    Write-Host 'Verification passed. The BC-250 runs on the amdgpu-wddm driver.' -ForegroundColor Green
    $warned = @($r | Where-Object { $_.PSObject.Properties['warning'] -and $_.warning })
    if ($warned.Count) { Write-Host "$($warned.Count) warning(s) above ([warn]): read the remedy on each line." -ForegroundColor Yellow }
    Exit-Engine -Code 0 -Outcome 'verified' -MessageId $(if ($warned.Count) { 'result.verified-with-warnings' } else { 'result.verified' }) -Detail "$($warned.Count) warning(s)"
}

# ---- the decision (GUI plan A2) ------------------------------------------------------------------------------------
# Everything this run will do comes from here, before any change: the action, the restarts, where the firmware comes
# from, and what happens to each setting (WU-006, WU-044). -Plan stops after it. Nothing here asks the user for a
# permission; the consent list of the decision event stays, empty, for the engine contract.
function Get-InstallSettingsImpact {
    $tbl = Get-Content -LiteralPath (Join-Path $here 'registry-defaults.json') -Raw | ConvertFrom-Json
    $prev = Get-PreviousAppliedDefaults $tbl ((Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\Release" -Name AppliedDefaults -ErrorAction SilentlyContinue).AppliedDefaults)
    $params=@{};$parameterPlan=@()
    try {
        $params=Read-DriverStateValues -Cli (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe') -Override
        $parameterPlan=Get-RegistryDefaultPlan -Defaults $tbl.defaults.parameters -Previous $prev.applied.parameters -Current $params -Explicit (Get-ExplicitParameters $commandLineParameters $params) -Reopen:$script:ReopenClosures
    } catch {
        # Before the first migrated generation, only the SYSTEM export can judge
        # legacy choices. The preview states that gap and does not invent defaults.
        $parameterPlan=@([pscustomobject]@{name='state migration';decision='unavailable';current=$null;value=$null;default=$null;present=$false;
            closure='Settings are checked by the protected migration before driver staging.'})
    }
    $groups = @(
        @{ group = 'parameters'; plan = $parameterPlan }
        @{ group = 'desktop_router'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.desktop_router -Previous $prev.applied.desktop_router -Current (Read-RegistryValues "$($script:SoftwareKey)\DesktopRouter")) }
        @{ group = 'app_router'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.app_router -Previous $prev.applied.app_router -Current (Read-RegistryValues "$($script:SoftwareKey)\AppRouter")) }
        @{ group = 'graphics_drivers'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.graphics_drivers -Previous $prev.applied.graphics_drivers -Current (Read-RegistryValues $script:GraphicsDriversKey)) }
    )
    foreach ($app in ConvertTo-PairList $tbl.defaults.d3d12_applications) {
        $prevApp = $null
        if ($prev.applied.d3d12_applications -and $prev.applied.d3d12_applications.PSObject.Properties[$app.Name]) { $prevApp = $prev.applied.d3d12_applications.($app.Name) }
        $groups += @{ group = "d3d12:$($app.Name)"; plan = (Get-RegistryDefaultPlan -Defaults $app.Value -Previous $prevApp -Current (Read-RegistryValues "$($script:SoftwareKey)\D3D12\Applications\$($app.Name)")) }
    }
    return (Get-SettingsImpact $groups)
}
# No consent is asked for any more: starting this installer IS the consent for the test mode the
# driver needs and for the BitLocker pause the boot-option change needs. The list stays in the decision event, empty,
# because it is part of the engine contract (interfaces-setup.md A2) and because a future change that really needs an
# answer belongs in it; the gate below still stops a window run that did not give one. What this install changes is
# stated instead: the plan screen says it in words, and the console prints it.
$consents = @()
$restarts = 0
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    $restarts = 2 # driver-set activation, then configuration
    $planInf = Join-Path $package 'payload\kmd\bc250kmd.inf'
    if ((Test-Path -LiteralPath $planInf) -and
        (Get-DriverSetAction $state.driver_set (Get-Sha256 $planInf) ([string](Get-BootIdentity).boot_id)) -eq 'verify-binding') {
        $restarts = 1 # this driver set has already crossed its shutdown boundary
    }
    if ($state.phase -in @('new', 'testsigning-pending') -and -not $script:TestSigningActive) { $restarts++ }
}
$script:PlannedRestarts = $restarts
$firmwareSource = $(if (-not $FirmwareDir) { 'download' } elseif (Test-SamePath $FirmwareDir (Join-Path $package 'firmware')) { 'package-folder' } else { 'folder' })
$notes = @(Get-ChildItem -LiteralPath $package -File -Filter 'RELEASE-NOTES*.md' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
# This package's own compatibility record (compatibility.ps1). Phase 1 only reports it; a rollback admits a package
# only when it verifies (G-RB, Ph 3).
$compat = $null
if ($action.action -ne 'verify' -and $script:Manifest) {
    try { $cv = Test-CompatibilityRecord -PackageRoot $package -CheckedManifest $script:Manifest; $compat = [ordered]@{ ok = $cv.ok; reasons = @($cv.reasons) }; if (-not $cv.ok) { Write-Log "   compatibility record: $($cv.detail)" } } catch { Write-Log "   compatibility record: $($_.Exception.Message)" }
}
if($legacyRollback) {
    $script:RollbackTarget=Get-LegacyRollbackTarget -PackageRoot $package -FirmwareDir $FirmwareDir
    if(-not $early -or -not $early.driver_state -or $early.driver_state.phase -ne 'acknowledged') {
        throw 'legacy rollback requires a completed schema 2 installation'
    }
}
Write-EngineEvent 'decision' ([ordered]@{ action = $action.action; message = $action.message; installed_version = $installedVersion; package_version = $packageVersion; phase = [string]$state.phase
    consents = @($consents); consents_given = [ordered]@{ test_signing = [bool]$AcceptTestSigning; bitlocker = $BitLocker }; restarts = $restarts
    firmware_source = $firmwareSource; firmware_dir = $FirmwareDir; notes = @($notes); secure_boot = $script:SecureBoot; bitlocker = $script:BitLockerState
    compatibility = $compat; system_tuning = [bool]$ApplySystemTuning
    # The two shortcut answers this run would act on (the command line, or the answer an earlier run of the same
    # install saved). The setup window opens its two boxes on them, so the run after a restart and a repair run
    # state the answer the tester gave, not the default (interfaces-setup.md A2).
    start_menu_shortcut = (-not [bool]$NoStartMenuShortcut); desktop_shortcut = (-not [bool]$NoDesktopShortcut)
    wallpaper = (-not [bool]$NoWallpaper)
    # The start-screen picture the box opens on: the stated answer, or the one the installed driver carries, so an
    # upgrade shows the person their own earlier choice and keeps it when they leave the box alone.
    boot_logo = (Get-BootLogoChoice -Stated $bootLogoStated -Current (Read-RegistryValues $script:ParametersKey)['BootLogo']) })
Write-SystemTuningPlan -Selected ([bool]$ApplySystemTuning) -Manifest $script:Manifest
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    $impact = Get-InstallSettingsImpact
    Write-Info ("settings: {0} kept as changed by you, {1} updated to a new default, {2} added, {3} unchanged, {4} from the command line, {5} reopened after the driver closed them, {6} left closed by the driver, {7} kept as you asked for them" -f $impact.summary.kept, $impact.summary.updated, $impact.summary.added, $impact.summary.unchanged, $impact.summary.command, $impact.summary.reopened, $impact.summary.driver_closed, $impact.summary.chosen)
    foreach ($row in @($impact.rows | Where-Object { $_.decision -ne 'same' })) { Write-Log ("   setting {0}\{1}: {2} {3} -> {4}{5}" -f $row.group, $row.name, $row.decision, (Format-RegistryValue $row.current), (Format-RegistryValue $row.value), $(if ($row.closure) { " ($($row.closure))" } else { '' })) }
    foreach ($row in @($impact.rows | Where-Object { $_.decision -eq 'driver-closed' })) { Write-Info "$($row.name) stays as the driver left it: $($row.closure). Remedy: run install.cmd -Repair." }
    Write-EngineEvent 'settings-plan' ([ordered]@{ summary = $impact.summary; rows = @($impact.rows) })
}
# BD-089: the files that the release installed here left, and this package does not install any more. The manifest in
# the install root is that release's own manifest (the installer copies it with the payload), so its files are known by
# name and by SHA256. Read here, before any change, so that the plan of this run already names every removal; the
# files stage below removes them before the new payload is copied. A file whose bytes are not the ones that release
# installed was changed after it, so it stays and is reported. A manifest that does not parse removes nothing.
$script:OrphanRows = @()
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    $installedManifestPath = Join-Path $InstallRoot 'manifest.json'
    $installedManifest = $null
    if (Test-Path -LiteralPath $installedManifestPath -PathType Leaf) {
        try { $installedManifest = Get-Content -LiteralPath $installedManifestPath -Raw | ConvertFrom-Json }
        catch { Write-Warn2 "$installedManifestPath does not parse ($($_.Exception.Message)): the files of the release installed there are not examined, and none of them is removed" }
    }
    if ($installedManifest -and $script:Manifest) {
        try { $script:OrphanRows = @(Resolve-OrphanFilePlan (Get-OrphanFilePlan -PreviousManifest $installedManifest -NewManifest $script:Manifest -InstallRoot $InstallRoot)) }
        catch { Write-Warn2 "the files of the release installed in $InstallRoot were not examined ($($_.Exception.Message)): none of them is removed" }
        # The D3D9 stub of the earlier releases is not a row here: the stub step of the files stage takes it away by
        # the uninstaller's rule (whatever its bytes, unless it was there before the first install of ours).
        $stubPaths = @(Get-LegacyStubPaths | ForEach-Object { $_.path.ToLowerInvariant() })
        $script:OrphanRows = @($script:OrphanRows | Where-Object { $stubPaths -notcontains ([string]$_.path).ToLowerInvariant() })
        $takeAway = @($script:OrphanRows | Where-Object { $_.state -eq 'remove' })
        $changed = @($script:OrphanRows | Where-Object { $_.state -eq 'changed' })
        $gone = @($script:OrphanRows | Where-Object { $_.state -eq 'absent' })
        Write-Info ("files of the installed release $(if ($installedManifest.version) { [string]$installedManifest.version } else { 'of unknown version' }) that $packageVersion does not install: $(@($script:OrphanRows).Count)" +
            " ($($takeAway.Count) to remove, $($changed.Count) changed after that install and kept, $($gone.Count) already gone)")
        foreach ($row in $takeAway) { Write-Info "to remove in the files stage: $($row.path) (installed by $($row.from_version))" }
        foreach ($row in $changed) { Write-Warn2 "kept: $($row.path) was installed by $($row.from_version) and is not in this release, but its bytes are not the ones that release installed. Remove it by hand if you do not want it." }
    }
}
$script:EngineConsents = @($consents)
if ($Plan) {
    Write-Host ''
    Write-Host "Plan: $($action.action); restarts: $restarts; firmware: $firmwareSource. Nothing was changed." -ForegroundColor Green
    if ($action.action -in @('install', 'upgrade', 'repair', 'resume') -and $state.phase -eq 'new' -and -not $script:TestSigningActive) {
        # What the install changes about the way Windows starts, as information: the console says the same as the
        # setup window's plan screen, and neither asks for an answer.
        Write-Host '      Windows test mode is turned on, and "Test Mode" then shows on the desktop. Uninstall turns it off again.' -ForegroundColor White
        Write-Host ('      ' + (Get-BitLockerPlan -State $script:BitLockerState -Requested $BitLocker -Restarts $restarts).message) -ForegroundColor White
    }
    Exit-Engine -Code 0 -Outcome 'planned' -MessageId 'result.planned'
}
# A consent the window did not give still stops a window run before any change. The list is empty today (nothing is
# asked for any more), so this gate passes every install; it stays because the engine contract carries the field and
# because a future change that really needs an answer must not be able to slip past the window.
if ($script:GuiMode -and $consents.Count) {
    $missing = @($consents)
    $script:EngineConsents = $missing
    Exit-Engine -Code 4 -Outcome 'needs-consent' -MessageId 'result.needs-consent' -Detail "missing: $($missing -join ', ')"
}
# ---- the extras: llama.cpp, Ollama, and the models -------------------------------------------------------------
# The questions are asked here, before any change, and the answers are saved with the install inputs: the run after a
# restart starts from RunOnce without a console and takes them from the state. Nothing is downloaded yet; the
# components stage at the end of phase 2 gets the files, after the driver is in place.
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    if ($script:ComponentProblems.Count) {
        Write-Warn2 "the extras of this package cannot be read ($($script:ComponentProblems -join '; ')): none of them is offered. The driver installs as usual."
    }
    # A run that can neither ask nor install an extra does not open the stage at all: the setup window has no control
    # for the extras, and what its command line did not name is not consented (components.ps1, Resolve-*Request).
    # An unbound [string[]] parameter of a script is $null, and @($null) has one element, so the ids are counted
    # after the empty ones are dropped.
    $namedExtras = @(@(@($Components) + @($Models)) | Where-Object { [string]$_ })
    $mayChooseExtras = (-not $script:GuiMode) -or ($namedExtras.Count -gt 0)
    if ($null -ne $script:ComponentsManifest -and $null -eq $installInputs.components -and $mayChooseExtras) {
        Enter-Stage 'extras' 'Extras: llama.cpp, Ollama, models'
        $rows = @(Get-ComponentRows $script:ComponentsManifest)
        $req = Resolve-ComponentRequest -Manifest $script:ComponentsManifest -Requested $Components -NoComponents ([bool]$NoComponents) -CanAsk (-not $script:GuiMode) -DefaultsWithoutAsking $false
        foreach ($u in @($req.unknown)) { Write-Warn2 "-Components names '$u', which this release does not have: it is not installed" }
        $chosen = @()
        if ($req.mode -eq 'ask') { $chosen = @(Read-ComponentChoice -Manifest $script:ComponentsManifest -Rows $rows) }
        else { $chosen = @($req.ids) }
        # A component without a usable pin in this package is never installed, whatever the switch asked for.
        $refused = @($rows | Where-Object { $_.refused -and (@($chosen) -contains $_.id) })
        foreach ($r in $refused) { Write-Warn2 "$([string]$r.component.title) cannot be installed from this package: $($r.refused). It is skipped." }
        $refusedIds = @($refused | ForEach-Object { $_.id })
        $chosen = @($chosen | Where-Object { $refusedIds -notcontains $_ })
        $installInputs.components = @($chosen)
        if (@($chosen).Count) { Write-Info "extras to install: $(@($chosen) -join ', ')" } else { Write-Info 'extras: none' }
        $offered = @()
        foreach ($id2 in @($chosen)) {
            $c2 = Get-ComponentById $script:ComponentsManifest $id2
            if ($null -eq $c2) { continue }
            foreach ($m2 in @($c2.models)) { if (@($offered) -notcontains [string]$m2) { $offered += [string]$m2 } }
        }
        $mreq = Resolve-ModelRequest -Manifest $script:ComponentsManifest -Requested $Models -NoModels ([bool]$NoModels) -CanAsk (-not $script:GuiMode) -ComponentIds @($chosen)
        foreach ($u in @($mreq.unknown)) { Write-Warn2 "-Models names '$u', which this release does not offer: it is skipped" }
        foreach ($u in @($mreq.unusable)) { Write-Warn2 "-Models names '$u', which none of the chosen extras can use: it is skipped" }
        $modelsChosen = @()
        $chosenComponents = @(foreach ($id3 in @($chosen)) { Get-ComponentById $script:ComponentsManifest $id3 })
        if ($mreq.mode -eq 'ask') { $modelsChosen = @(Read-ModelChoice -Manifest $script:ComponentsManifest -Offered $offered -Components $chosenComponents) }
        else { $modelsChosen = @($mreq.ids) }
        $installInputs.models = @($modelsChosen)
        if (@($modelsChosen).Count) {
            $modelsDir = [string]$script:ComponentsManifest.models_dir
            Write-Info "models to download into $modelsDir`: $(@($modelsChosen) -join ', ')"
            foreach ($id4 in @($modelsChosen)) {
                $m4 = Get-ComponentModelById $script:ComponentsManifest $id4
                if ($null -eq $m4) { continue }
                $cost = Get-ModelCostText $m4 @(@($chosenComponents) | Where-Object { (Get-ComponentModelRoute $_) -eq 'gguf' }) `
                                              @(@($chosenComponents) | Where-Object { (Get-ComponentModelRoute $_) -eq 'pull' })
                if ($cost) { Write-Info "    $([string]$m4.title): $cost" }
            }
        } else { Write-Info 'models: none (you can download one later; the documents say how)' }
        # The component zips and the model files may come from two different folders (a prepared folder keeps them
        # in components\ and models\), so each is checked against the folder it is actually read from.
        if ($ComponentDir -or $ModelDir) {
            $bad = @()
            if ($ComponentDir) { $bad += @(Test-ComponentSetDir $script:ComponentsManifest @($chosen) $ComponentDir) }
            if ($ModelDir) { $bad += @(Test-ModelSetDir $script:ComponentsManifest @($modelsChosen) $ModelDir) }
            elseif (@($modelsChosen).Count) { $bad += "no prepared folder holds the models: they are downloaded" }
            $where = @(@($ComponentDir, $ModelDir) | Where-Object { $_ } | Select-Object -Unique) -join ' and '
            if ($bad.Count) { Write-Warn2 "$where`: $($bad -join '; '). What is missing there is downloaded instead." }
        }
    } elseif ($null -ne $script:ComponentsManifest -and $null -ne $installInputs.components) {
        Write-Info "extras from the first run of this install: $(if (@($installInputs.components).Count) { @($installInputs.components) -join ', ' } else { 'none' }); models: $(if (@($installInputs.models).Count) { @($installInputs.models) -join ', ' } else { 'none' })"
    }
}
Set-CancelAvailable $true 'before-changes'
Invoke-CancelPoint 'before-changes'

switch ($action.action) {
    'already' { Write-Host $action.message -ForegroundColor White; Exit-Engine -Code 0 -Outcome 'already' -MessageId 'result.already' }
    'verify' { Write-Host "[dry run] $packageVersion is installed and waits for its restart: a real run verifies it (verify.cmd)." -ForegroundColor DarkYellow; Exit-Engine -Code 0 -Outcome 'completed' -MessageId 'result.dry-run-verify' }
    { $_ -in @('upgrade', 'repair') } {
        # Phase 2 again over the installed release: unchanged files are kept, the rest replaced, registry values,
        # task and Release\Version rewritten, RunOnce verify armed again. Phase 1 (test signing) is done already.
        Write-Step $action.message
        Write-Host "   $($action.message)" -ForegroundColor White
        Set-StateValue $state 'previous_package_version' $installedVersion
        Set-StateValue $state 'package_version' $packageVersion
    }
    'resume' { Write-Step $action.message }
    'install' {
        $was = Set-InstallPackage $state $packageVersion
        if ($was) { Write-Info "this package ($packageVersion) takes over the unfinished installation of $was (phase $($state.phase)); test signing is finished first as before" }
    }
}
# Saved with the state before any step that can end in a restart, for an install, an upgrade and a repair alike.
Save-InstallInputs $state $installInputs
$inputText = @()
if ($FirmwareDir) { $inputText += "-FirmwareDir $FirmwareDir" }
foreach ($k in @($commandLineParameters.Keys | Sort-Object)) { $inputText += "-$k $($commandLineParameters[$k])" }
$inputText += @($installInputs.switches | ForEach-Object { "-$_" })
if ($inputText.Count) { Write-Info "options of this install, kept until it completes: $($inputText -join ' ')" }

# ---- phase 1: test signing -------------------------------------------------------------------------------------
function New-RestorePoint {
    Invoke-Change 'create a System Restore point "amdgpu-wddm before install" (only if System Protection is on)' {
        try { Checkpoint-Computer -Description 'amdgpu-wddm before install' -RestorePointType MODIFY_SETTINGS -ErrorAction Stop; Write-Info 'restore point created' }
        catch { Write-Warn2 "no restore point: $($_.Exception.Message)" }
    } | Out-Null
    Set-StateValue $state 'restore_point_attempted' (-not $script:DryRunMode)
}

if ($state.phase -eq 'new') {
    Enter-Stage 'test-signing' 'Phase 1: save state, restore point, test signing'
    Set-StateValue $state 'previous_service' $script:DeviceService
    Set-StateValue $state 'previous_testsigning' $script:TestSigningConfigured
    Set-StateValue $state 'install_root' $InstallRoot
    Set-StateValue $state 'package_version' $script:Manifest.version
    New-RestorePoint
    if ($script:TestSigningActive) {
        Write-Info 'test signing is already active in this boot: no restart needed before the install'
        Set-StateValue $state 'testsigning_set_by_installer' $false
        Save-Phase 'testsigning-active'
    } else {
        # No question here: a person who started this installer wants the driver, and the driver
        # needs the test mode. The two lines say what happens, and it happens. -AcceptTestSigning stays accepted so
        # that every script that passes it still runs; it changes nothing now.
        Write-Host ''
        Write-Host '   Windows is switched to TEST MODE, so that it can load this driver. Test mode lets Windows load' -ForegroundColor White
        Write-Host '   drivers signed with a test certificate, and shows "Test Mode" on the desktop. Uninstall turns it off.' -ForegroundColor White
        Write-Info ('test signing: turning it on' + $(if ($AcceptTestSigning) { ' (-AcceptTestSigning was given; it is not needed any more)' } else { '' }))
        # Recheck before the boot change; preparation may have taken long enough for protection to change.
        $script:BitLockerState = Get-BitLockerState
        $blPlan = Get-BitLockerPlan -State $script:BitLockerState -Requested $BitLocker -Restarts $(if ($script:PlannedRestarts) { $script:PlannedRestarts } else { 2 })
        Write-Info $blPlan.message
        if ($blPlan.action -eq 'suspend') {
            Invoke-Change "suspend BitLocker on $($env:SystemDrive) for $($blPlan.reboots) restart(s)" {
                Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount $blPlan.reboots -ErrorAction Stop | Out-Null
                # A suspension that did not take hold is the one case that still stops the install: the boot options
                # are not touched, so Windows cannot ask for a recovery key the person may not have.
                if (Test-BitLockerConsentRequired (Get-BitLockerState)) { throw 'BitLocker suspension could not be verified; boot options were not changed.' }
            } | Out-Null
        }
        if ($blPlan.action -ne 'none') { Set-StateValue $state 'bitlocker' $(if ($blPlan.action -eq 'suspend') { 'Suspend' } elseif ($blPlan.action -eq 'have-key') { 'HaveKey' } else { 'unknown' }) }
        # The continuation of this install runs after the restart from the staged closure. Its after-staging stop point
        # is the last one of phase 1: test signing, the state and RunOnce follow without a stop.
        Initialize-Closure
        Set-CancelAvailable $false 'test-signing'
        Invoke-Change "bcdedit /set '{current}' testsigning on" {
            $n = Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'on')
            if ($n.code -ne 0) { throw "bcdedit failed: $($n.text)" }
        } | Out-Null
        Set-StateValue $state 'testsigning_set_by_installer' $true
        Save-Phase 'testsigning-pending'
        Set-ResumeAtLogon 'continue'
        Request-Restart 'test signing takes effect at the next start.' 'restart.test-signing'
        if (-not $script:DryRunMode) { Exit-Engine -Code 0 -Outcome 'restart-required' -MessageId 'result.restart-test-signing' }
        Write-Info '(dry run: phase 2 is shown as it would run after the restart)'
        Save-Phase 'testsigning-active'
        # The run after the restart is a new one that offers Cancel again until its own last stop point.
        Set-CancelAvailable $true 'phase-2'
    }
}
if ($state.phase -eq 'testsigning-pending') {
    if (-not $script:TestSigningActive) {
        # This run's package and inputs are kept, and the run after the restart is this package's: a newer package
        # that took over an older one's phase 1 continues as itself, from its own closure.
        if (-not $script:DryRunMode) { Save-InstallState $state }
        Initialize-Closure
        Set-CancelAvailable $false 'continuation'
        Set-ResumeAtLogon 'continue'
        Write-Host 'Test signing is set but not active yet: restart the computer. The installer continues after you log on again (if it does not, run install.cmd again).' -ForegroundColor Yellow
        if ($script:SecureBoot -eq 'on') { Write-Host 'Secure Boot is on, so Windows ignores test signing. Turn it off in the BIOS setup.' -ForegroundColor Yellow }
        $script:EngineRestart = [ordered]@{ required = $true; reason_id = 'restart.test-signing'; still_pending = $true; continuation = $script:Continuation }
        Exit-Engine -Code 5 -Outcome 'restart-required' -MessageId $(if ($script:SecureBoot -eq 'on') { 'result.testsigning-secureboot' } else { 'result.testsigning-not-active' })
    }
    Save-Phase 'testsigning-active'
}

# ---- phase 2: install ------------------------------------------------------------------------------------------
Enter-Stage 'install' 'Phase 2: install'
if (-not $script:DryRunMode -and -not $script:TestSigningActive) {
    Write-Fail 'test signing is not active'
    Exit-Engine -Code 5 -Outcome 'failed' -MessageId 'result.testsigning-not-active' -Detail 'phase 2 needs test signing in the running boot'
}
# The Microsoft Visual C++ runtime, before our own files go on the PC: our control application, the CLI and the
# user-mode drivers need it. Microsoft's own installer travels
# in the package; it runs only when the runtime is missing or older than 14.44, and only after its SHA-256 and its
# Microsoft signature are checked here again, so a changed copy never runs. A code that asks for a restart is folded
# into the restart this installation makes at the end of phase 2.
$vcNow = Get-VcRuntimeState
if (-not $vcNow.install) { Write-Info "Microsoft Visual C++ runtime: $($vcNow.detail)" }
else {
    $vcExe = Join-Path $package $script:VcRuntimePackagePath
    $vcFile = Test-VcRuntimeInstaller -Path $vcExe
    if (-not $vcFile.ok) { throw "the Microsoft Visual C++ runtime has to be installed, and $($vcFile.problem). Nothing of the driver was installed. Download the release again." }
    Write-Info "Microsoft Visual C++ runtime: $($vcNow.detail)"
    $vcRun = Invoke-Change "install the Microsoft Visual C++ 2015-2022 Redistributable (x64) $($script:VcRuntimeVersion) from $($script:VcRuntimePackagePath) ($($script:VcRuntimeArguments -join ' '); SHA256 $($vcFile.sha256.Substring(0, 16)) and the Microsoft signature checked)" {
        Invoke-VcRuntimeInstall -Path $vcExe
    }
    if ($null -ne $vcRun) {
        Write-Log ($vcRun.text)
        if (-not $vcRun.ok) { throw $vcRun.message }
        Write-Info $vcRun.message
        $vcAfter = Get-VcRuntimeState
        # Code 1638 says a same or newer runtime is registered, so Microsoft's installer installed nothing. When a
        # file of it is missing from System32 all the same, the registration is there and its files are not: the same
        # program repairs that, and only a repair that does not help ends the installation.
        if ($vcAfter.install -and $vcRun.code -eq 1638) {
            Write-Info "the Microsoft Visual C++ runtime is registered on this PC and $($vcAfter.detail): the installer repairs it"
            $vcRepair = Invoke-Change "repair the Microsoft Visual C++ 2015-2022 Redistributable (x64) from $($script:VcRuntimePackagePath) ($($script:VcRuntimeRepairArguments -join ' '))" {
                Invoke-VcRuntimeInstall -Path $vcExe -Arguments $script:VcRuntimeRepairArguments
            }
            if ($null -ne $vcRepair) {
                Write-Log ($vcRepair.text)
                if (-not $vcRepair.ok) { throw $vcRepair.message }
                $vcAfter = Get-VcRuntimeState
            }
        }
        if ($vcAfter.install -and -not $vcRun.restart) { throw "Microsoft's installer reported success (code $($vcRun.code)), but the runtime is still not in place: $($vcAfter.detail). Repair the Microsoft Visual C++ 2015-2022 Redistributable (x64) in Windows Settings, Apps, Installed apps (or install it again from Microsoft), then run this installer again." }
        Write-Info "Microsoft Visual C++ runtime after the step: $($vcAfter.detail)"
    }
}
# The Vulkan loader (common.ps1, "the Vulkan loader"): LunarG's installer runs only when System32\vulkan-1.dll or
# SysWOW64\vulkan-1.dll is missing or older than the version the package carries, and only after its SHA-256 and its
# LunarG signature are checked. A loader of the same or a newer version stays as it is. The driver works without a
# loader for Direct3D and the desktop, so a loader that cannot be installed is a warning with its remedy, and verify
# reports it; it does not stop the installation (Invoke-VulkanLoaderStep catches every error of the step).
# The record goes into the state object only. state.json is not written here: at this point the object already
# holds the new package_version with the phase of phase 1, and a stop before phase 2 marks itself (InPhase2) would
# leave a state.json that reads as this version installed. The next Save-Phase writes the record with its phase.
$vkRecord = Invoke-VulkanLoaderStep -PackageRoot $package
if ($null -ne $vkRecord) { Set-StateValue $state 'vulkan_loader' $vkRecord }
Enter-Stage 'firmware' 'GPU firmware'
# Every step below can run again over its own result: files are compared by SHA256 and a file in use is replaced
# by rename, registry values and the task are overwritten, a staged driver set is checked after its restart.
# The firmware comes first: every install downloads it again (or takes it from -FirmwareDir) into a staging folder and
# checks each SHA256 before phase 2 changes anything, so a failed download leaves the computer and the phase as they
# were.
$fw = $script:Manifest.firmware
$driverStoreFirmware = Test-DriverStoreFirmware $fw
$driverStoreUmd = Test-DriverStoreUmdPackage $script:Manifest
$fwStaging = Join-Path $script:StateDir 'firmware-staging'
$fwFrom = $(if ($FirmwareDir) { "from $FirmwareDir" } else { "from linux-firmware $($fw.commit)" })
if ($script:DryRunMode) {
    foreach ($f in @($fw.files)) {
        Write-Info ('firmware {0}  SHA256 {1}' -f $f.name, ([string]$f.sha256).ToUpperInvariant())
        if ($FirmwareDir) { Write-Info "    $(Join-Path $FirmwareDir $f.name)" } else { foreach ($u in Get-FirmwareUrls $fw $f) { Write-Info "    $u" } }
    }
}
$fwStaged = Invoke-Change "get the $(@($fw.files).Count) GPU firmware files $fwFrom into $fwStaging and check each SHA256 (a file with another SHA256 stops the install before any change)" {
    Get-FirmwareStaged -Firmware $fw -Staging $fwStaging -FromDir $FirmwareDir
}
$hydrated = $null
if ($driverStoreFirmware -and -not $script:DryRunMode) {
    $hydrated = New-HydratedDriverPackage -Manifest $script:Manifest -PackageRoot $package -FirmwareDir $fwStaging -Parent $script:StateDir
}
try {
# The closure before the driver package, which can end this run in a restart (driver-pending-restart).
Initialize-Closure
# The last safe point: from here to the end of phase 2 a cancel would leave a half-installed driver.
Invoke-CancelPoint 'before-driver-install'
Set-CancelAvailable $false 'driver-install'
$script:InPhase2 = $true
if($legacyRollback) {
    $script:RollbackWorker=Join-Path $script:BridgeClosure 'payload\tools\amdgpu-state-installer.exe'
    Invoke-Change 'freeze current settings, disable the device persistently, capture final state and protect the rollback package identity' {
        [void](Start-LegacyRollback -State $state -Target $script:RollbackTarget -Device $script:Device.DeviceID `
            -Worker $script:RollbackWorker -SourceInf (Get-DeviceDriverPackage -InstanceId $script:Device.DeviceID) -Save {param($s) Save-InstallState $s})
    } | Out-Null
}
if ($state.phase -notin @('testsigning-active')) { Write-Info "continuing an earlier run (phase $($state.phase)): finished steps are skipped" }
Enter-Stage 'files' 'Files'

$cer = Join-Path $package 'payload\cert\amdgpu-wddm-release.cer'
$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
Set-StateValue $state 'cert_thumbprint' $cert.Thumbprint
# Root is required: without it the test signature is not trusted at all. TrustedPublisher only spares the user the
# Windows prompt that asks whether to trust the publisher when the driver package is installed, so a refusal there,
# after Root holds the certificate, is a warning and the install goes on (BD-125).
Invoke-Change "add the release test certificate $($cert.Thumbprint) to LocalMachine Root and TrustedPublisher" {
    $stores = @('Root', 'TrustedPublisher')
    for ($i = 0; $i -lt $stores.Count; $i++) {
        $added = Add-CertificateToStore -Store $stores[$i] -Certificate $cer -Thumbprint $cert.Thumbprint
        if ($added.ok) { if ($added.via -ne 'Import-Certificate') { Write-Info $added.detail }; continue }
        $err = New-CertificateStoreError -Store $stores[$i] -Certificate $cer -Exception $added.exception `
            -Done @($stores | Select-Object -First $i) -Remaining @($stores | Select-Object -Skip ($i + 1)) -Tried $added.detail
        if ($stores[$i] -eq 'TrustedPublisher') {
            Write-Warn2 "$($added.detail). The install goes on: LocalMachine\Root holds the certificate. Windows may ask once whether to trust the publisher of this driver when it is installed; answer Install."
            Write-Log "   $($err.Message)"
            continue
        }
        throw $err
    }
} | Out-Null

# Files. Each payload directory goes to the same name under the install root; wow64 holds the x86 builds (BD-064).
# One decision, in one tested function (Get-InstallPayloadDirs, BD-183): a one-element list must stay a list.
$dirs = Get-InstallPayloadDirs -DriverStoreUmd $driverStoreUmd `
    -HasSystemTuning (Test-Path -LiteralPath (Join-Path $package 'payload\system-tuning')) `
    -HasControl (-not $NoControlApp -and (Test-Path -LiteralPath (Join-Path $package 'payload\control')))
# The H.264 encoder Media Foundation transform (M15.11, driver/umd/mft-h264/INSTALL.md): the release decides whether
# it is installed at all (manifest.json "mft_h264"). The decision is taken here, before the first change of this
# stage, and the settings stage below writes or removes its registry keys by the same answer.
$mft = Get-MftReleaseSwitch -Manifest $script:Manifest -PackageRoot $package
$mftDll = Join-Path $InstallRoot $mft.install_path
$mftKeys = @(Get-MftRegistrationKeysPresent -ClassesKey $script:ClassesKey)
$mftAction = Get-MftAction -Switch $mft -PresentKeys $mftKeys -DirPresent (Test-Path -LiteralPath (Split-Path $mftDll))
if ($mft.register) {
    if (-not $mft.present) { throw "manifest.json registers the H.264 encoder MFT, but this package has no $($mft.package_path)" }
    if (-not $driverStoreUmd) { $dirs += $mft.payload_dir }
}
# The OpenGL ICD (BD-076, GitHub issue #8): the release decides whether it is registered at all and for which
# architectures (manifest.json "opengl_icd" with its "architectures" list), and the decision is taken here, before
# the first change of this stage, so that the files of this stage and the registry values of the settings stage below
# follow one answer. A release that registers an architecture has to carry that architecture's file: a registration
# naming a file that is not there leaves an OpenGL program of that architecture with no driver at all, instead of the
# software renderer Windows gives it without a registration. The x86 ICD rides in the wow64 tree, which goes in
# anyway, so only the 64-bit directory ever joins the list of payload directories.
# This release lists x64 alone (common.ps1 holds the lab measurement behind that): the 32-bit ICD loaded but had no
# 32-bit Vulkan loader to run zink on, so a 32-bit program was better off with the renderer of Windows itself. The
# loader step of phase 2 now brings that loader; the list gets "x86" after a lab pass of the x86 ICD on it.
$gl = Get-OpenGlReleaseSwitch -Manifest $script:Manifest -PackageRoot $package
$glDir = Split-Path (Join-Path $InstallRoot $gl.install_path)         # <install root>\opengl, the x64 ICD
$glDirWow = Split-Path (Join-Path $InstallRoot $gl.install_path_wow)  # <install root>\wow64\opengl, the x86 ICD
if ($gl.register) {
    $glMissing = @(@(Get-OpenGlArchFiles -Switch $gl) | Where-Object { -not $_.present } | ForEach-Object { $_.package_path })
    if ($glMissing.Count) { throw "manifest.json registers the OpenGL ICD for $(@($gl.architectures) -join ' and '), but this package has no $($glMissing -join ' and ')" }
    if ($gl.register_x64 -and -not $driverStoreUmd) { $dirs += $gl.payload_dir }
}
# BD-089: what the release installed here installed and this one does not, before the new payload goes in. The rows
# come from the plan that this run printed before any change (above); each removal names the file and the release that
# installed it. The SHA256 is read again here, so a file that somebody changed between the plan and this step stays.
# A file in use goes at the next restart (Remove-PathOrSchedule).
$deferredUmdOrphans = @()
if ($driverStoreUmd -and $state.PSObject.Properties['umd_retired_files']) {
    $deferredUmdOrphans = @(Get-RetiredUmdFilePlan -InstallRoot $InstallRoot -Rows @($state.umd_retired_files))
}
foreach ($row in @($script:OrphanRows | Where-Object { $_.state -eq 'remove' })) {
    # The old router still needs its old absolute overrides until the new INF
    # has bound. Do not strand it across the first BD-152 restart.
    $prefix = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\') + '\'
    if ($driverStoreUmd -and $row.path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and
        $row.path.Substring($prefix.Length) -match '^(desktop|d3d11|d3d12|vulkan|wow64|opengl|mft)\\') {
        $deferredUmdOrphans += $row
        continue
    }
    Invoke-Change "remove $($row.path): installed by $($row.from_version), not in $packageVersion" {
        if (-not (Test-Path -LiteralPath $row.path -PathType Leaf)) { Write-Info "already gone: $($row.path)"; return }
        $now = Get-Sha256 $row.path
        if ($now -ne $row.sha256) { Write-Warn2 "kept: $($row.path) changed after the plan of this run (SHA256 $($now.Substring(0, 16)))"; return }
        Remove-PathOrSchedule $row.path
    } | Out-Null
}
# BD-146: the old uninstall can still have deletes queued for files about to become current again. Cancel only
if ($driverStoreUmd) {
    # The new manifest is copied before the first restart. Preserve the exact
    # old file plan first, so the continuation cannot lose its orphan list.
    $deferredUmdOrphans = @(Get-RetiredUmdFilePlan -InstallRoot $InstallRoot -Rows $deferredUmdOrphans)
    Set-StateValue $state 'umd_retired_files' $deferredUmdOrphans
    Save-InstallState $state
}
# those exact destinations before Copy-FileSafe can keep a same-hash file. No unrelated rename/delete is changed.
Invoke-Change 'keep the files of this installation from being removed at the next restart' {
    $legacyFirmwareCopies = if ($driverStoreFirmware) { @() } else { @($fwStaged) }
    $destinations = @(Get-InstallCopyDestinations -Package $package -InstallRoot $InstallRoot -PayloadDirs $dirs -FirmwareFiles $legacyFirmwareCopies)
    $repaired = Repair-PendingFileRemovals -Destinations $destinations
    if ($repaired) { Write-EngineEvent 'pending-removals-cancelled' ([ordered]@{ count = $repaired }) }
} | Out-Null
foreach ($d in $dirs) {
    $src = Join-Path $package "payload\$d"
    Invoke-Change "copy payload\$d -> $InstallRoot\$d (same SHA256: kept; in use: replaced by rename)" {
        Copy-TreeSafe -Source $src -Destination (Join-Path $InstallRoot $d)
    } | Out-Null
}
# The HIP runtime (experimental) works without a step of the tester's: its folder goes at the end of the machine PATH,
# once (common.ps1, Get-HipPathEntry). A package without tools\hip leaves PATH alone.
if (Test-Path -LiteralPath (Join-Path $package 'payload\tools\hip')) {
    $hipEntry = Get-HipPathEntry $InstallRoot
    if (Test-PathEntry (Get-MachinePath).value $hipEntry) { Write-Info "the machine PATH holds $hipEntry already: kept" }
    else {
        Invoke-Change "append $hipEntry to the machine PATH (the HIP runtime of this release; an amdhip64.dll in System32 or earlier on PATH still wins)" {
            $p = Get-MachinePath
            Set-MachinePath (Add-PathEntry $p.value $hipEntry) $p.kind
        } | Out-Null
    }
    Set-StateValue $state 'hip_path_entry' $hipEntry
}
# A release that does not register the encoder takes the registration and the files of an earlier install away again. A
# repair with the switch off is therefore a rollback. The keys go first, in this stage, and the file after them, as
# uninstall.ps1 does it: a step between the two (driver staging, the firmware, a restart, a failure of any of them) would
# otherwise leave a COM registration that names a DLL which is gone, and every Media Foundation client that enumerates
# hardware or asynchronous H.264 encoders would get an activation object that cannot be created.
if ($mftAction -eq 'rollback' -and $mftKeys.Count) {
    Invoke-Change "remove the H.264 encoder MFT registration of an earlier install ($($mftKeys -join ', ')): this release does not register it" {
        [void](Remove-MftRegistration -ClassesKey $script:ClassesKey)
    } | Out-Null
}
if ($mftAction -eq 'rollback' -and (Test-Path -LiteralPath (Split-Path $mftDll))) {
    Invoke-Change "remove $(Split-Path $mftDll) (the H.264 encoder MFT of an earlier install; this release does not register it)" {
        Remove-PathOrSchedule (Split-Path $mftDll)
    } | Out-Null
}
Invoke-Change "copy the installer's own scripts to $InstallRoot\installer (uninstall works without the package folder)" {
    Copy-TreeSafe -Source $here -Destination (Join-Path $InstallRoot 'installer')
    foreach ($f in 'manifest.json', 'uninstall.cmd', 'verify.cmd') { [void](Copy-FileSafe -Source (Join-Path $package $f) -Destination (Join-Path $InstallRoot $f)) }
} | Out-Null
Invoke-Change "copy licenses\ and THIRD-PARTY.md -> $InstallRoot\licenses (the licence texts of every installed component)" {
    Copy-TreeSafe -Source (Join-Path $package 'licenses') -Destination (Join-Path $InstallRoot 'licenses')
    [void](Copy-FileSafe -Source (Join-Path $package 'THIRD-PARTY.md') -Destination (Join-Path $InstallRoot 'licenses\THIRD-PARTY.md'))
} | Out-Null
# Facts about the computer before the install are recorded once and saved before the step that changes them. The stub
# flags come from the first install of ours: a stub that was there before it is not ours and stays.
foreach ($s in @(Get-LegacyStubPaths)) { [void](Set-StateValueOnce $state $s.flag (Test-Path -LiteralPath $s.path)) }
if (-not $driverStoreFirmware) {
$bc250Dir = Split-Path -Parent $script:FirmwareInstallDir
[void](Set-StateValueOnce $state 'bc250_dir_existed' (Test-Path -LiteralPath $bc250Dir))
$fwExisted = Set-StateValueOnce $state 'firmware_dir_existed' (Test-Path -LiteralPath $script:FirmwareInstallDir)
if (-not $script:DryRunMode) { Save-InstallState $state }
}
# D3D9 goes through D3D9On12 (the D3D9 slot holds '<>', below): no stub is installed. The releases up to
# 0.7.213.102-tester.17 installed bc250umd.dll in System32 and SysWOW64; it goes here, as uninstall.ps1 takes it.
foreach ($s in @(Get-LegacyStubPaths)) {
    if ($state.($s.flag)) { Write-Info "$($s.path) was there before the first install of ours: kept (no slot names it)"; continue }
    if (-not (Test-Path -LiteralPath $s.path)) { continue }
    Invoke-Change "remove $($s.path) (the D3D9 stub of an earlier release; D3D9 now goes through D3D9On12; in use: removed at the restart)" {
        Remove-PathOrSchedule $s.path
    } | Out-Null
}
if (-not $driverStoreFirmware) {
$fwAcl = $(if ($fwExisted) { 'its existing access rights are kept' } else { 'created writable by administrators only' })
Invoke-Change "copy the checked GPU firmware files (linux-firmware cyan_skillfish2_*.bin, LICENSE.amdgpu) from $fwStaging -> $($script:FirmwareInstallDir); $fwAcl; $bc250Dir itself is not changed" {
    # An existing C:\BC250 may hold other files: its access rights stay as they are. Only the firmware folder that
    # this installer creates gets its own rights: C:\ lets every user create and change folders, and the KMD loads
    # this firmware, so only administrators may change it.
    if (-not (Test-Path -LiteralPath $bc250Dir)) { [void][IO.Directory]::CreateDirectory($bc250Dir) }
    if (-not $fwExisted) {
        [void][IO.Directory]::CreateDirectory($script:FirmwareInstallDir)
        $n = Invoke-Native icacls.exe @($script:FirmwareInstallDir, '/inheritance:r', '/grant:r', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX')
        if ($n.code -ne 0) { throw "icacls $($script:FirmwareInstallDir) failed: $($n.text)" }
    }
    foreach ($p in @($fwStaged)) {
        Write-Info ('{0}: {1}' -f (Split-Path $p -Leaf), (Copy-FileSafe -Source $p -Destination (Join-Path $script:FirmwareInstallDir (Split-Path $p -Leaf))))
    }
    # Checked again where the KMD reads them: the staging folder lives under %ProgramData%, which is not admin-only.
    foreach ($f in @($fw.files)) {
        $h = Get-Sha256 (Join-Path $script:FirmwareInstallDir $f.name)
        if ($h -ne ([string]$f.sha256).ToUpperInvariant()) { throw "firmware: $($script:FirmwareInstallDir)\$($f.name) has SHA256 $h after the copy, expected $($f.sha256)" }
    }
    # The repair set keeps the firmware it was installed with, so that a repair needs no network (WU-051).
    if ((Test-FirmwareFolder $fw (Join-Path $script:Closure 'firmware')).Count) { [void](Save-ClosureFirmware -Firmware $fw -Closure $script:Closure -FromFiles @($fwStaged)) }
    Remove-Item -LiteralPath $fwStaging -Recurse -Force -ErrorAction SilentlyContinue
} | Out-Null
} else {
    Invoke-Change 'keep the pinned firmware with this repair set; Driver Store owns the installed copies' {
        [void](Save-ClosureFirmware -Firmware $fw -Closure $script:Closure -FromFiles @($fwStaged))
        Set-StateValue $state 'firmware_delivery' 'driver-store-v1'
    } | Out-Null
}
Save-Phase 'files-copied'
Enter-Stage 'driver' 'Driver package'

# Typed migration preserves explicit absence, types and bounded bytes. The early
# snapshot and final old-driver stop are merged by the first new KMD. Factory
# defaults are in Parameters\DefaultsV2 and never overwrite the legacy input.
$stateWorker = Join-Path $InstallRoot 'tools\amdgpu-state-installer.exe'
if($legacyRollback){$stateWorker=$script:RollbackWorker}

# BD-152 / BD-060: INSTALL_AS_SET stages now and binds at the next shutdown. A saved receipt keeps a resumed
# install from staging forever; a different, known boot permits the exact image checks below (BD-150).
# No Reboot directive, live GPU rebind, or DWM restart. The KMD's session markers (BD-059) remain under its
# ownership.
$infFile = Join-Path $package 'payload\kmd\bc250kmd.inf'
# Observations around the driver package, in the log and in the state (dwm_observations, the last 12 of every run of
# this install): the session's DWM instances (process ID and creation time) before it, right after it and at the end
# of phase 2, with the boot they were made in. The device's own outcome after it is a separate field: a device-loss
# event and a DWM process restart are two different things. Read-only, in a dry run too, and never a reason to stop.
$dwmSession = 0; $dwmBefore = @()
$dwmBoot = $null
try { $dwmBoot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o') } catch { }
function Add-DwmObservation([string]$Stage, $Instances, [string]$Device = $null) {
    $e = [ordered]@{ stage = $Stage; utc = [DateTime]::UtcNow.ToString('o'); boot_utc = $dwmBoot; session = $dwmSession
        dwm = @(@($Instances) | ForEach-Object { [ordered]@{ pid = $_.pid; created_utc = ([datetime]$_.created_utc).ToString('o') } }) }
    if ($Stage -ne 'before the driver package') { $e['dwm_change'] = Compare-DwmReadings $dwmBefore $Instances }
    if ($PSBoundParameters.ContainsKey('Device')) { $e['device'] = $Device }
    $all = @(@($state.dwm_observations) | Where-Object { $_ }) + @([pscustomobject]$e)
    Set-StateValue $state 'dwm_observations' @($all | Select-Object -Last 12)
}
try { $dwmSession = Get-DesktopSessionId; if ($dwmSession) { $dwmBefore = Get-SessionDwm $dwmSession } } catch { Write-Log "   DWM reading: $($_.Exception.Message)" }
Write-Info "DWM of session $dwmSession before the driver package: $(if (@($dwmBefore).Count) { (@($dwmBefore) | ForEach-Object { Format-DwmInstance $_ }) -join ', ' } else { 'none' })"
if ($dwmSession) { Add-DwmObservation 'before the driver package' $dwmBefore }
if (Test-InfHasReboot ([IO.File]::ReadAllLines($infFile))) { throw 'display INF contains the unsupported Reboot directive (BD-152)' }
$driverSetBoot = [string](Get-BootIdentity).boot_id
$driverSetSha = Get-Sha256 $infFile
$driverSetAction = Get-DriverSetAction $state.driver_set $driverSetSha $driverSetBoot
if ($driverSetAction -in @('unknown-boot', 'invalid-receipt') -and -not $script:DryRunMode) {
    throw "cannot establish the driver set restart boundary: $driverSetAction"
}
$pnp = $null
if ($driverSetAction -eq 'stage' -or $script:DryRunMode) {
    if($legacyRollback) {
        Set-ResumeAtLogon 'continue'
        Invoke-Change 'remove superseded driver ranks while the captured device remains disabled' {
            Remove-LegacyRollbackDriverSets -State $state -Worker $stateWorker
        } | Out-Null
    }
    if(-not $legacyRollback) {
    Invoke-Change 'freeze state writers and publish the protected state handoff before INF staging' {
        # BD-227: the preflight refused this already; read again here, because a freeze for a set that Windows does not
        # bind leaves the GPU on a KMD that does not start.
        $rank = Test-DriverSetRank -InstallState $state
        if ($rank -and -not $rank.ok) { throw "driver package rank: $($rank.detail)" }
        Save-DriverSettingsComparison -State $state -Worker $stateWorker -Device $script:Device.DeviceID `
            -InfSha256 $driverSetSha -Save {param($s) Save-InstallState $s}
        [void](Start-DriverStateTransaction -State $state -Worker $stateWorker -Product (Get-ProductPackageQuad $script:Manifest.version) `
            -Device $script:Device.DeviceID -InfSha256 $driverSetSha -Save {param($s) Save-InstallState $s})
    } | Out-Null
    }
    $pnp = Invoke-Change 'DiInstallDriverW INSTALL_AS_SET payload\kmd (the GPU changes to it at the next restart)' {
        $stageInf = $infFile
        if ($driverStoreFirmware) {
            if (-not $hydrated) { throw 'firmware package was not hydrated' }
            Assert-DriverCatalogSigner -Directory $hydrated.DirectoryPath -Certificate $cer
            $stageInf = Join-Path $hydrated.DirectoryPath 'bc250kmd.inf'
        }
        $o = Invoke-DriverSetStage -InfFile $stageInf
        Set-StateValue $state 'driver_set' ([pscustomobject]@{ method = $o.method; inf_sha256 = $driverSetSha
            boot_id = $driverSetBoot; staged = $true; api_need_reboot = $o.api_need_reboot })
        Save-Phase 'driver-pending-restart'
        return $o
    }
}
} finally { if ($hydrated) { $hydrated.Dispose() } }
$instance = $null
if ($script:Device) { $instance = $script:Device.DeviceID }
# Right after the driver package, before any branch: the observation covers the restart-pending case too.
if ($dwmSession) {
    try {
        $dwmAfter = Get-SessionDwm $dwmSession
        $devNow = $null
        if ($instance) { $devNow = Get-PnpDevice -InstanceId $instance -ErrorAction SilentlyContinue }
        $devText = $(if ($pnp) { "$($pnp.text); device status $(if ($devNow) { $devNow.Status } else { 'unknown' })" }
            elseif ($script:DryRunMode) { 'not run (dry run)' } else { "saved driver set: $driverSetAction; device status $(if ($devNow) { $devNow.Status } else { 'unknown' })" })
        Add-DwmObservation 'after the driver package' $dwmAfter $devText
        Write-Info "after the driver package: device: $devText; DWM: $(Compare-DwmReadings $dwmBefore $dwmAfter)"
    } catch { Write-Log "   DWM reading: $($_.Exception.Message)" }
}
$classKey = $null
if (-not $script:DryRunMode) {
    if ($driverSetAction -ne 'verify-binding') {
        Save-Phase 'driver-pending-restart'
        Set-ResumeAtLogon 'continue'
        Request-Restart 'the staged GPU driver becomes available at shutdown. Its binding is checked after the next logon.' 'restart.driver-package'
        Exit-Engine -Code 0 -Outcome 'restart-required' -MessageId 'result.restart-driver'
    }
    # BD-150: after the recorded restart, check the service and store images against this package. These are disk
    # checks; the final verify step separately checks the loaded image. An API success alone proves neither.
    $script:CurrentStep = "check the driver package binding after the recorded restart"
    $kmdImage = Get-KmdImageDecision -WantSha (Get-Sha256 (Join-Path $package 'payload\kmd\bc250kmd.sys')) -Store (Read-KmdStoreImages) -Service (Read-ServiceImage $script:ServiceName)
    if (-not $kmdImage.ok) { throw $kmdImage.detail }
    Write-Info "driver image: $($kmdImage.detail)"
    $svc = Get-DeviceServiceName -InstanceId $instance
    $script:CurrentStep = 'check that the GPU is bound to the new driver package'
    if ($svc -ne $script:ServiceName) { throw "the GPU is on '$svc' after the driver set restart, not on $($script:ServiceName)" }
    $classKey = Get-DeviceDriverKey -InstanceId $instance
    if (-not $classKey) { throw 'no software key for the GPU' }
    Set-StateValue $state 'class_key' $classKey
    if($legacyRollback) {
        Complete-LegacyRollback -State $state -Worker $stateWorker -Operation restore -Save {param($s) Save-InstallState $s}
    } else {
        Complete-DriverStateTransaction -State $state -Worker $stateWorker -Product (Get-ProductPackageQuad $script:Manifest.version) `
            -Device $instance -InfSha256 $driverSetSha -Save {param($s) Save-InstallState $s}
    }
} else { $classKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$($script:DisplayClassGuid)\<device's key after install>" }
Save-Phase 'driver-installed'
Enter-Stage 'settings' 'Driver settings, graphics registration, start-confirm task'

# Driver settings and router policy. The defaults come from installer\registry-defaults.json, the table that
# manifest.json ("defaults") carries for the control application's reset. An upgrade keeps every value the tester
# changed and writes a new default only over a value the previous installer wrote (common.ps1, Get-RegistryDefaultPlan).
# Installer-owned, always written: the paths into the install root, the graphics registration, UnconfirmedStarts and
# the Release record.
$regDefaults = Get-Content -LiteralPath (Join-Path $here 'registry-defaults.json') -Raw | ConvertFrom-Json
$applied = $null
$appliedSource = 'Release\AppliedDefaults'
$record = (Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\Release" -Name AppliedDefaults -ErrorAction SilentlyContinue).AppliedDefaults
if ($record) { try { $applied = $record | ConvertFrom-Json } catch { Write-Warn2 "Release\AppliedDefaults does not parse: $($_.Exception.Message)" } }
if (-not $applied) { $applied = $regDefaults.legacy_applied; $appliedSource = 'the defaults of tester.1 to tester.7 (no record)' }
Write-Info "previous installer defaults: $appliedSource"
function Invoke-RegistryDefaults([string]$Key, $Defaults, $Previous, [hashtable]$Explicit = @{}, $Owned = $null) {
    $now = Read-RegistryValues $Key
    $plan = Get-RegistryDefaultPlan -Defaults $Defaults -Previous $Previous -Current $now -Explicit $Explicit -Owned $Owned
    Invoke-Change ("${Key}: " + (Format-RegistryPlan $plan)) { Write-RegistryPlan $Key $plan } | Out-Null
}
# Gates: the registered lab configuration (EnableFullWddm 2 opens it at every start). Clocks: load-driven DPM up to
# DpmMaxMHz, thermal limits are the driver's own. KeepLog 0: no log files on the tester's disk. UnconfirmedStarts 0 is
# the INF's own reset (a GPU that was not started has already counted one display-only start).
# BD-175: use the live shared state after migration. Never replay parameters_before_install or restore
# values deleted by a later control-app choice. Runtime records are not installer defaults.
if(-not $legacyRollback) {
Invoke-Change 'apply release setting policy through the validated shared-state resolver' {
    $before=Get-DriverSettingsComparison -State $state -Device $instance -InfSha256 $driverSetSha
    Invoke-DriverStateDefaults -Cli (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe') -Defaults $regDefaults.defaults.parameters `
        -Previous $applied.parameters -Explicit $commandLineParameters -Before $before `
        -FreshExplicit $freshCommandLineParameters -Reopen:$script:ReopenClosures -FreshReopen:$freshRepair -RecordChoices
} | Out-Null
# The record of a person's choice is not a setting, and it has one writer: -RecordChoices above, which writes
# CuModeChosen through the state client inside the same resolver run that sets CuMode (state-lifecycle.ps1).
# A second writer here, straight into the Parameters key, would race that one and could put a value back that a
# later control-app choice deleted (BD-175).
}

# The picture on the start screen. Not in registry-defaults.json: its default is the absent
# value, so the installer removes the value for on and writes 0 for off, and a run that states no answer leaves the
# installed driver's answer alone (common.ps1). The driver reads it when it starts, so the choice shows at the next
# start of Windows. A driver setting, not a per-person one: it is applied whether or not the control application is
# part of this package.
[void](Set-BootLogoChoice -Stated $bootLogoStated)

# DisplayPort audio, the interrupt of the GPU's HD Audio function (BD-092, common.ps1). That function keeps the inbox
# HDAudBus driver, and the inbox INF leaves it on a line interrupt, where the stream interrupts of this board never
# arrive: the audio plays at a third of its rate and a stream takes 7 to 9 seconds to start. One value, MSISupported,
# and a restart of that one device put it on message-signalled interrupts, which is what Linux uses here. The value
# from before this install goes into the state first, so the uninstaller can put it back; a re-run finds the value
# already set and writes nothing. A failure is a warning: the rest of the driver does not depend on the audio.
$audioPlan = @(Get-GpuAudioMsiPlan -Readings (Read-GpuAudioMsi) -Wanted 1)
Write-Info "GPU HD Audio function: $(Format-GpuAudioMsiPlan $audioPlan)"
if ($audioPlan.Count) {
    # Recorded before the first write of this install, and only once: a re-run must not record the value this
    # installer itself wrote as the value from before the install.
    [void](Set-StateValueOnce $state 'hda_msi' @($audioPlan | ForEach-Object { [pscustomobject]@{ instance = $_.instance; previous = $_.previous; key_present = $_.key_present } }))
    Save-InstallState $state
}
foreach ($audio in @($audioPlan | Where-Object { $_.write })) {
    # Invoke-Change throws on to the top-level trap, so the catch is outside it (as for the H.264 encoder above).
    try {
        Invoke-Change "$($audio.key): $(Format-GpuAudioMsiPlan @($audio)), then pnputil /restart-device for that one function" {
            Set-GpuAudioMsiValue $audio.key $audio.value
            $check = Test-GpuAudioMsi -InstanceId $audio.instance -Expected $audio.value
            if (-not $check.ok) { throw "$($script:AudioMsiValue) reads back as '$($check.value)', not $($audio.value)" }
            $restart = Restart-GpuAudioDevice -InstanceId $audio.instance
            Write-Info "pnputil /restart-device: exit $($restart.code), device status '$($restart.status)' after $($restart.seconds) s"
            Write-Log $restart.text
            # A pnputil that did not return inside the budget is its own answer, not a failed device status.
            if ($restart.timed_out) { throw "pnputil /restart-device did not return within $($script:AudioRestartWaitSeconds) s (process $($restart.id) is still running; it was not stopped, because a kernel PnP restart cannot be taken back)" }
            if (-not $restart.ok) { throw "the HD Audio function did not come back within $($script:AudioRestartWaitSeconds) s (pnputil exit $($restart.code), status '$($restart.status)')" }
            $check = Test-GpuAudioMsi -InstanceId $audio.instance -Expected $audio.value
            if (-not $check.ok) { throw "$($script:AudioMsiValue) reads back as '$($check.value)' after the restart, not $($audio.value)" }
        } | Out-Null
    } catch {
        Write-Warn2 ("DisplayPort audio keeps the line interrupt of $($audio.instance): $($_.Exception.Message). " +
            'Sound over DisplayPort can play too slowly. Remedy: run install.cmd -Repair. The rest of the driver is not affected.')
    }
}

# The installed INF owns the complete graphics registration on new packages.
# Resolve it after the recorded PnP restart; no previous FileRepository path is reused.
if ($driverStoreUmd) {
    if ($script:DryRunMode) {
        $graphicsRoot = '%13%'
        foreach ($row in @(Get-DriverPackageRegistration -Package $script:Manifest.driver_package -Root $graphicsRoot)) {
            Write-Info "INF $classKey $($row.name) [$($row.kind)] = $(@($row.value) -join ' | ')"
        }
        foreach ($row in @(Get-DriverPackageSubkeyRegistration -Package $script:Manifest.driver_package)) {
            Write-Info "INF $classKey\$($row.subkey) $($row.name) [$($row.kind)] = $(@($row.value) -join ' | ')"
        }
    } else {
        $graphicsRoot = Get-DriverPackageRoot -ClassKey $classKey
        if (-not $graphicsRoot) { throw 'bound package lacks its DriverStore UMD identity' }
        $problems = @(Test-DriverPackageRegistration -Manifest $script:Manifest -ClassKey $classKey -Root $graphicsRoot)
        if ($problems.Count) { throw ($problems -join '; ') }
    }
    Invoke-Change 'expose the verified ADL directories to new processes after restart' {
        $desired = @(Get-AdlSearchPaths -Package $script:Manifest.driver_package -Root $graphicsRoot)
        $previous = @()
        if ($state.PSObject.Properties['adl_path_entries']) { $previous = @($state.adl_path_entries) }
        $path = Get-MachinePath
        $next = Get-AdlMachinePathPlan -Current $path.value -Previous $previous -Desired $desired
        # Save the union before writing PATH so a partial install can remove both generations.
        Set-StateValue $state 'adl_path_entries' @(@($previous + $desired) | Select-Object -Unique)
        Save-InstallState $state
        if ($next -cne $path.value) { Set-MachinePath $next $path.kind }
        Set-StateValue $state 'adl_path_entries' $desired
        Save-InstallState $state
    } | Out-Null
    $mftDll = Join-Path $graphicsRoot $mft.install_path
    $legacyUmd = @(Get-LegacyUmdValues -InstallRoot $InstallRoot)
    if (-not $state.PSObject.Properties['umd_migration']) {
        Set-StateValue $state 'umd_migration' ([pscustomobject]@{schema=1; install_root=$InstallRoot; values=$legacyUmd})
        Save-InstallState $state
    }
    Invoke-Change 'remove legacy router paths and duplicate global Vulkan entries; retain route policy' {
        Remove-LegacyUmdValues -Rows $legacyUmd
    } | Out-Null
    Set-StateValue $state 'khronos_value' $null
    Set-StateValue $state 'khronos_wow_value' $null
    Set-StateValue $state 'opengl_icd' $null
} else {
# Graphics registration in the GPU's software key: the D3D9 slot '<>' (D3D9On12 on our D3D12 driver), the D3D10/11
# slots, the D3D12 slot, Vulkan.
if ($state.PSObject.Properties['adl_path_entries'] -and @($state.adl_path_entries).Count) {
    Invoke-Change 'remove ADL search entries when returning to a legacy package' {
        $path = Get-MachinePath
        $next = Get-AdlMachinePathPlan -Current $path.value -Previous @($state.adl_path_entries)
        if ($next -cne $path.value) { Set-MachinePath $next $path.kind }
        Set-StateValue $state 'adl_path_entries' @()
        Save-InstallState $state
    } | Out-Null
}
Invoke-Change 'clear the DriverStore UMD marker for the bound legacy rollback package' {
    # An older INF has no DelReg for a value introduced by a newer package.
    # This executes after its binding/restart boundary, before legacy paths.
    Remove-ItemProperty -LiteralPath $classKey -Name AmdgpuDriverPackage -ErrorAction SilentlyContinue
} | Out-Null
$umd = Get-UmdNames $InstallRoot
$icdJson = Join-Path $InstallRoot 'vulkan\radeon_icd.json'
Invoke-Change ("$classKey UserModeDriverName = " + (Format-UmdNames $umd) + "; VulkanDriverName = $icdJson") {
    New-ItemProperty -LiteralPath $classKey -Name UserModeDriverName -Value ([string[]]$umd) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath $classKey -Name VulkanDriverName -Value ([string[]]@($icdJson)) -PropertyType MultiString -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:KhronosKey) '$icdJson' = 0 (system Vulkan ICD)" {
    Initialize-RegistryKey $script:KhronosKey
    New-ItemProperty -LiteralPath $script:KhronosKey -Name $icdJson -Value 0 -PropertyType DWord -Force | Out-Null
} | Out-Null
Set-StateValue $state 'khronos_value' $icdJson
# The same for 32-bit processes (BD-064): the D3D9On12 slot (on the x86 D3D12 shell), the D3D10/11 slots,
# the x86 D3D12 slot, the x86 Vulkan ICD.
$umdWow = Get-WowUmdNames $InstallRoot
$icdJsonWow = Join-Path $InstallRoot 'wow64\vulkan\radeon_icd.json'
Invoke-Change ("$classKey UserModeDriverNameWow = " + (Format-UmdNames $umdWow) + "; VulkanDriverNameWow = $icdJsonWow") {
    New-ItemProperty -LiteralPath $classKey -Name UserModeDriverNameWow -Value ([string[]]$umdWow) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath $classKey -Name VulkanDriverNameWow -Value ([string[]]@($icdJsonWow)) -PropertyType MultiString -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:KhronosKeyWow) '$icdJsonWow' = 0 (32-bit system Vulkan ICD)" {
    Initialize-RegistryKey $script:KhronosKeyWow
    New-ItemProperty -LiteralPath $script:KhronosKeyWow -Name $icdJsonWow -Value 0 -PropertyType DWord -Force | Out-Null
} | Out-Null
Set-StateValue $state 'khronos_wow_value' $icdJsonWow

# The OpenGL ICD (BD-076, GitHub issue #8): three values of the same key per registered architecture, so that every
# OpenGL application of that architecture runs on the GPU without a DLL beside its executable. Windows' opengl32.dll
# reads them through D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO); common.ps1 holds the contract and the sources of
# it. The registration is written here, with the Direct3D and Vulkan ones, because the ICD files live under the
# install root and an INF can only name the driver store.
# The values from before the first install of ours go into the state first, so the uninstaller can put them back; a
# re-run must not record the values this installer itself wrote, hence Set-StateValueOnce. The record covers all six
# value names whatever this release registers, so an architecture that leaves the list can be put back as it was. The
# values this release writes go into the state too: the uninstaller only takes away a value that is still the one from
# here, the rule TdrDelay and the HD Audio interrupt follow.
if ($gl.register) {
    $glPlan = Get-OpenGlRegistrationPlan -Switch $gl -InstallRoot $InstallRoot
    # What the release installed here wrote, read before this run's record replaces it: the step below works on the
    # values of the architectures this release does not register, and that record is the only exact answer to
    # whether they are ours. Read after the replacement it would hold this run's values alone, and those values
    # would have to be judged by the install root they name, which is a weaker test.
    $glWroteBefore = @()
    if ($state.PSObject.Properties['opengl_icd'] -and $state.opengl_icd) { $glWroteBefore = @($state.opengl_icd) }
    # -InstallRoot keeps a registration of ours out of that record: a value naming the install root was written by a
    # release of this family, so it is not a value from before the install, and the uninstaller and the step below
    # have to remove it instead of putting it back. Without this a computer whose state file is gone would get its
    # own earlier Wow registration "restored" and the file it names deleted in the same run (BD-076 review).
    [void](Set-StateValueOnce $state 'opengl_icd_previous' (Get-OpenGlValueRecord -ClassKey $classKey -InstallRoot $InstallRoot))
    Save-InstallState $state
    Invoke-Change ("$classKey " + (Format-OpenGlRegistrationPlan $glPlan) + " ($($gl.value_type), $(@($gl.architectures) -join ' and '))") {
        Write-OpenGlRegistration -ClassKey $classKey -Plan $glPlan
    } | Out-Null
    Set-StateValue $state 'opengl_icd' @($glPlan | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } })
    # The architectures this release does not register. An install over a release of ours that did register one of
    # them has to take those values and that file away again, in that order, or an OpenGL program of that
    # architecture would be sent to a DLL this package does not carry and get no driver at all, instead of the
    # software renderer Windows gives it without a registration. A value that is not ours stays (Get-OpenGlRestorePlan
    # decides that, from the installer's own record and the install root the name value carries), and on a computer
    # that never had such a release this step prints one line and changes nothing.
    $glOff = @(Get-OpenGlUnregisteredValueNames -Switch $gl)
    if ($glOff.Count) {
        $glNowOff = Read-RegistryValues $classKey
        $glOffPlan = @(Get-OpenGlRestorePlan -Previous $(if ($state.PSObject.Properties['opengl_icd_previous']) { $state.opengl_icd_previous } else { @() }) `
            -Wrote $glWroteBefore -Current $glNowOff -InstallRoot $InstallRoot -Names $glOff)
        $glOffAct = @($glOffPlan | Where-Object { $_.action -eq 'restore' -or $_.action -eq 'remove' })
        if ($glOffAct.Count) {
            Invoke-Change ("$classKey OpenGL ICD values of an architecture this release does not register ($(@($script:OpenGlArchitecturesKnown | Where-Object { @($gl.architectures) -notcontains $_ }) -join ', ')): " + (Format-OpenGlRestorePlan $glOffPlan)) {
                Write-OpenGlRestore -ClassKey $classKey -Plan $glOffPlan
            } | Out-Null
        } else {
            Write-Info ("OpenGL ICD: this release registers $(@($gl.architectures) -join ' and ') only; nothing of ours for " +
                "$(@($script:OpenGlArchitecturesKnown | Where-Object { @($gl.architectures) -notcontains $_ }) -join ', ') in the GPU key")
        }
        if ((-not $gl.register_x86) -and (Test-Path -LiteralPath $glDirWow)) {
            Invoke-Change "remove $glDirWow (the 32-bit OpenGL ICD of an earlier install; this release registers $(@($gl.architectures) -join ' and ') only)" {
                Remove-PathOrSchedule $glDirWow
            } | Out-Null
        }
        if ((-not $gl.register_x64) -and (Test-Path -LiteralPath $glDir)) {
            Invoke-Change "remove $glDir (the 64-bit OpenGL ICD of an earlier install; this release registers $(@($gl.architectures) -join ' and ') only)" {
                Remove-PathOrSchedule $glDir
            } | Out-Null
        }
    }
} else {
    # A release that does not register the ICD takes the registration and the files of an earlier install away again,
    # and in that order: a value that names a DLL which is gone would leave every OpenGL application without an ICD
    # and without the software renderer that no registration at all gives it. The rollback is in this stage and not
    # in the files stage above, because the key it writes is the GPU's own software key, which exists only after the
    # driver package of this run.
    $glNow = Read-RegistryValues $classKey
    $glPresent = @($script:OpenGlValueNames | Where-Object { $glNow.ContainsKey($_) })
    $glRollback = Get-OpenGlAction -Switch $gl -PresentValues $glPresent -DirPresent ((Test-Path -LiteralPath $glDir) -or (Test-Path -LiteralPath $glDirWow))
    if ($glRollback -eq 'rollback') {
        # The same opening as the 'none' case below, so that one line of a run always says what this release does
        # about the ICD, whatever it finds on the computer.
        Write-Info ("OpenGL ICD: not registered by this release; the registration and the files of an earlier " +
            "install go ($(if ($glPresent.Count) { $glPresent -join ', ' } else { 'no value of ours in the GPU key' }))")
        if ($glPresent.Count) {
            $glRestore = @(Get-OpenGlRestorePlan -Previous $(if ($state.PSObject.Properties['opengl_icd_previous']) { $state.opengl_icd_previous } else { @() }) `
                -Wrote $(if ($state.PSObject.Properties['opengl_icd']) { $state.opengl_icd } else { @() }) -Current $glNow -InstallRoot $InstallRoot)
            Invoke-Change ("$classKey OpenGL ICD registration of an earlier install (this release does not register it): " + (Format-OpenGlRestorePlan $glRestore)) {
                Write-OpenGlRestore -ClassKey $classKey -Plan $glRestore
            } | Out-Null
        }
        foreach ($d in @($glDir, $glDirWow)) {
            if (Test-Path -LiteralPath $d) {
                Invoke-Change "remove $d (the OpenGL ICD of an earlier install; this release does not register it)" {
                    Remove-PathOrSchedule $d
                } | Out-Null
            }
        }
    } else { Write-Info 'OpenGL ICD: not registered by this release, and nothing of an earlier install on this computer' }
    Set-StateValue $state 'opengl_icd' $null
}

} # legacy Program Files graphics registration

# The H.264 encoder MFT (M15.11): route A of driver/umd/mft-h264/INSTALL.md, machine wide under
# HKLM\SOFTWARE\Classes, which is what HKCR gives every process. A client that wants the encoder of one adapter asks
# Media Foundation with MFT_ENUM_ADAPTER_LUID; the registration holds no LUID and cannot hold one, because Windows
# gives a LUID at boot (INSTALL.md, "What a per-adapter enumeration needs"). The four binary values come out of the
# shipped DLL itself (Bc250BuildMftRegistration), so the enumeration view and the live object cannot differ.
# The encoder is an extra, never a condition of the driver: a computer whose Windows edition has no Media Foundation
# gets a warning here and the rest of the install.
if ($mftAction -eq 'register') {
    $mftBlobs = $null
    # The installed copy, under %ProgramFiles%, not the package copy: the package can sit on a file server, and a UNC
    # path is not a path the interop of mft-h264.ps1 takes. A dry run has copied nothing, so there the package copy is
    # the only one there is.
    $mftBlobDll = $mftDll
    if (-not (Test-Path -LiteralPath $mftBlobDll -PathType Leaf)) { $mftBlobDll = $mft.source }
    try { $mftBlobs = Get-MftRegistrationBlobs -DllPath $mftBlobDll }
    catch { Write-Warn2 "the H.264 encoder MFT is NOT registered: $($_.Exception.Message). The driver itself is not affected." }
    if ($mftBlobs) {
        Write-Info "H.264 encoder MFT: $($mft.name) $($mft.clsid), registration values from $(if ($mftBlobDll -eq $mft.source) { $mft.package_path } else { $mft.install_path })"
        # The encoder is an extra: a registry key that this computer does not let us write ends the step with a
        # warning, never the install of the driver (the comment above this block). Invoke-Change itself throws on to
        # the top-level trap, so the catch has to be outside it.
        try {
            Invoke-Change ('register the H.264 encoder MFT: ' + (Format-MftRegistration -ClassesKey $script:ClassesKey -DllPath $mftDll -Blobs $mftBlobs)) {
                [void](Write-MftRegistration -ClassesKey $script:ClassesKey -DllPath $mftDll -Blobs $mftBlobs)
                $mftCheck = Test-MftRegistration -ClassesKey $script:ClassesKey -DllPath $mftDll -Blobs $mftBlobs
                if (-not $mftCheck.ok) { throw "the H.264 encoder MFT registration does not read back: $($mftCheck.detail)" }
            } | Out-Null
            Set-StateValue $state 'mft_h264' $mftDll
        } catch {
            Write-Warn2 "the H.264 encoder MFT is NOT registered: $($_.Exception.Message). The driver itself is not affected."
            Set-StateValue $state 'mft_h264' $null
        }
    } else { Set-StateValue $state 'mft_h264' $null }
} elseif ($mftAction -eq 'rollback') {
    # The registration of an earlier install went in the files stage, before its DLL: the keys must never outlive the
    # file they name.
    if ($mftKeys.Count) { Write-Info 'H.264 encoder MFT: the registration of an earlier install was removed with its file, in the files stage' }
    else { Write-Info 'H.264 encoder MFT: not registered by this release; no registry key of an earlier install' }
    Set-StateValue $state 'mft_h264' $null
} else {
    Write-Info 'H.264 encoder MFT: not registered by this release, and nothing of an earlier install on this computer'
    Set-StateValue $state 'mft_h264' $null
}

# Router policy (HKLM\SOFTWARE\amdgpu-wddm). DesktopRouter DwmForceCpu 0 composes the desktop on the GPU route (zink);
# 1 is the kill switch to the CPU route (tester.1 to tester.8 shipped 1 until BD-058 was fixed). RequireKmdSwitches 1
# keeps the GPU route gated: when the KMD's effective interop switches are off, the router takes the CPU route by
# itself. AppRouter Mode gpu-default: D3D10.1/D3D11 applications, Windows components included since b20 (BD-061
# verified, BD-088), run on the GPU UMD, except Deny and the sign-in processes. Allow and Deny ship empty since b20:
# Allow dxdiag.exe only lifted the old component rule, and Deny witcher3.exe kept two RADV builds out of one process,
# which the D3D11 and D3D12 paths no longer have (one ICD).
# The *Wow paths are what the x86 router (32-bit processes) loads; the policy values are shared (BD-064).
$desktopPaths = if ($driverStoreUmd) { @{} } else { [ordered]@{ CpuUmdPath = (Join-Path $InstallRoot 'desktop\bc250d3d.dll'); CpuUmdPathWow = (Join-Path $InstallRoot 'wow64\desktop\bc250d3d.dll') } }
$appPaths = if ($driverStoreUmd) { @{} } else { [ordered]@{ GpuUmdPath = (Join-Path $InstallRoot 'd3d11\amdgpu_wddm_d3d11.dll'); GpuUmdPathWow = (Join-Path $InstallRoot 'wow64\d3d11\amdgpu_wddm_d3d11.dll') } }
Invoke-RegistryDefaults "$($script:SoftwareKey)\DesktopRouter" $regDefaults.defaults.desktop_router $applied.desktop_router @{} $desktopPaths
Invoke-RegistryDefaults "$($script:SoftwareKey)\AppRouter" $regDefaults.defaults.app_router $applied.app_router @{} $appPaths
if (-not $driverStoreUmd -and $state.PSObject.Properties['umd_migration']) {
    Invoke-Change 'restore the retained legacy UMD registration overrides for the rollback package' {
        Restore-LegacyUmdValues -Rows @($state.umd_migration.values) -InstallRoot $InstallRoot
    } | Out-Null
}
foreach ($row in $deferredUmdOrphans) {
    Invoke-Change "remove legacy UMD $($row.path) after the new INF binding and route migration" {
        if (-not (Test-Path -LiteralPath $row.path -PathType Leaf)) { return }
        if ((Get-Sha256 $row.path) -ine $row.sha256) {
            throw "legacy UMD changed during migration: $($row.path)"
        }
        Remove-PathOrSchedule $row.path
    } | Out-Null
}
if ($driverStoreUmd) {
    Set-StateValue $state 'umd_retired_files' @()
    Save-InstallState $state
}
# How long Windows waits for the graphics before it resets them: TdrDelay under Windows' own GraphicsDrivers key
# (BD-079; tools\win\amdgpu_wddm_control\src\TdrSetting.cs holds the rules and the range). Windows waits 2 s while
# the value is absent, and this part has no working GPU reset, so a picture that needs longer than the wait does not
# end as a reset engine but as a stopped machine. The release writes 10 s, the waiting time the lab has run since
# 2026-09-28, and the control application's Help page shows and changes it. The upgrade rule is the one every other
# default follows: a value the previous installer wrote is replaced, a value the tester or the control application set
# is kept. The value from before the first write of this install goes into the state, so the uninstaller can put it
# back; a re-run must not record the value this installer itself wrote, hence Set-StateValueOnce.
$tdrBefore = Read-RegistryValues $script:GraphicsDriversKey
[void](Set-StateValueOnce $state 'tdr_delay' ([pscustomobject]@{
    present = [bool]$tdrBefore.ContainsKey('TdrDelay')
    previous = $(if ($tdrBefore.ContainsKey('TdrDelay')) { $tdrBefore['TdrDelay'] } else { $null })
    wrote = $regDefaults.defaults.graphics_drivers.TdrDelay }))
Save-InstallState $state
Invoke-RegistryDefaults $script:GraphicsDriversKey $regDefaults.defaults.graphics_drivers $applied.graphics_drivers
# Application profiles: the shipped ones by the same rule; a tester's own profiles are other keys and stay as they are.
foreach ($app in ConvertTo-PairList $regDefaults.defaults.d3d12_applications) {
    $prevApp = $null
    if ($applied.d3d12_applications -and $applied.d3d12_applications.PSObject.Properties[$app.Name]) { $prevApp = $applied.d3d12_applications.($app.Name) }
    Invoke-RegistryDefaults "$($script:SoftwareKey)\D3D12\Applications\$($app.Name)" $app.Value $prevApp
}
# A profile an older release shipped and this one does not (b20 drops witcher3.exe: present-noprimary forfeits the
# scanout route of M15.14 and the pair gains nothing under the GPU compositor, K115) goes, when its Experiment is still
# the one that release wrote. A profile the tester changed stays, and so does every profile of the tester's own.
foreach ($old in ConvertTo-PairList $applied.d3d12_applications) {
    if (@(ConvertTo-PairList $regDefaults.defaults.d3d12_applications | Where-Object { $_.Name -ieq $old.Name }).Count) { continue }
    $k = "$($script:SoftwareKey)\D3D12\Applications\$($old.Name)"
    if (-not (Test-Path -LiteralPath $k)) { continue }
    $now = (Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue).Experiment
    if (Test-RegistryValueSame $now $old.Value.Experiment) {
        Invoke-Change "$k`: remove the profile of an older release (this release ships none for $($old.Name))" { Remove-Item -LiteralPath $k -Recurse -Force } | Out-Null
    } else {
        Write-Info "$k`: kept, its Experiment '$now' is not what the older release wrote"
    }
}
Invoke-Change "$($script:SoftwareKey)\Release: Version, InstallDir, InstallRoot, InstalledUtc, AppliedDefaults (this package's defaults)" {
    $k = "$($script:SoftwareKey)\Release"
    Initialize-RegistryKey $k
    New-ItemProperty -LiteralPath $k -Name Version -Value ([string]$script:Manifest.version) -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstallDir -Value $InstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstallRoot -Value $InstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstalledUtc -Value ([DateTime]::UtcNow.ToString('o')) -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name AppliedDefaults -Value ($regDefaults.defaults | ConvertTo-Json -Depth 6 -Compress) -PropertyType String -Force | Out-Null
} | Out-Null
# The repair entry (docs/gui/interfaces.md section 4): the setup window of the kept repair set, which the control
# application's Help -> Repair starts with --repair. A package without a setup window leaves no entry.
$repairSetup = Join-Path $script:Closure $script:SetupExeName
Invoke-Change "$($script:SoftwareKey)\Release: RepairSetup = $repairSetup (when the repair set has a setup window)" {
    $k = "$($script:SoftwareKey)\Release"
    if (Test-Path -LiteralPath $repairSetup) { New-ItemProperty -LiteralPath $k -Name RepairSetup -Value $repairSetup -PropertyType String -Force | Out-Null }
    else { Remove-ItemProperty -LiteralPath $k -Name RepairSetup -ErrorAction SilentlyContinue }
} | Out-Null

# The logon tasks: after every logon the confirmation confirms the boot's driver start, so the boot-loop guard does
# not fall back to Basic Display at the third start (see INSTALL.md, "The small blue window after logon"). BD-080:
# two tasks, because the confirmation needs an administrator's token and the window needs the user's session. The
# plan and the reasons are in common.ps1 (Get-LogonTaskPlan). The installer decides nothing here: it holds the plan
# against the plan's own rules (Test-LogonTaskPlan) and then registers exactly what the plan says.
$sc = Join-Path $InstallRoot 'tools\start-confirm.ps1'
$sw = Join-Path $InstallRoot 'tools\start-window.ps1'
# The window's host: the control application of this install, which starts the script with no console window
# (common.ps1 Get-LogonTaskPlan, M899). Without it (-NoControlApp, or a package without one) the plan keeps
# powershell.exe and says why.
$windowHost = $null
if ($script:Manifest -and $script:Manifest.control_app_exe -and ($dirs -contains 'control')) { $windowHost = Join-Path $InstallRoot ('control\' + $script:Manifest.control_app_exe) }
$logonPlan = @(Get-LogonTaskPlan -ConfirmScript $sc -WindowScript $sw -WindowHost $windowHost)
if (-not $windowHost) { Write-Info 'logon window: no control application in this install, so the window task runs powershell.exe and a console window can appear at sign-in' }
# The rules of the plan run here, over the plan this install registers, and not only in the host test: a plan that
# breaks them (the Administrators principal of the defect, a trigger bound to one account, a confirmation without
# an administrator token) stops the install instead of leaving behind a task that never runs at a standard user's
# logon. A dry run reaches this line as well, because nothing of it changes the computer.
$logonPlanProblems = @(Test-LogonTaskPlan $logonPlan)
if ($logonPlanProblems.Count) { throw "the logon task plan of common.ps1 (Get-LogonTaskPlan) breaks its own rules: $($logonPlanProblems -join '; ')" }
$logonRegistered = @()
foreach ($logonTask in $logonPlan) {
    # The trigger comes from the plan too, so "at the logon of any user" - the half of BD-080 that the principal
    # does not cover - cannot differ between the plan and what is registered. New-ScheduledTaskTrigger -AtLogOn
    # without -User is that trigger; any other plan is refused here rather than silently registered as this one.
    if ([string]$logonTask.trigger.kind -ne 'at-logon') { throw "scheduled task '$($logonTask.name)': the plan asks for the trigger kind '$($logonTask.trigger.kind)', and the installer registers only 'at-logon'" }
    if ($logonTask.trigger.user) { throw "scheduled task '$($logonTask.name)': the plan binds the trigger to the logon of '$($logonTask.trigger.user)'; every logon has to run it (BD-080)" }
    # BD-193: phase 2 runs after the logon that started the tasks of phase 1. A registration that matches the plan
    # stays, so the confirmation of this logon runs to its end.
    if (Test-LogonTaskRegistered (Get-ScheduledTask -TaskName $logonTask.name -ErrorAction SilentlyContinue) $logonTask) {
        Write-Info "scheduled task '$($logonTask.name)': registered as planned, kept (a new registration would miss this logon)"
        continue
    }
    Invoke-Change "scheduled task '$($logonTask.name)': $($logonTask.what) (at most $($logonTask.minutes) minutes)" {
        $old = Get-ScheduledTask -TaskName $logonTask.name -ErrorAction SilentlyContinue
        if ($old) { Unregister-ScheduledTask -TaskName $logonTask.name -Confirm:$false }
        $action = New-ScheduledTaskAction -Execute $logonTask.execute -Argument $logonTask.arguments
        $trigger = New-ScheduledTaskTrigger -AtLogOn
        if ($logonTask.principal.kind -eq 'user') { $principal = New-ScheduledTaskPrincipal -UserId $logonTask.principal.id -LogonType $logonTask.principal.logon_type -RunLevel $logonTask.principal.run_level }
        else { $principal = New-ScheduledTaskPrincipal -GroupId $logonTask.principal.id -RunLevel $logonTask.principal.run_level }
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes $logonTask.minutes) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName $logonTask.name -Action $action -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
    } | Out-Null
    $logonRegistered += $logonTask
}
# BD-193: a task registered here while a user is logged on has missed the trigger of that logon. A fresh install
# registers both tasks only now, in phase 2 after the restart, so the start of this boot would stay unconfirmed until
# the next restart. Each such task runs once now. The window task goes first, because the confirmation needs the
# presentation that its window keeps up. A task that does not start costs only this confirmation: the next logon
# runs it.
if ($logonRegistered.Count -and @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -gt 0 }).Count) {
    foreach ($logonTask in @($logonRegistered | Sort-Object { [string]$_.principal.kind -ne 'group' })) {
        Invoke-Change "scheduled task '$($logonTask.name)': run it once now, for the logon that it missed" {
            try { Start-ScheduledTask -TaskName $logonTask.name -ErrorAction Stop }
            catch { Write-Info "scheduled task '$($logonTask.name)': did not start now ($($_.Exception.Message)); it runs at the next logon" }
        } | Out-Null
    }
}

# The control application: a Start menu entry and a desktop icon, both on unless a switch leaves one out. The recovery entry (docs/gui/interfaces.md section 4; it never
# loads bc250control.dll) belongs to the Start menu alone. Nothing resident is installed either way.
$controlExe = $null
if ($script:Manifest -and $script:Manifest.control_app_exe) { $controlExe = Join-Path $InstallRoot ('control\' + $script:Manifest.control_app_exe) }
if ($controlExe -and $dirs -contains 'control') {
    $shortcutPlan = @(Get-ControlShortcutPlan -NoStartMenu ([bool]$NoStartMenuShortcut) -NoDesktop ([bool]$NoDesktopShortcut))
    $shortcutLeft = @(@(Get-ControlShortcutPaths) | Where-Object { $p = $_.path; -not [bool]$_.legacy -and -not (@($shortcutPlan | ForEach-Object { $_.path }) -contains $p) })
    Write-Info ("shortcuts: $(if ($shortcutPlan.Count) { @($shortcutPlan | ForEach-Object { $_.label }) -join ', ' } else { 'none' })" +
        $(if ($shortcutLeft.Count) { "; left out: $(@($shortcutLeft | ForEach-Object { $_.label }) -join ', ')" } else { '' }))
    foreach ($sc in $shortcutPlan) {
        $link = $sc.path
        $arguments = [string]$sc.arguments
        Invoke-Change "$($sc.where) shortcut $link -> $controlExe$(if ($arguments) { " $arguments" } else { '' })" {
            [void](New-ControlShortcut -Link $link -Target $controlExe -Arguments $arguments)
        } | Out-Null
    }
    # A shortcut the tester asked to leave out goes when an earlier install wrote it.
    foreach ($sc in $shortcutLeft) {
        if (-not (Test-Path -LiteralPath $sc.path -PathType Leaf)) { continue }
        Invoke-Change "remove the $($sc.label) $($sc.path): this install was asked for no $($sc.where) shortcut" {
            Remove-Item -LiteralPath $sc.path -Force -ErrorAction SilentlyContinue
        } | Out-Null
    }
    # The names an older release wrote ("amdgpu-wddm Control", common.ps1 Get-ControlShortcutPaths): removed when they
    # point at our control application, whatever the two switches say, because this install replaces them.
    Remove-LegacyControlShortcuts -OwnTargets @(Get-OwnControlTargets -InstallRoots @($InstallRoot) -ExeName ([string]$script:Manifest.control_app_exe))
    Set-StateValue $state 'shortcut' $(if ($shortcutPlan.Count) { @($shortcutPlan | ForEach-Object { $_.path })[0] } else { $null })
    Set-StateValue $state 'shortcuts' @($shortcutPlan | ForEach-Object { $_.path })
    # The Nagi wallpaper: the choice and its stamp for the application, which sets the picture (common.ps1).
    Set-WallpaperRecord -Selected (-not [bool]$NoWallpaper) -Version ([string]$script:Manifest.version)
} else { Write-Info 'control application: not in this package (or -NoControlApp); skipped' }

# The machine profile directory that the Mesa drivers, DXVK and vkd3d-proton read (app-profiles.ps1). A failure is a
# warning: the drivers work without it, with their built-in profiles.
try {
    Invoke-Change 'make the game profile directory: administrators edit, users read' { Initialize-AppProfileDirectory } | Out-Null
} catch { Write-Warn2 "game profile directory not made: $($_.Exception.Message)" }

# ---- the extras ------------------------------------------------------------------------------------------------
# After the driver, and last of all the changes of phase 2: an extra is optional, its download is large, and nothing
# here may put the driver installation at risk. Every failure is a warning, and the stage ends with the install
# complete either way.
# The ids the questions or the command line settled on. A run that never opened the extras stage has $null there, and
# @($null) has one element, so the empty ids are dropped before they are counted.
$componentsChosen = @(@($installInputs.components) | Where-Object { [string]$_ })
$modelsChosenNow = @(@($installInputs.models) | Where-Object { [string]$_ })
if ($null -ne $script:ComponentsManifest -and $componentsChosen.Count) {
    Enter-Stage 'components' 'Extras: llama.cpp, Ollama, models'
    $modelsDir = [string]$script:ComponentsManifest.models_dir
    if (-not $modelsDir) { $modelsDir = $script:ModelsDirDefault }
    $staging = Join-Path $script:StateDir 'component-staging'
    $installedComponents = @()
    $componentRecords = @()
    foreach ($cid in $componentsChosen) {
        $c = Get-ComponentById $script:ComponentsManifest $cid
        if ($null -eq $c) { Write-Warn2 "extra '$cid' is not in this package: skipped"; continue }
        try {
            $from = 'the release assets of this version'
            if ($ComponentDir) { $from = $ComponentDir }
            $zip = Invoke-Change "get $([string]$c.asset.name) ($(Format-ComponentSize ([long]$c.asset.size)), $([string]$c.title)) from $from into $staging and check its SHA256" {
                Get-ComponentAssetStaged -Manifest $script:ComponentsManifest -Component $c -Staging $staging -FromDir $ComponentDir
            }
            if ($script:DryRunMode) {
                if ($ComponentDir) { Write-Info "    $(Join-Path $ComponentDir ([string]$c.asset.name))" }
                else { foreach ($u in Get-ComponentAssetUrls $script:ComponentsManifest $c) { Write-Info "    $u" } }
                Write-Info "    SHA256 $(([string]$c.asset.sha256).ToUpperInvariant())"
                $zip = Join-Path $staging ([string]$c.asset.name)
            }
            $componentRecord = Install-ComponentFiles -Manifest $script:ComponentsManifest -Component $c -Zip $zip -InstallRoot $InstallRoot -Staging $staging -ModelsDir $modelsDir
            $componentRecords += $componentRecord
            $installedComponents += $c
            Write-Info "$([string]$c.title): in $($componentRecord.dir), licence $([string]$c.licence) ($([string]$c.licence_url))"
            # A component that keeps the models in its own format has its own folder for them: say where, because the
            # one in the manifest holds the GGUF files only.
            if ([string]$componentRecord.user_models_dir) { Write-Info "    its own models go into $([string]$componentRecord.user_models_dir)" }
            if ([string]$componentRecord.hooks) { Write-Info "    it is registered for the account that runs this installation; another account runs '$([string]$componentRecord.hooks) register' once" }
        } catch {
            Write-Warn2 "$([string]$c.title) was not installed: $($_.Exception.Message). The driver installation is not affected; run install.cmd again, or install it later."
        }
    }
    if (@($componentRecords).Count) { Set-StateValue $state 'components_installed' @($componentRecords) }
    Set-StateValue $state 'models_dir' $modelsDir
    if ($modelsChosenNow.Count -and @($installedComponents).Count) {
        try {
            $modelRecords = @(Install-ComponentModels -Manifest $script:ComponentsManifest -Components @($installedComponents) -ModelIds $modelsChosenNow -InstallRoot $InstallRoot -ModelsDir $modelsDir -FromDir $ModelDir)
            if (@($modelRecords).Count) { Set-StateValue $state 'models_installed' @($modelRecords) }
            Write-Info "models in $modelsDir"
        } catch {
            Write-Warn2 "a model was not downloaded: $($_.Exception.Message). The driver and the extras stay installed; download it later (the documents say how)."
        }
    }
    if (-not $script:DryRunMode) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
    if (-not $script:DryRunMode) { Save-InstallState $state }
}

Set-StateValue $state 'parameters_before_install' $null
Set-StateValue $state 'driver_settings_compare' $null
Set-StateValue $state 'driver_set' $null
Set-StateValue $state 'firmware_commit' $fw.commit
if ($dwmSession) {
    try {
        $dwmEnd = Get-SessionDwm $dwmSession
        Add-DwmObservation 'end of phase 2' $dwmEnd
        Write-Info "DWM at the end of phase 2: $(Compare-DwmReadings $dwmBefore $dwmEnd)"
    } catch { Write-Log "   DWM reading: $($_.Exception.Message)" }
}
Enter-Stage 'finish' 'Finish'
# The closure of this package becomes the kept repair set, firmware included (WU-051, WU-058): a later repair needs
# neither the downloaded folder nor the network.
Invoke-Change "keep $($script:Closure) as the active repair set (with the installed firmware), the one before it as the previous set, remove older sets" {
    $repairFirmware = if ($driverStoreFirmware) { Join-Path $script:Closure 'firmware' } else { $script:FirmwareInstallDir }
    [void](Complete-RepairSet -Manifest $script:Manifest -Closure $script:Closure -FirmwareFiles @(@($fw.files) | ForEach-Object { Join-Path $repairFirmware $_.name }))
} | Out-Null
# BD-089: our older driver packages in the store. Every release adds one bc250kmd.inf package, Windows binds the newest
# one, and until this release every older one stayed. Kept: the package the GPU is bound to now, and the package of the
# previous repair set, because a rollback installs that one again. pnputil runs without /uninstall and without /force,
# so the started device keeps its driver and no device is touched here. A package that Windows refuses to delete is a
# warning: it changes nothing of this installation.
$storePlan = $null
try {
    $bound = $null
    if ($instance) { $bound = Get-DeviceDriverPackage -InstanceId $instance }
    $sets = @(Get-ChildItem -LiteralPath (Get-PackagesDir) -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $previousSet = (Get-RepairSetPlan (Read-RepairSetIndex) ([string]$script:Manifest.version) $sets).previous
    $storePlan = Get-DriverStoreRemovePlan -Packages (Get-OurDriverPackageList) -BoundPublished $bound -PreviousVersion (Get-RepairSetDriverVersion $previousSet)
} catch { Write-Warn2 "our driver packages in the store were not examined ($($_.Exception.Message)): none of them is removed" }
if ($storePlan) {
    Write-Info ("driver store: $(@($storePlan.keep).Count) package(s) of ours kept ($(if (@($storePlan.keep).Count) { @($storePlan.keep) -join ', ' } else { 'none' }))" +
        $(if ($storePlan.previous) { ", $($storePlan.previous) of them for a rollback" } else { '' }) +
        "; $(@($storePlan.remove).Count) to remove" + $(if ($storePlan.why) { "; $($storePlan.why)" } else { '' }))
    foreach ($oldPackage in @($storePlan.remove)) {
        Invoke-Change "pnputil /delete-driver $oldPackage (an older package of ours; no /uninstall, so the GPU keeps the package it uses)" {
            $n = Invoke-Native pnputil.exe @('/delete-driver', $oldPackage)
            Write-Log $n.text
            if ($n.code -ne 0) { Write-Warn2 "pnputil /delete-driver $oldPackage exit $($n.code): $($n.text). The package stays in the store and changes nothing of this installation." }
        } | Out-Null
    }
}
# BD-211: the MEMxxxx.TMP files that the user-mode drivers of earlier releases left in the temp folders
# (mesa-temp-cleanup.ps1). Only files older than a day and not open; a failure here is a warning and changes nothing
# of this installation.
try {
    $mesaRoots = Get-MesaTempRoots
    $mesaFound = Find-MesaTempLeftovers -Roots $mesaRoots
    Write-Log "   BD-211 temp files: $($mesaFound.count) older than a day ($($mesaFound.bytes) bytes), $($mesaFound.young) newer, $($mesaFound.unreadable) folder(s) not readable, in $($mesaRoots.Count) temp folder(s)"
    if ($mesaFound.count) {
        Invoke-Change "remove $($mesaFound.count) temporary files ($([Math]::Round($mesaFound.bytes / 1MB, 1)) MB) that earlier versions of this driver left in the temp folders (BD-211); a file that is open stays" {
            $mesaDone = Remove-MesaTempLeftovers -Files $mesaFound.files
            Write-Info "removed $($mesaDone.removed) temporary files ($([Math]::Round($mesaDone.bytes / 1MB, 1)) MB); $($mesaDone.in_use) in use and $($mesaDone.denied) not permitted stay"
        } | Out-Null
    }
} catch { Write-Warn2 "the temporary files of earlier versions were not removed ($($_.Exception.Message)); this changes nothing of this installation" }
Invoke-SelectedSystemTuning -Selected ([bool]$ApplySystemTuning) -Action ApplyRecommended -PackageRoot $package
if($legacyRollback) {
    Invoke-Change 'enable the exact legacy driver only after its complete payload, firmware, settings and tasks are in place' {
        Complete-LegacyRollback -State $state -Worker $stateWorker -Operation enable -Save {param($s) Save-InstallState $s}
    } | Out-Null
}
Clear-InstallInputs $state
Save-Phase 'installed'
Set-ResumeAtLogon 'verify'
Write-Host ''
if ($script:DryRunMode) { Write-Host 'Dry run complete: every check ran, nothing was changed.' -ForegroundColor Green }
else { Write-Host 'Installation complete. After the restart, the installer verifies the driver by itself.' -ForegroundColor Green }
# A dry run reports the restart that the real run would need, and never restarts.
Request-Restart 'the driver starts with its full configuration at the next start.' 'restart.complete'
if ($script:DryRunMode) { Exit-Engine -Code 0 -Outcome 'completed' -MessageId 'result.dry-run-complete' }
Exit-Engine -Code 0 -Outcome 'restart-required' -MessageId 'result.installed-restart'
