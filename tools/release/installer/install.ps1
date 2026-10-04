# amdgpu-wddm tester installer, v0. Run install.cmd as administrator from the unpacked package folder.
#
# Three phases, resumed automatically from %ProgramData%\amdgpu-wddm\installer\state.json:
#   1. preflight, System Restore point, test signing on (asks first), then a restart. Skipped when test signing is
#      already active in the running boot.
#   2. certificate, driver package, user-mode drivers, registry, firmware, start-confirm task, control app; restart.
#      The GPU changes driver at that restart: no device restart and no DWM restart under the running desktop.
#   3. verify: driver bound, version, start health, D3D12 device at feature level 12_1, Vulkan enumerates the GPU.
# -DryRun runs every check and prints every change without making it. -Verify runs phase 3 only.
# Network: the package does not contain the AMD GPU firmware. Phase 2 downloads it from linux-firmware at the commit
# pinned in manifest.json (kernel.org, GitLab mirror as fallback) and checks each SHA256, or takes it from a local
# folder (-FirmwareDir), checked the same way. A prepared offline folder or a kept repair set carries the files in its
# own firmware\ folder, which is used when -FirmwareDir is not given. Nothing else uses the network.
#
# The setup window (tools\win\amdgpu_wddm_setup) drives this script with -Gui (docs/gui/interfaces-setup.md): the
# engine writes events (-EventsFile) and one terminal result bound to -InvocationId (-ResultFile), decides the action
# and the consents it needs, never asks, never elevates itself and never restarts Windows. -Plan runs the checks and
# reports the decision, the consents, the restarts and the settings-impact plan, and changes nothing. Before the
# first restart it asks for, the whole package (and an offline firmware folder) is staged and checked in
# %ProgramData%\amdgpu-wddm\installer\packages\<version>, and RunOnce runs the continuation from there; when phase 2
# completes, that copy stays as the repair set. On the command line the installer asks "Restart now?" as before and
# then asks Windows for a normal, planned restart (never a forced one).
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Plan,                          # checks and the decision only; changes nothing (a dry run that stops early)
    [switch]$Gui,                           # driven by the setup window: no prompt, no elevation, no restart
    [string]$InvocationId,                  # the setup window's id of this run, echoed in every event and the result
    [string]$EventsFile,                    # events, one JSON object per line (appended)
    [string]$ResultFile,                    # the terminal result (written once, at the end)
    [string]$DeadlineUtc,                   # ISO 8601 UTC: stop at the first stop point after this time (interfaces-setup.md 10)
    [switch]$Verify,
    [switch]$Force,                         # install even over a development-lab installation (not supported)
    [switch]$NoReboot,                      # never restart; tell the tester to do it
    [switch]$AcceptTestSigning,             # unattended: answer YES to the test-signing question
    [ValidateSet('', 'HaveKey', 'Suspend')][string]$BitLocker = '',
    [ValidateRange(1000, 2000)][int]$DpmMaxMHz,  # absent = the default (registry-defaults.json) or the tester's own value
    [ValidateSet(0, 24, 40)][int]$CuMode = 0,  # 0 = leave unset (driver default, 24 CUs)
    [string]$InstallRoot = (Join-Path $env:ProgramFiles 'amdgpu-wddm'),
    [switch]$NoControlApp,
    [switch]$Repair,                        # install the same, already verified version again
    [string]$FirmwareDir,                   # offline: the 9 firmware files (INSTALL.md, "GPU firmware") instead of a download
    [switch]$DryRunIgnoreBoard             # host test only, honoured with -DryRun: walk all phases on a PC without a BC-250
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
. (Join-Path $here 'dwm-session.ps1')
. (Join-Path $here 'engine.ps1')
. (Join-Path $here 'release-witness.ps1')
. (Join-Path $here 'compatibility.ps1')
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
# (plan, dry run, verify) every stage boundary is a stop point; in a real install the deadline is only announced here
# and acted on at the next stop point.
function Enter-Stage([string]$Id, [string]$Text) {
    if ($script:DryRunMode -or $script:EngineMode -eq 'verify') { Invoke-CancelPoint "stage:$Id" } else { Write-DeadlineNotice "stage:$Id" }
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
    $build = [int]$os.BuildNumber
    if (-not [Environment]::Is64BitOperatingSystem) { Add-Check 'Windows' 'fail' "$($os.Caption) is 32-bit; x64 is required" 'windows.not-x64' }
    elseif ($build -lt 22000) { Add-Check 'Windows' 'fail' "$($os.Caption) build $build; Windows 11 (build 22000 or later) is required" 'windows.too-old' }
    else { Add-Check 'Windows' 'ok' "$($os.Caption) build $build" 'windows.ok' }

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

    $sb = Get-SecureBootState
    $script:SecureBoot = $sb
    if ($sb -eq 'on') { Add-Check 'Secure Boot' 'fail' 'on: test signing cannot be enabled. Turn Secure Boot off in the BIOS setup first (see INSTALL.md, section Secure Boot).' 'secureboot.on' }
    elseif ($sb -eq 'unknown') { Add-Check 'Secure Boot' 'warn' 'state unknown' 'secureboot.unknown' }
    else { Add-Check 'Secure Boot' 'ok' $sb 'secureboot.off' }

    $tsActive = Get-TestSigningActive
    $tsNext = Get-TestSigningConfigured
    $script:TestSigningActive = $tsActive
    $script:TestSigningConfigured = $tsNext
    Add-Check 'test signing' 'ok' ("active in this boot: $tsActive; set for the next boot: $(if ($null -eq $tsNext) { 'unknown' } else { $tsNext })") $(if ($tsActive) { 'testsigning.active' } else { 'testsigning.inactive' })

    $bl = Get-BitLockerState
    $script:BitLockerState = $bl
    if ($bl -eq 'on') { Add-Check 'BitLocker' 'warn' "protection on for $($env:SystemDrive): changing the boot options makes Windows ask for the recovery key at the next start. You must have the key, or let the installer suspend BitLocker for two restarts." 'bitlocker.on' }
    elseif ($bl -eq 'unknown') { Add-Check 'BitLocker' 'warn' 'state unknown: if BitLocker is on, have the recovery key ready' 'bitlocker.unknown' }
    else { Add-Check 'BitLocker' 'ok' $bl 'bitlocker.ok' }

    $hvci = Get-MemoryIntegrityState
    if ($hvci -eq 'on') { Add-Check 'Memory integrity' 'warn' 'on: if the driver does not start, turn off Core isolation > Memory integrity (see INSTALL.md)' 'hvci.on' }
    else { Add-Check 'Memory integrity' 'ok' $hvci 'hvci.off' }

    $vc = @(Get-VcRuntimeMissing)
    if ($vc.Count) { Add-Check 'Visual C++ runtime' 'fail' ("missing in System32: $($vc -join ', '). Install the Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 (x64) from https://aka.ms/vs/17/release/vc_redist.x64.exe (on any computer with a network, then copy it here), then run the installer again.") 'vcruntime.missing' }
    else { Add-Check 'Visual C++ runtime' 'ok' 'present' 'vcruntime.ok' }

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

    if (Test-LabInstallPresent) {
        if ($Force) { Add-Check 'existing installation' 'warn' 'a development-lab installation (C:\BC250\m1x) is present; -Force given' 'lab-install.forced' }
        else { Add-Check 'existing installation' 'fail' 'a development-lab installation (C:\BC250\m1x) is present. The release installer does not change a lab machine.' 'lab-install.present' }
    }
    $st = Read-InstallState
    $script:State = $st
    if ($st) { Add-Check 'installer state' 'ok' "phase $($st.phase) since $($st.updated_utc)" 'state.ok' }
    elseif ($script:DeviceService -eq $script:ServiceName -and -not $Force) { Add-Check 'existing installation' 'fail' 'bc250kmd is already installed without installer state; run uninstall.cmd first' 'existing.no-state' }

    return $checks
}

# RunOnce runs the continuation at the next logon (GUI plan A1): the installer of the staged closure ('continue') or
# the installed verify.cmd ('verify'); a run of the setup window continues in the closure's setup window. The program
# and each argument are quoted on their own (Format-CommandLine).
$script:Closure = $null
$script:Continuation = $null
function Set-ResumeAtLogon([ValidateSet('continue', 'verify')][string]$Kind) {
    $c = Get-ContinuationCommand -Kind $Kind -Closure $script:Closure -InstallRoot $InstallRoot -Gui ([bool]$script:GuiMode)
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
if ($Verify) { $action = [ordered]@{ action = 'verify'; message = $null } }
if ($early) { Write-Info "installed: $(if ($installedVersion) { $installedVersion } else { 'unknown version' }), phase $($early.phase); package: $packageVersion; action: $($action.action)" }
$script:EngineAction = $action.action
$script:EnginePackageVersion = $packageVersion
# A boundary advances only in a new boot (GUI plan A2): the run that continues after the driver package's restart
# checks that Windows did restart since the restart was asked for. Nothing is changed when it did not.
$restartBoot = $null
if ($early -and $early.PSObject.Properties['restart_boot_id']) { $restartBoot = $early.restart_boot_id }
if ($action.action -eq 'resume' -and $null -ne $restartBoot -and $null -ne (Get-BootIdentity).boot_id -and [int64]$restartBoot -eq [int64](Get-BootIdentity).boot_id) {
    Write-Host 'The restart that the driver package asked for has not happened yet: restart the computer. The installer continues after you log on again.' -ForegroundColor Yellow
    $script:EngineRestart = [ordered]@{ required = $true; reason_id = 'restart.driver-package'; still_pending = $true; continuation = $null }
    Exit-Engine -Code 7 -Outcome 'restart-required' -MessageId 'result.restart-still-pending' -Detail "boot $restartBoot is still running"
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
$commandLineParameters = $installInputs.parameters
$NoControlApp = [switch](@($installInputs.switches) -contains 'NoControlApp')
$NoReboot = [switch](@($installInputs.switches) -contains 'NoReboot')
$Force = [switch](@($installInputs.switches) -contains 'Force')

# Verification needs no preflight: it reads the installed copy (verify.cmd in the install root) or the package.
if ($action.action -eq 'verify' -and -not ($DryRun -and -not $Verify)) {
    $m = Join-Path $package 'manifest.json'
    if (Test-Path -LiteralPath $m) { $script:Manifest = Get-Content -LiteralPath $m -Raw | ConvertFrom-Json }
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

$state = $script:State
if (-not $state) { $state = [pscustomobject]@{ schema = 1; phase = 'new'; created_utc = [DateTime]::UtcNow.ToString('o'); updated_utc = $null } }
}
function Save-Phase([string]$Phase) {
    Set-StateValue $state 'phase' $Phase
    Set-StateValue $state 'updated_utc' ([DateTime]::UtcNow.ToString('o'))
    Save-InstallState $state
}
# A restart that this install needs. The boot it was asked in goes into the state first (a boundary advances only in
# a new boot). A run of the setup window ends here with the restart in its result: the window asks the user and
# restarts Windows itself. The command line asks "Restart now?" and then asks Windows for a normal, planned restart:
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
    $go = Read-Confirmation -Question 'Restart now?' -Expect 'Y'
    if ($go) {
        $why = Request-PlannedRestart
        if ($why) { Write-Warn2 "Windows did not start the restart ($why): restart the computer yourself; the installer continues after you log on again" }
    } else { Write-Info 'restart later; the installer continues after you log on again' }
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
    # Every verify run ends with its report (Write-VerifyReport), also when a check throws or the GPU is missing: the
    # control application counts a verification only from a report with a non-empty result list for the installed
    # package, never from the phase label.
    try {
    if (-not $script:DryRunMode) {
        # Files replaced while in use: the restart deletes their old copies; this catches any it could not.
        $n = (Remove-OldCopies -Directory $InstallRoot -Recurse) + (Remove-OldCopies -Directory (Join-Path $env:windir 'System32') -Filter 'bc250umd.dll.old-*')
        if ($n) { Write-Info "removed $n old copies of replaced files" }
    }
    $dev = @(Get-Bc250Device)
    if ($dev.Count -ne 1) { Add-Result 'device' $false 'BC-250 GPU not found'; return $results }
    $id = $dev[0].DeviceID
    $svc = Get-DeviceServiceName -InstanceId $id
    $pnp = Get-PnpDevice -InstanceId $id
    Add-Result 'driver bound' (($svc -eq $script:ServiceName) -and ($pnp.Status -eq 'OK')) "service $svc, status $($pnp.Status), problem code $($dev[0].ConfigManagerErrorCode)"
    $drv = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID='$($id -replace '\\', '\\')'" -ErrorAction SilentlyContinue
    $want = $null
    if ($script:Manifest) { $want = $script:Manifest.kmd_version }
    # The release's DriverVer (x.y.z.100) differs from every lab build, so the bound version names the package.
    $inf = $null
    if ($drv) { $inf = $drv.InfName }
    # Win32_PnPSignedDriver leaves DriverProviderName empty for this package; the device property has the INF's Provider.
    $provider = $null
    try { $provider = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverProvider -ErrorAction Stop).Data } catch { }
    if (-not $provider -and $drv) { $provider = $drv.DriverProviderName }
    Add-Result 'driver version' (($null -ne $drv) -and ($drv.DriverVersion -eq $want)) "installed $($drv.DriverVersion) from $inf (provider $provider), package $want"
    # One read-only reading of the start-confirm task's inputs, in a child process (start-confirm.ps1 -Probe): the
    # driver version and LastStage through bc250kmd_cli, the KMD start health through bc250control.dll, the DPM
    # registry state. The task itself runs at the same logon and may still be waiting for its 60 s.
    # Verify starts at the same logon as the start-confirm task, which needs about a minute: wait for this boot's run
    # of the task to end (at most 120 s), so that the confirmation and DPM lines below are final.
    $taskNote = 'start-confirm task: not registered'
    $task = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
    if ($task) {
        $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
        $waited = [Diagnostics.Stopwatch]::StartNew()
        $announced = $false
        while ($true) {
            $ti = Get-ScheduledTaskInfo -TaskName $script:TaskName -ErrorAction SilentlyContinue
            $running = (Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue).State -eq 'Running'
            $ranThisBoot = $ti -and $ti.LastRunTime -and ($ti.LastRunTime -ge $boot)
            if (($ranThisBoot -and -not $running) -or $script:DryRunMode) { break }
            if (Test-DeadlinePassed) { Write-DeadlineNotice 'verify:start-confirm-wait'; break }
            if ($waited.Elapsed.TotalSeconds -ge 120) { break }
            if (-not $announced) { Write-Info 'waiting for the start-confirm task of this logon to finish (at most 120 s)...'; $announced = $true }
            Start-Sleep -Seconds 2
        }
        $code = $ti.LastTaskResult
        $meaning = switch ($code) { 0 { 'confirmed through the KMD start health' } 6 { 'fallback: boot-loop guard reset only, DPM not confirmed' } 1 { 'bound reached unconfirmed' } 5 { 'no start-health reading' } 267009 { 'still running' } default { 'see C:\ProgramData\amdgpu-wddm\start-confirm.log' } }
        $taskNote = "start-confirm task: last run $($ti.LastRunTime), result $code ($meaning), waited $([int]$waited.Elapsed.TotalSeconds) s"
    }
    Write-Info $taskNote
    $sc = Join-Path $InstallRoot 'tools\start-confirm.ps1'
    $probe = ''
    if (Test-Path -LiteralPath $sc) { $probe = (Invoke-Native powershell.exe @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $sc, '-Probe')).text }
    Write-Log $probe
    $view = $null; if ($probe -match 'fallback view: (.+)') { $view = $Matches[1].Trim() }
    $want = [string]$script:Manifest.kmd_abi
    $driverOk = ($null -ne $view) -and ($view -match 'device problem 0,') -and ($view -match 'LastStage 50$') -and ($view -match "driver version $want ")
    Add-Result 'driver start' $driverOk $(if ($view) { $view } else { "no reading from $sc" })
    $health = $null; if ($probe -match 'start health (.+)') { $health = $Matches[1].Trim() }
    $hflags = $null; if ($health -match 'flags (\d+)') { $hflags = [int]$Matches[1] }
    Add-Result 'start health' (($null -ne $hflags) -and (($hflags -band 7) -eq 7)) $(if ($health) { "$health (15 = confirmed; 7 = full, ready, visible)" } else { 'no reading' })
    $uc = (Get-ItemProperty -LiteralPath $script:ParametersKey -Name UnconfirmedStarts -ErrorAction SilentlyContinue).UnconfirmedStarts
    Add-Result 'boot-loop guard' (($null -eq $uc) -or ([int]$uc -lt 2)) "UnconfirmedStarts $uc (the start-confirm task resets it after each logon)"
    $p = Get-ItemProperty -LiteralPath $script:ParametersKey -ErrorAction SilentlyContinue
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
    if (Test-Path -LiteralPath $cli) { $io = [string](Invoke-Native $cli @('interop')).text }
    Write-Log $io
    $ioFirst = (($io -split "`n") | Select-Object -First 1)
    $ioOk = ($io -match 'effective blit\+cdd') -and ($io -notmatch 'died in a session')
    $forceCpu = (Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\DesktopRouter" -Name DwmForceCpu -ErrorAction SilentlyContinue).DwmForceCpu
    $dwmNote = "DwmForceCpu $forceCpu (desktop on the CPU route as set)"
    $dwmOk = $true
    if ($forceCpu -eq 0) {
        $zink = $false
        $mySession = (Get-Process -Id $PID).SessionId
        foreach ($d in @(Get-Process dwm -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $mySession })) {
            try { if ($d.Modules | Where-Object { $_.FileName -match 'bc250d3d_zink\.dll$' }) { $zink = $true } } catch { }
        }
        $dwmOk = $zink
        $dwmNote = "DwmForceCpu 0, DWM of session $mySession has bc250d3d_zink.dll loaded: $(if ($zink) { 'yes' } else { 'no (CPU route)' })"
    }
    $ioClosed = ''; if ($io -match 'closed by the driver: (\S+)') { $ioClosed = "; closed by the driver: $($Matches[1])" }
    if ($io -match 'died in a session') { $ioClosed += '; last boot died in a session' }
    Add-Result 'GPU desktop path' ($ioOk -and $dwmOk) $(if ($io) { "$($ioFirst.Trim())$ioClosed; $dwmNote" } else { "no reading from $cli; $dwmNote" })

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

    # D3D12 through the system runtime, as an application sees it.
    $caps = Join-Path $InstallRoot 'tools\amdgpu_wddm_d3d12caps.exe'
    $vdir = Join-Path $script:StateDir 'verify'
    [void][IO.Directory]::CreateDirectory($vdir)
    $index = $null
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

    # Vulkan through the system loader, in a child process so that a failing ICD cannot take the installer down.
    $vk = Join-Path $here 'vk-check.ps1'
    $vkOut = (Invoke-Native powershell.exe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $vk)).text.Trim()
    if ($vkOut -match '^SKIP') { Write-Host "   [skip] Vulkan               $vkOut" -ForegroundColor Yellow; Write-Log "   verify Vulkan: $vkOut" }
    else { Add-Result 'Vulkan' ($vkOut -match '(?m)^device 0x1002:0x13FE') $vkOut }
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
        $mp = Join-Path $InstallRoot 'manifest.json'
        $source = 'install-root'
        if (-not (Test-Path -LiteralPath $mp)) { $mp = Join-Path $package 'manifest.json'; $source = 'package' }
        $mf = $null; $msha = $null
        if (Test-Path -LiteralPath $mp) { $mf = Get-Content -LiteralPath $mp -Raw | ConvertFrom-Json; $msha = Get-Sha256 $mp }
        $failed = @($list | Where-Object { -not $_.pass }).Count
        $complete = [bool]$script:VerifyComplete
        $report = [ordered]@{
            schema = 'amdgpu-wddm.verify-report/1'
            utc = [DateTime]::UtcNow.ToString('o')
            invocation = $script:EngineInvocationId
            boot_id = (Get-BootIdentity).boot_id
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
        Write-FileAtomic (Join-Path $vdir ('verify-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '.json')) ($report | ConvertTo-Json -Depth 5)
    } catch { Write-Log "   verify report not written: $($_.Exception.Message)" }
}

if ($script:VerifyOnly) {
    # Phase 2 ended in this boot: the driver runs with the old settings until the restart, and the start-confirm
    # task was registered for the next logon. Verifying now would only report the restart that is still due.
    if ($state.phase -eq 'installed' -and $state.updated_utc) {
        $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
        $done = ([DateTime]::Parse([string]$state.updated_utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)).ToUniversalTime()
        $sameBoot = $state.PSObject.Properties['restart_boot_id'] -and ($null -ne $state.restart_boot_id) -and ($null -ne (Get-BootIdentity).boot_id) -and ([int64]$state.restart_boot_id -eq [int64](Get-BootIdentity).boot_id)
        if ($boot -lt $done -or $sameBoot) {
            Write-Host "Restart pending: the installation finished at $($done.ToString('u')), after this start ($($boot.ToString('u'))). Restart the computer; verify runs by itself after the logon." -ForegroundColor Yellow
            $script:EngineRestart = [ordered]@{ required = $true; reason_id = 'restart.complete'; still_pending = $true; continuation = $null }
            Exit-Engine -Code 7 -Outcome 'restart-required' -MessageId 'result.verify-before-restart' -Detail "installed at $($done.ToString('o')), this boot started at $($boot.ToString('o'))"
        }
    }
    Enter-Stage 'verify' 'Verify'
    $r = Invoke-Verify
    $bad = @($r | Where-Object { -not $_.pass })
    if (-not $DryRun) { Save-Phase $(if ($bad.Count) { 'verify-failed' } else { 'verified' }) }
    # The running-release witness of this boot (release-witness.ps1): written only when the loaded KMD image and the
    # driver's reply match this release; the control application reads it.
    if (-not $DryRun) {
        $why = Write-RunningReleaseWitness -InstallRoot $InstallRoot -RecordedBy 'verify' -Boot (Get-BootIdentity) -StatePath $script:StatePath
        if ($why) { Write-Log "   running-release witness not written: $why" } else { Write-Log "   running-release witness written: $($script:WitnessPath)" }
        Write-EngineEvent 'witness' ([ordered]@{ written = (-not $why); reason = $why })
    }
    if ($bad.Count) {
        Write-Host "Verification: $($bad.Count) check(s) failed. See INSTALL.md, section 'If something fails'." -ForegroundColor Red
        Exit-Engine -Code 3 -Outcome 'verify-failed' -MessageId 'result.verify-failed' -Detail (($bad | ForEach-Object { "$($_.check): $($_.detail)" }) -join ' | ')
    }
    Write-Host 'Verification passed. The BC-250 runs on the amdgpu-wddm driver.' -ForegroundColor Green
    $warned = @($r | Where-Object { $_.PSObject.Properties['warning'] -and $_.warning })
    if ($warned.Count) { Write-Host "$($warned.Count) warning(s) above ([warn]): read the remedy on each line." -ForegroundColor Yellow }
    Exit-Engine -Code 0 -Outcome 'verified' -MessageId $(if ($warned.Count) { 'result.verified-with-warnings' } else { 'result.verified' }) -Detail "$($warned.Count) warning(s)"
}

# ---- the decision (GUI plan A2) ------------------------------------------------------------------------------------
# Everything the user decides on comes from here, before any change: the action, the consents it needs, the restarts,
# where the firmware comes from, and what happens to each setting (WU-006, WU-044). -Plan stops after it. A run of the
# setup window that lacks a consent stops here too, with nothing changed.
function Get-InstallSettingsImpact {
    $tbl = Get-Content -LiteralPath (Join-Path $here 'registry-defaults.json') -Raw | ConvertFrom-Json
    $prev = Get-PreviousAppliedDefaults $tbl ((Get-ItemProperty -LiteralPath "$($script:SoftwareKey)\Release" -Name AppliedDefaults -ErrorAction SilentlyContinue).AppliedDefaults)
    $params = Read-RegistryValues $script:ParametersKey
    # A re-run past the driver package judges the values from before its reset, as phase 2 does.
    if ($state -and $state.PSObject.Properties['parameters_before_install'] -and $null -ne $state.parameters_before_install) {
        $params = @{}; foreach ($p in $state.parameters_before_install.PSObject.Properties) { $params[$p.Name] = $p.Value }
    }
    $groups = @(
        @{ group = 'parameters'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.parameters -Previous $prev.applied.parameters -Current $params -Explicit $commandLineParameters) }
        @{ group = 'desktop_router'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.desktop_router -Previous $prev.applied.desktop_router -Current (Read-RegistryValues "$($script:SoftwareKey)\DesktopRouter")) }
        @{ group = 'app_router'; plan = (Get-RegistryDefaultPlan -Defaults $tbl.defaults.app_router -Previous $prev.applied.app_router -Current (Read-RegistryValues "$($script:SoftwareKey)\AppRouter")) }
    )
    foreach ($app in ConvertTo-PairList $tbl.defaults.d3d12_applications) {
        $prevApp = $null
        if ($prev.applied.d3d12_applications -and $prev.applied.d3d12_applications.PSObject.Properties[$app.Name]) { $prevApp = $prev.applied.d3d12_applications.($app.Name) }
        $groups += @{ group = "d3d12:$($app.Name)"; plan = (Get-RegistryDefaultPlan -Defaults $app.Value -Previous $prevApp -Current (Read-RegistryValues "$($script:SoftwareKey)\D3D12\Applications\$($app.Name)")) }
    }
    return (Get-SettingsImpact $groups)
}
$consents = @()
$restarts = 0
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    $restarts = 1
    if ($state.phase -in @('new', 'testsigning-pending') -and -not $script:TestSigningActive) { $restarts = 2 }
    if ($state.phase -eq 'new' -and -not $script:TestSigningActive) {
        $consents += 'test-signing'
        if ($script:BitLockerState -eq 'on') { $consents += 'bitlocker' }
    }
}
$firmwareSource = $(if (-not $FirmwareDir) { 'download' } elseif (Test-SamePath $FirmwareDir (Join-Path $package 'firmware')) { 'package-folder' } else { 'folder' })
$notes = @(Get-ChildItem -LiteralPath $package -File -Filter 'RELEASE-NOTES*.md' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
# This package's own compatibility record (compatibility.ps1). Phase 1 only reports it; a rollback admits a package
# only when it verifies (G-RB, Ph 3).
$compat = $null
if ($action.action -ne 'verify' -and $script:Manifest) {
    try { $cv = Test-CompatibilityRecord -PackageRoot $package -CheckedManifest $script:Manifest; $compat = [ordered]@{ ok = $cv.ok; reasons = @($cv.reasons) }; if (-not $cv.ok) { Write-Log "   compatibility record: $($cv.detail)" } } catch { Write-Log "   compatibility record: $($_.Exception.Message)" }
}
Write-EngineEvent 'decision' ([ordered]@{ action = $action.action; message = $action.message; installed_version = $installedVersion; package_version = $packageVersion; phase = [string]$state.phase
    consents = @($consents); consents_given = [ordered]@{ test_signing = [bool]$AcceptTestSigning; bitlocker = $BitLocker }; restarts = $restarts
    firmware_source = $firmwareSource; firmware_dir = $FirmwareDir; notes = @($notes); secure_boot = $script:SecureBoot; bitlocker = $script:BitLockerState
    compatibility = $compat })
if ($action.action -in @('install', 'upgrade', 'repair', 'resume')) {
    $impact = Get-InstallSettingsImpact
    Write-Info ("settings: {0} kept as changed by you, {1} updated to a new default, {2} added, {3} unchanged, {4} from the command line" -f $impact.summary.kept, $impact.summary.updated, $impact.summary.added, $impact.summary.unchanged, $impact.summary.command)
    foreach ($row in @($impact.rows | Where-Object { $_.decision -ne 'same' })) { Write-Log ("   setting {0}\{1}: {2} {3} -> {4}" -f $row.group, $row.name, $row.decision, (Format-RegistryValue $row.current), (Format-RegistryValue $row.value)) }
    Write-EngineEvent 'settings-plan' ([ordered]@{ summary = $impact.summary; rows = @($impact.rows) })
}
$script:EngineConsents = @($consents)
if ($Plan) {
    Write-Host ''
    Write-Host "Plan: $($action.action)$(if ($consents.Count) { "; consents needed: $($consents -join ', ')" }); restarts: $restarts; firmware: $firmwareSource. Nothing was changed." -ForegroundColor Green
    Exit-Engine -Code 0 -Outcome 'planned' -MessageId 'result.planned'
}
if ($script:GuiMode) {
    $missing = @()
    if ($consents -contains 'test-signing' -and -not $AcceptTestSigning) { $missing += 'test-signing' }
    if ($consents -contains 'bitlocker' -and -not $BitLocker) { $missing += 'bitlocker' }
    $script:EngineConsents = $missing
    if ($missing.Count) { Exit-Engine -Code 4 -Outcome 'needs-consent' -MessageId 'result.needs-consent' -Detail "missing: $($missing -join ', ')" }
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
        Write-Host ''
        Write-Host '   Windows must run in TEST MODE to load this driver. Test mode lets Windows load drivers that are' -ForegroundColor White
        Write-Host '   signed with a test certificate, and shows "Test Mode" on the desktop. Uninstall can turn it off.' -ForegroundColor White
        $ok = Read-Confirmation -Question 'Turn on test signing (bcdedit /set testsigning on)?' -Answer $(if ($AcceptTestSigning) { 'YES' } else { $null })
        if (-not $ok) {
            Write-Host 'Stopped: test signing not confirmed. Nothing else was changed.' -ForegroundColor Yellow
            if (-not $script:DryRunMode) { Save-Phase 'new' }
            $script:EngineConsents = @('test-signing')
            Exit-Engine -Code 4 -Outcome 'needs-consent' -MessageId 'result.needs-consent' -Detail 'test signing not confirmed'
        }
        if ($script:BitLockerState -eq 'on') {
            $choice = $BitLocker
            if (-not $choice) {
                if (Read-Confirmation -Question 'BitLocker is on. Do you have the recovery key for this drive (it will be asked for after the restart)?' -Answer $null) { $choice = 'HaveKey' }
                elseif (Read-Confirmation -Question 'Suspend BitLocker for the next two restarts instead?' -Answer $null) { $choice = 'Suspend' }
            }
            if (-not $choice) {
                Write-Host 'Stopped: BitLocker not handled. Nothing else was changed.' -ForegroundColor Yellow
                $script:EngineConsents = @('bitlocker')
                Exit-Engine -Code 4 -Outcome 'needs-consent' -MessageId 'result.needs-consent' -Detail 'BitLocker not handled'
            }
            if ($choice -eq 'Suspend') {
                Invoke-Change "suspend BitLocker on $($env:SystemDrive) for two restarts" { Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 2 | Out-Null } | Out-Null
            }
            Set-StateValue $state 'bitlocker' $choice
        }
        # The continuation of this install runs after the restart from the staged closure.
        Initialize-Closure
        Invoke-Change 'bcdedit /set {current} testsigning on' {
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
    }
}
if ($state.phase -eq 'testsigning-pending') {
    if (-not $script:TestSigningActive) {
        # This run's package and inputs are kept, and the run after the restart is this package's: a newer package
        # that took over an older one's phase 1 continues as itself, from its own closure.
        if (-not $script:DryRunMode) { Save-InstallState $state }
        Initialize-Closure
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
Enter-Stage 'firmware' 'GPU firmware'
# Every step below can run again over its own result: files are compared by SHA256 and a file in use is replaced
# by rename, registry values and the task are overwritten, pnputil accepts a package that is already there.
# The firmware comes first: every install downloads it again (or takes it from -FirmwareDir) into a staging folder and
# checks each SHA256 before phase 2 changes anything, so a failed download leaves the computer and the phase as they
# were.
$fw = $script:Manifest.firmware
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
# The closure before the driver package, which can end this run in a restart (driver-pending-restart).
Initialize-Closure
# The last safe point: from here to the end of phase 2 a cancel would leave a half-installed driver.
Invoke-CancelPoint 'before-driver-install'
Set-CancelAvailable $false 'driver-install'
$script:InPhase2 = $true
if ($state.phase -notin @('testsigning-active')) { Write-Info "continuing an earlier run (phase $($state.phase)): finished steps are skipped" }
Enter-Stage 'files' 'Files'

$cer = Join-Path $package 'payload\cert\amdgpu-wddm-release.cer'
$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
Set-StateValue $state 'cert_thumbprint' $cert.Thumbprint
Invoke-Change "add the release test certificate $($cert.Thumbprint) to LocalMachine Root and TrustedPublisher" {
    foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
} | Out-Null

# Files. Each payload directory goes to the same name under the install root.
$dirs = @('d3d12', 'desktop', 'd3d11', 'vulkan', 'tools')
if (-not $NoControlApp -and (Test-Path -LiteralPath (Join-Path $package 'payload\control'))) { $dirs += 'control' }
foreach ($d in $dirs) {
    $src = Join-Path $package "payload\$d"
    Invoke-Change "copy payload\$d -> $InstallRoot\$d (same SHA256: kept; in use: replaced by rename)" {
        Copy-TreeSafe -Source $src -Destination (Join-Path $InstallRoot $d)
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
# Facts about the computer before the install are recorded once and saved before the step that changes them.
$stub = Join-Path $env:windir 'System32\bc250umd.dll'
[void](Set-StateValueOnce $state 'stub_existed' (Test-Path -LiteralPath $stub))
$bc250Dir = Split-Path -Parent $script:FirmwareInstallDir
[void](Set-StateValueOnce $state 'bc250_dir_existed' (Test-Path -LiteralPath $bc250Dir))
$fwExisted = Set-StateValueOnce $state 'firmware_dir_existed' (Test-Path -LiteralPath $script:FirmwareInstallDir)
if (-not $script:DryRunMode) { Save-InstallState $state }
Invoke-Change "copy payload\system32\bc250umd.dll -> $stub (the D3D9 slot of UserModeDriverName; same SHA256: kept; in use: replaced by rename)" {
    $r = Copy-FileSafe -Source (Join-Path $package 'payload\system32\bc250umd.dll') -Destination $stub
    Write-Info "$stub`: $r"
} | Out-Null
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
Save-Phase 'files-copied'
Enter-Stage 'driver' 'Driver package'

# The driver settings as they are before the driver package: pnputil runs the INF's AddReg, which writes 0 into every
# gate and counter it names. The upgrade rule below judges these values, never the INF's zeros. Kept in the state, so a
# re-run after a failure past pnputil still judges the values from before the first pnputil of this install.
$infParameterNames = Get-InfParameterNames (Join-Path $package 'payload\kmd\bc250kmd.inf')
$judgedNames = @(@($infParameterNames) + @((ConvertTo-PairList (Get-Content -LiteralPath (Join-Path $here 'registry-defaults.json') -Raw | ConvertFrom-Json).defaults.parameters) | ForEach-Object { $_.Name })) | Select-Object -Unique
$parametersBefore = @{}
if ($state.PSObject.Properties['parameters_before_install'] -and ($null -ne $state.parameters_before_install)) {
    foreach ($p in $state.parameters_before_install.PSObject.Properties) { $parametersBefore[$p.Name] = $p.Value }
    Write-Info "driver settings from before the first driver package install of this run: $($parametersBefore.Count) values"
} else {
    $all = Read-RegistryValues $script:ParametersKey
    foreach ($n in $judgedNames) { if ($all.ContainsKey($n)) { $parametersBefore[$n] = $all[$n] } }
    Set-StateValue $state 'parameters_before_install' ([pscustomobject]$parametersBefore)
    Save-InstallState $state
    Write-Info "driver settings before the driver package: $($parametersBefore.Count) of $(@($judgedNames).Count) values present"
}

# The driver package. Its INF carries the Reboot directive (build-release.ps1): Windows installs the package but does
# not restart a GPU that is already started (on Microsoft Basic Display or on the previous release), so the desktop
# and DWM keep their device for the rest of this session (BD-060, common.ps1), and the GPU changes driver at the next
# restart. A GPU that is not started can be installed at once, with the INF's closed gates (display only). pnputil's
# exit code says only that the operation completed, with or without a restart required (Get-DriverPackageOutcome):
# the binding check below decides. Nothing here stops or restarts DWM, and nothing touches the KMD's own state (the
# BD-059 session marker included); the registry below opens the gates for the next start.
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
if (-not (Test-InfDefersDeviceRestart ([IO.File]::ReadAllLines($infFile)))) { throw 'payload\kmd\bc250kmd.inf has no Reboot directive: this package would restart the GPU under the running desktop' }
$pnp = Invoke-Change 'pnputil /add-driver payload\kmd\bc250kmd.inf /install (the GPU changes to it at the next restart)' {
    $n = Invoke-Native pnputil.exe @('/add-driver', $infFile, '/install')
    Write-Log $n.text
    # 259 is also what a re-run over an earlier run's install sees.
    $o = Get-DriverPackageOutcome $n.code
    if (-not $o.ok) { throw "pnputil failed ($($n.code)): $($n.text)" }
    Write-Info "pnputil: $($o.text)"
    return $o
}
$instance = $null
if ($script:Device) { $instance = $script:Device.DeviceID }
# Right after the driver package, before any branch: the observation covers the restart-pending case too.
if ($dwmSession) {
    try {
        $dwmAfter = Get-SessionDwm $dwmSession
        $devNow = $null
        if ($instance) { $devNow = Get-PnpDevice -InstanceId $instance -ErrorAction SilentlyContinue }
        $devText = $(if ($pnp) { "$($pnp.text); device status $(if ($devNow) { $devNow.Status } else { 'unknown' })" } else { 'not run (dry run)' })
        Add-DwmObservation 'after the driver package' $dwmAfter $devText
        Write-Info "after the driver package: device: $devText; DWM: $(Compare-DwmReadings $dwmBefore $dwmAfter)"
    } catch { Write-Log "   DWM reading: $($_.Exception.Message)" }
}
$classKey = $null
if (-not $script:DryRunMode) {
    $svc = Get-DeviceServiceName -InstanceId $instance
    $script:CurrentStep = 'check that the GPU is bound to the new driver package'
    if ($svc -ne $script:ServiceName -and $pnp.deferred) {
        # A restart is required to finish the install. If the device's registry still names the old service, phase 2
        # finishes after the restart instead of writing into the old driver's key; the inputs of this install stay in
        # the state for that argument-free run.
        Write-Info "the GPU is still on '$svc' until the restart: phase 2 continues after the next logon"
        Save-Phase 'driver-pending-restart'
        Set-ResumeAtLogon 'continue'
        Request-Restart 'the GPU changes to the new driver package at the next start.' 'restart.driver-package'
        Exit-Engine -Code 0 -Outcome 'restart-required' -MessageId 'result.restart-driver'
    }
    if ($svc -ne $script:ServiceName) { throw "the GPU is on '$svc' after pnputil, not on $($script:ServiceName)" }
    $classKey = Get-DeviceDriverKey -InstanceId $instance
    if (-not $classKey) { throw 'no software key for the GPU' }
    Set-StateValue $state 'class_key' $classKey
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
function Invoke-RegistryDefaults([string]$Key, $Defaults, $Previous, [hashtable]$Explicit = @{}, $Owned = $null, [hashtable]$Before = $null, [string[]]$Restore = @()) {
    $now = Read-RegistryValues $Key
    if ($null -eq $Before) { $plan = Get-RegistryDefaultPlan -Defaults $Defaults -Previous $Previous -Current $now -Explicit $Explicit -Owned $Owned }
    else { $plan = Get-RegistryDefaultPlan -Defaults $Defaults -Previous $Previous -Current $Before -Explicit $Explicit -Owned $Owned -After $now -Restore $Restore }
    Invoke-Change ("${Key}: " + (Format-RegistryPlan $plan)) { Write-RegistryPlan $Key $plan } | Out-Null
}
# Gates: the registered lab configuration (EnableFullWddm 2 opens it at every start). Clocks: load-driven DPM up to
# DpmMaxMHz, thermal limits are the driver's own. KeepLog 0: no log files on the tester's disk. UnconfirmedStarts 0 is
# the INF's own reset (a GPU that was not started has already counted one display-only start). Judged on the values
# from before pnputil; the INF's other values (EnableMmioWrite, EnableHangBugcheck) get their values from before
# pnputil back.
Invoke-RegistryDefaults $script:ParametersKey $regDefaults.defaults.parameters $applied.parameters $commandLineParameters ([ordered]@{ UnconfirmedStarts = 0 }) $parametersBefore $infParameterNames

# Graphics registration in the GPU's software key: D3D9/10/11 slots, D3D12 slot, Vulkan.
$umd = @('bc250umd.dll', (Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'), (Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'), (Join-Path $InstallRoot 'd3d12\amdgpu_wddm_d3d12.dll'))
$icdJson = Join-Path $InstallRoot 'vulkan\radeon_icd.json'
Invoke-Change ("$classKey UserModeDriverName = " + ($umd -join ' | ') + "; VulkanDriverName = $icdJson") {
    New-ItemProperty -LiteralPath $classKey -Name UserModeDriverName -Value ([string[]]$umd) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath $classKey -Name VulkanDriverName -Value ([string[]]@($icdJson)) -PropertyType MultiString -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:KhronosKey) '$icdJson' = 0 (system Vulkan ICD)" {
    Initialize-RegistryKey $script:KhronosKey
    New-ItemProperty -LiteralPath $script:KhronosKey -Name $icdJson -Value 0 -PropertyType DWord -Force | Out-Null
} | Out-Null
Set-StateValue $state 'khronos_value' $icdJson

# Router policy (HKLM\SOFTWARE\amdgpu-wddm). DesktopRouter DwmForceCpu 0 composes the desktop on the GPU route (zink);
# 1 is the kill switch to the CPU route (tester.1 to tester.8 shipped 1 until BD-058 was fixed). RequireKmdSwitches 1
# keeps the GPU route gated: when the KMD's effective interop switches are off, the router takes the CPU route by
# itself. AppRouter Mode gpu-default: D3D10.1/D3D11 applications run on the GPU UMD, except Deny, the sign-in
# processes and Windows components (blank or missing windows on the GPU UMD, BD-061; cause not established).
Invoke-RegistryDefaults "$($script:SoftwareKey)\DesktopRouter" $regDefaults.defaults.desktop_router $applied.desktop_router @{} ([ordered]@{ CpuUmdPath = (Join-Path $InstallRoot 'desktop\bc250d3d.dll') })
Invoke-RegistryDefaults "$($script:SoftwareKey)\AppRouter" $regDefaults.defaults.app_router $applied.app_router @{} ([ordered]@{ GpuUmdPath = (Join-Path $InstallRoot 'd3d11\amdgpu_wddm_d3d11.dll') })
# Application profiles: the shipped ones by the same rule; a tester's own profiles are other keys and stay as they are.
foreach ($app in ConvertTo-PairList $regDefaults.defaults.d3d12_applications) {
    $prevApp = $null
    if ($applied.d3d12_applications -and $applied.d3d12_applications.PSObject.Properties[$app.Name]) { $prevApp = $applied.d3d12_applications.($app.Name) }
    Invoke-RegistryDefaults "$($script:SoftwareKey)\D3D12\Applications\$($app.Name)" $app.Value $prevApp
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
$repairSetup = Join-Path $script:Closure $script:SetupExeRelative
Invoke-Change "$($script:SoftwareKey)\Release: RepairSetup = $repairSetup (when the repair set has a setup window)" {
    $k = "$($script:SoftwareKey)\Release"
    if (Test-Path -LiteralPath $repairSetup) { New-ItemProperty -LiteralPath $k -Name RepairSetup -Value $repairSetup -PropertyType String -Force | Out-Null }
    else { Remove-ItemProperty -LiteralPath $k -Name RepairSetup -ErrorAction SilentlyContinue }
} | Out-Null

# The start-confirm task: after every logon it confirms the boot's driver start, so the boot-loop guard does not
# fall back to Basic Display at the third start (see INSTALL.md, "The small blue window after logon").
$sc = Join-Path $InstallRoot 'tools\start-confirm.ps1'
Invoke-Change "scheduled task '$($script:TaskName)': at logon of an administrator, runs $sc (at most 2 minutes)" {
    $old = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
    if ($old) { Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$sc`""
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-544' -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $script:TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
} | Out-Null

# The control application: a Start menu entry, nothing resident.
$controlExe = $null
if ($script:Manifest -and $script:Manifest.control_app_exe) { $controlExe = Join-Path $InstallRoot ('control\' + $script:Manifest.control_app_exe) }
if ($controlExe -and $dirs -contains 'control') {
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\amdgpu-wddm Control.lnk'
    Invoke-Change "Start menu shortcut $lnk -> $controlExe" {
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($lnk); $s.TargetPath = $controlExe; $s.WorkingDirectory = (Split-Path $controlExe); $s.Save()
    } | Out-Null
    Set-StateValue $state 'shortcut' $lnk
    # The recovery view (docs/gui/interfaces.md section 4): it never loads bc250control.dll.
    $rlnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\amdgpu-wddm Control (recovery).lnk'
    Invoke-Change "Start menu shortcut $rlnk -> $controlExe --recovery" {
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($rlnk); $s.TargetPath = $controlExe; $s.Arguments = '--recovery'; $s.WorkingDirectory = (Split-Path $controlExe); $s.Save()
    } | Out-Null
} else { Write-Info 'control application: not in this package (or -NoControlApp); skipped' }

Clear-InstallInputs $state
Set-StateValue $state 'parameters_before_install' $null
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
    [void](Complete-RepairSet -Manifest $script:Manifest -Closure $script:Closure -FirmwareFiles @(@($fw.files) | ForEach-Object { Join-Path $script:FirmwareInstallDir $_.name }))
} | Out-Null
Save-Phase 'installed'
Set-ResumeAtLogon 'verify'
Write-Host ''
if ($script:DryRunMode) { Write-Host 'Dry run complete: every check ran, nothing was changed.' -ForegroundColor Green }
else { Write-Host 'Installation complete. After the restart, the installer verifies the driver by itself.' -ForegroundColor Green }
# A dry run reports the restart that the real run would need, and never restarts.
Request-Restart 'the driver starts with its full configuration at the next start.' 'restart.complete'
if ($script:DryRunMode) { Exit-Engine -Code 0 -Outcome 'completed' -MessageId 'result.dry-run-complete' }
Exit-Engine -Code 0 -Outcome 'restart-required' -MessageId 'result.installed-restart'
