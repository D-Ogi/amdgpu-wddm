# Shared functions of the tester installer (install.ps1, uninstall.ps1). Windows PowerShell 5.1 syntax only: the
# tester runs the copy of powershell.exe that ships with Windows 11, so no ternary, no '??', no '&&'.
#
# Every change to the system goes through Invoke-Change. In a dry run it prints what it would do and does nothing,
# so the dry run executes the same code path as the real run, checks included.

$script:ReleaseName      = 'amdgpu-wddm'
$script:ServiceName      = 'bc250kmd'
$script:HardwareIdPrefix = 'PCI\VEN_1002&DEV_13FE'          # bc250kmd.inf [Models.NTamd64.10.0...22631]
# The GPU's HD Audio function, the second function of the same silicon. It keeps the inbox HDAudBus driver; the
# installer changes one value of it (BD-092, Set-GpuAudioMsi below).
$script:AudioIdPrefix    = 'PCI\VEN_1002&DEV_13FF'
$script:AudioMsiValue    = 'MSISupported'
$script:AudioEnumRoot    = 'HKLM:\SYSTEM\CurrentControlSet\Enum'
$script:AudioRestartWaitSeconds = 60                           # pnputil /restart-device, and the device back in D0
# The program that restarts that function. A host test points this at a child of its own to drive the deadline of
# Restart-GpuAudioDevice without a real device (audit finding K3).
$script:PnpUtilPath      = 'pnputil.exe'
$script:DisplayClassGuid = '{4d36e968-e325-11ce-bfc1-08002be10318}'
$script:SoftwareKey      = 'HKLM:\SOFTWARE\amdgpu-wddm'
$script:ParametersKey    = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$script:KhronosKey       = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
# Windows' own graphics key. One value of it belongs to this release: TdrDelay, how long Windows waits for the
# graphics before it resets them (BD-079). Nothing else here is ours.
$script:GraphicsDriversKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers'
# The 32-bit Vulkan loader reads its drivers from the WOW64 view (BD-064); the installer runs as a 64-bit process.
$script:KhronosKeyWow    = 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers'
$script:RunOnceKey       = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
$script:RunOnceName      = 'amdgpu-wddm-installer'
$script:TaskName         = 'amdgpu-wddm start confirm'
$script:WindowTaskName   = 'amdgpu-wddm start window'          # BD-080: the part that needs a session, not rights
$script:FirmwareInstallDir      = 'C:\BC250\firmware'                 # compiled into the KMD (psp.c BC250_PSP_FIRMWARE_DIR)
$script:StateDir         = Join-Path $env:ProgramData 'amdgpu-wddm\installer'
$script:StatePath        = Join-Path $script:StateDir 'state.json'
$script:ProfileListKey   = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
$script:ProfilePathsOverride = $null                           # host tests: fixture profile folders
$script:DryRunMode       = $false
# The visible countdown before a restart the installer makes (owner, 2026-10-12). The same 15 s as the setup
# window's restart screen (tools\win\amdgpu_wddm_setup\src\InstallFlow.cs, RestartCountdownSeconds).
$script:RestartCountdownSeconds = 15
$script:PlannedRestarts  = 0                                   # install.ps1: the restarts the decision counted
$script:LogPath          = $null
$script:CurrentStep      = '(before the first change)'
$script:ScheduleOldCopies = $true                              # host tests set $false: no MoveFileEx on the test PC
$script:GuiMode          = $false                              # engine.ps1: the setup window drives this run
$script:Mutated          = $false                              # an Invoke-Change action ran (a change to the system)
$script:OnFirstChange    = $null                               # engine.ps1: runs once, before the first change
$script:OnChange         = $null                               # engine.ps1: one event per change (the description)
$script:OnMutation       = $null                               # engine.ps1: each later change of a real run
# Host tests only: no network at all (G-OFF). Every download and host probe fails as if the PC were offline.
$script:NetworkBlocked   = [bool]$env:AMDGPU_WDDM_TEST_NO_NETWORK

function Write-Step([string]$Text)  { Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan; Write-Log "== $Text" }
function Write-Info([string]$Text)  { Write-Host "   $Text"; Write-Log "   $Text" }
function Write-Warn2([string]$Text) { Write-Host "   WARNING: $Text" -ForegroundColor Yellow; Write-Log "   WARNING: $Text" }
function Write-Fail([string]$Text)  { Write-Host "   FAIL: $Text" -ForegroundColor Red; Write-Log "   FAIL: $Text" }
function Write-Log([string]$Text) {
    if (-not $script:LogPath) { return }
    try { [IO.File]::AppendAllText($script:LogPath, ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') + ' ' + $Text + "`r`n")) } catch { }
}

# One change to the system. $Action runs only in a real run; a dry run prints the description.
function Invoke-Change {
    param([Parameter(Mandatory)][string]$Description, [Parameter(Mandatory)][scriptblock]$Action)
    $script:CurrentStep = $Description
    if ($script:OnChange) { & $script:OnChange $Description }
    if ($script:DryRunMode) { Write-Host "   [dry run] would: $Description" -ForegroundColor DarkYellow; return $null }
    if (-not $script:Mutated) {
        if ($script:OnFirstChange) { & $script:OnFirstChange }
        $script:Mutated = $true
    } elseif ($script:OnMutation) { & $script:OnMutation }
    Write-Info "doing: $Description"
    return (& $Action)
}
# The message for a step that threw: which step, why, and that a re-run with the same package finishes the job.
function Write-StepFailure($ErrorRecord) {
    Write-Fail "stopped at step: $($script:CurrentStep)"
    Write-Fail "$($ErrorRecord.Exception.Message) $($ErrorRecord.InvocationInfo.PositionMessage)"
    Write-Host ''
    Write-Host 'Nothing after this step ran. The steps before it are done and stay done.' -ForegroundColor Yellow
    Write-Host 'Fix the cause if the message names one, then run install.cmd again from the same package folder:' -ForegroundColor Yellow
    Write-Host 'it skips what is already in place and continues from here.' -ForegroundColor Yellow
    if ($script:LogPath) { Write-Host "Log: $($script:LogPath)" -ForegroundColor Yellow }
}
# The certificate step when a store refuses (BD-125: E_ACCESSDENIED in an elevated run on a tester's PC). The error
# names the store, keeps the HRESULT, and gives the commands that add the certificate by hand. Add-CertificateToStore
# below has already tried certutil when this error is made, and a run after the cause is fixed continues from here.
function New-CertificateStoreError {
    param([Parameter(Mandatory)][string]$Store, [Parameter(Mandatory)][string]$Certificate,
          [Parameter(Mandatory)][Exception]$Exception, [string[]]$Done = @(), [string[]]$Remaining = @(),
          [string]$Tried = '')
    $hr = $Exception.HResult
    $code = '0x{0:X8}' -f $hr
    $names = @{ '0x80070005' = 'E_ACCESSDENIED' }
    if ($names.ContainsKey($code)) { $code += " ($($names[$code]))" }
    $text = "Windows refused to add the certificate to LocalMachine\$Store, HRESULT $code."
    if ($Done.Count) { $text += " LocalMachine\$($Done -join ' and LocalMachine\') already has it." }
    $text += ' Setup runs elevated, so Windows itself refused the change. The usual causes are antivirus protection' +
        ' of the certificate store or a policy that locks the store.'
    if ($Tried) { $text += " Setup also tried: $Tried." }
    $commands = @(@($Store) + @($Remaining) | ForEach-Object { "certutil -addstore -f $_ `"$Certificate`"" })
    $text += " To add it by hand, run in an administrator command prompt: $($commands -join ' and then ')."
    $text += ' Then run setup again: it continues from here.'
    $text += " Windows said: $($Exception.Message)"
    return (New-Object Runtime.InteropServices.COMException -ArgumentList $text, $hr)
}
# The release certificate into one LocalMachine store (BD-125). Import-Certificate first. When Windows refuses it,
# certutil -addstore -f: on a tester's PC (tester.29, 2026-10-13) Import-Certificate got E_ACCESSDENIED for
# TrustedPublisher and certutil added the same certificate to the same store in the same elevated session. Only the
# store decides the result: the thumbprint must be in it afterwards. certutil's exit code and text are logged, never
# parsed (the text is translated). Result: ok, via ('Import-Certificate', 'certutil' or 'none'), detail, and the
# exception of Import-Certificate. The three actions are parameters for the host test (test-engine-units.ps1).
function Add-CertificateToStore {
    param([Parameter(Mandatory)][string]$Store, [Parameter(Mandatory)][string]$Certificate,
          [Parameter(Mandatory)][string]$Thumbprint,
          [scriptblock]$Import = { param($File, $Name) Import-Certificate -FilePath $File -CertStoreLocation "Cert:\LocalMachine\$Name" -ErrorAction Stop | Out-Null },
          [scriptblock]$CertUtil = { param($File, $Name) Invoke-NativeBounded -File (Join-Path $env:SystemRoot 'System32\certutil.exe') -Arguments @('-addstore', '-f', $Name, $File) -TimeoutSeconds 60 },
          [scriptblock]$Present = { param($Name, $Thumb) @(Get-ChildItem "Cert:\LocalMachine\$Name" -ErrorAction Stop | Where-Object { $_.Thumbprint -eq $Thumb }).Count -gt 0 })
    function Result([bool]$Ok, [string]$Via, [string]$Detail, $Exception) { [pscustomobject]@{ ok = $Ok; via = $Via; detail = $Detail; exception = $Exception } }
    try { & $Import $Certificate $Store; return (Result $true 'Import-Certificate' "LocalMachine\$Store`: added" $null) }
    catch { $refused = $_.Exception }
    $why = "Import-Certificate was refused (HRESULT $('0x{0:X8}' -f $refused.HResult))"
    try {
        $r = & $CertUtil $Certificate $Store
        $ran = if ($r.timed_out) { 'certutil did not finish in 60 s' } else { "certutil exit code $($r.code)" }
    } catch { $ran = "certutil did not run: $($_.Exception.Message)" }
    $held = $false
    try { $held = [bool](& $Present $Store $Thumbprint) } catch { $ran += "; the store could not be read: $($_.Exception.Message)" }
    if ($held) { return (Result $true 'certutil' "LocalMachine\$Store`: $why, added with certutil ($ran)" $refused) }
    return (Result $false 'none' "LocalMachine\$Store`: $why, and certutil did not add it ($ran)" $refused)
}

# A native program, its stdout and stderr as one text, and its exit code. Windows PowerShell 5.1 turns a stderr line
# of a native program into a terminating error under ErrorActionPreference Stop; here it stays text.
function Invoke-Native {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @())
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& $File @Arguments 2>&1 | ForEach-Object { [string]$_ }) -join "`n" } finally { $ErrorActionPreference = $old }
    return @{ text = $text; code = $LASTEXITCODE }
}
# The same, under a deadline, for a step whose budget must cover the program as well as the waiting after it
# (audit finding K3). Invoke-Native above is an ordinary blocking pipeline: it has no timeout, so a program that
# never returns holds the install or the uninstall for ever, whatever the caller's budget says.
#
# Both output streams are read as they arrive, so a program that writes more than a pipe holds cannot stop on a
# reader that is in fact waiting for its exit, and only $MaxOutputChars of the text come back.
#
# A program that passes the deadline is NOT killed. Terminating pnputil in the middle of a kernel PnP transition
# does not roll that transition back: the device is left wherever the kernel got to, and a kill would only hide
# that. The caller gets timed_out = $true, the process id that is still running and the output up to that
# moment, and decides what to tell the user. What the program writes after the deadline is not collected.
function Invoke-NativeBounded {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @(),
          [Parameter(Mandatory)][double]$TimeoutSeconds, [int]$MaxOutputChars = 8192)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $quoted = @(foreach ($a in @($Arguments)) {
            if ($a -eq '' -or $a -match '[^A-Za-z0-9_.:\\/=,+-]') { '"' + ($a -replace '(\\+)$', '$1$1') + '"' } else { $a }
        })
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ($quoted -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $text = New-Object Text.StringBuilder
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $psi
    # Both streams are read as they arrive, so a program that writes more than a pipe holds cannot stop on a
    # reader that is in fact waiting for its exit. The handler takes the lines of both streams in one buffer.
    $sink = { if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
    $subscriptions = @(
        (Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action $sink -MessageData $text),
        (Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action $sink -MessageData $text))
    $code = $null
    $id = $null
    $timedOut = $false
    try {
        [void]$p.Start()
        $id = $p.Id
        $p.BeginOutputReadLine()
        $p.BeginErrorReadLine()
        $ms = [int][Math]::Max(0.0, [Math]::Min(2147483.0, [double]$TimeoutSeconds) * 1000.0)
        if ($p.WaitForExit($ms)) { $code = $p.ExitCode } else { $timedOut = $true }
    } finally {
        foreach ($s in $subscriptions) { Unregister-Event -SubscriptionId $s.Id -ErrorAction SilentlyContinue }
        $p.Dispose()
    }
    $out = ([string]$text.ToString() -replace "`r`n", "`n").Trim()
    if ($out.Length -gt $MaxOutputChars) { $out = $out.Substring(0, $MaxOutputChars) + "`n[output cut]" }
    return @{ text = $out; code = $code; timed_out = $timedOut; id = $id
        seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1) }
}

# Restarts the calling script elevated (UAC prompt) and ends this process with exit code 10. Started from a .cmd
# launcher (AMDGPU_WDDM_LAUNCHER = its full path), the launcher itself is elevated, so the elevated run keeps the
# launcher's console and its final pause: one result window. The launcher skips its own pause on exit code 10, so
# the unelevated window closes at once. Never called in a dry run.
function Invoke-SelfElevation {
    param([Parameter(Mandatory)][string]$ScriptPath, [Parameter(Mandatory)]$Bound)
    if ($script:DryRunMode) { throw 'internal error: elevation requested in a dry run' }
    $args2 = @()
    # The launcher adds its own fixed arguments again (verify.cmd: -Verify); pass only the others.
    $fixed = @(([string]$env:AMDGPU_WDDM_LAUNCHER_FIXED) -split ',' | Where-Object { $_ })
    foreach ($k in $Bound.Keys) {
        if ($env:AMDGPU_WDDM_LAUNCHER -and $fixed -contains $k) { continue }
        $v = $Bound[$k]
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v) { $args2 += "-$k" } }
        else { $args2 += "-$k"; $args2 += ('"' + [string]$v + '"') }
    }
    Write-Host 'Administrator rights are needed: Windows will ask for them now.'
    $launcher = $env:AMDGPU_WDDM_LAUNCHER
    if ($launcher -and (Test-Path -LiteralPath $launcher)) {
        if ($args2.Count) { Start-Process -FilePath $launcher -ArgumentList $args2 -Verb RunAs | Out-Null }
        else { Start-Process -FilePath $launcher -Verb RunAs | Out-Null }
    } else {
        Start-Process -FilePath powershell.exe -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', ('"' + $ScriptPath + '"')) + $args2) -Verb RunAs | Out-Null
    }
    exit 10
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Nothing here asks the person a question (owner, 2026-10-12: "Przestan wymagac potwierdzen od uzytkownika").
# Starting install.cmd, or pressing Install in the window, IS the answer for everything an installation of this
# driver needs; uninstall.cmd is the answer for the removal. The function that asked - Read-Confirmation - is gone,
# so a new question cannot be added by calling it, and the tests of tools\release (test-boot-locale.ps1) refuse a
# call to it on the way to the boot change. A choice with two good outcomes is a switch on the command line with a
# stated default, never a prompt; a refusal that protects the machine is not a question and stays.

# ---------------------------------------------------------------------------------------------------------------
# State kept between the phases (and for uninstall): %ProgramData%\amdgpu-wddm\installer\state.json.
function Read-InstallState {
    if (-not (Test-Path -LiteralPath $script:StatePath)) { return $null }
    return (Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json)
}
# The state folder holds the logs and verify results that testers attach to bug reports: administrators and SYSTEM
# full control, users read, the same for everything inside it (existing files included), so a tester can read and
# attach them without elevation whatever the folder held before.
function Set-StateDirAccess {
    if ($script:DryRunMode) { return }
    try {
        $n = Invoke-Native icacls.exe @($script:StateDir, '/inheritance:r', '/grant:r', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX', '/Q')
        if ($n.code -ne 0) { Write-Warn2 "icacls $($script:StateDir): $($n.text)"; return }
        if (-not @(Get-ChildItem -LiteralPath $script:StateDir -Force -ErrorAction SilentlyContinue).Count) { return }
        $n = Invoke-Native icacls.exe @((Join-Path $script:StateDir '*'), '/reset', '/T', '/C', '/Q')
        if ($n.code -ne 0) { Write-Warn2 "icacls /reset under $($script:StateDir): $($n.text)" }
    } catch { Write-Warn2 "state folder access not set: $($_.Exception.Message)" }
}
function Save-InstallState($State) {
    if ($script:DryRunMode) { return }
    [void][IO.Directory]::CreateDirectory($script:StateDir)
    [IO.File]::WriteAllText($script:StatePath, ($State | ConvertTo-Json -Depth 6))
}
# Creates a registry key if it is missing and leaves an existing one alone. Never `New-Item -Force` on a key that may
# exist: the registry provider then deletes and recreates it, with all its values and subkeys (measured under 5.1).
# That wiped the KMD's own state in Services\bc250kmd\Parameters, every other RunOnce entry and every other Vulkan
# driver registration up to tester.5.
function Initialize-RegistryKey([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { New-Item -Path $Path -Force | Out-Null }
}
function Set-StateValue($State, [string]$Name, $Value) {
    if ($State.PSObject.Properties[$Name]) { $State.$Name = $Value } else { $State | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
}
# What a run of install.ps1 does with what is already on the computer. Pure: the state, the package version and the
# installed version in, the action out. Testers get new packages often, so a different version always installs.
#   install   no state, or phase 1 not finished: the normal run
#   upgrade   another version is installed (any phase): phase 2 again, files that did not change are kept
#   resume    the same version waits for the restart that its driver package asked for: phase 2 continues
#   repair    the same version in an unfinished or failed phase, or -Repair: phase 2 again
#   verify    the same version installed and waiting for its restart (phase 'installed'): phase 3 only
#   already   the same version verified: nothing to do (exit 0)
function Get-InstallAction {
    param($State, [string]$PackageVersion, [string]$InstalledVersion, [bool]$Repair)
    if (-not $State) { return [ordered]@{ action = 'install'; message = $null } }
    $phase = [string]$State.phase
    if ($phase -in @('new', 'testsigning-pending', 'testsigning-active')) { return [ordered]@{ action = 'install'; message = $null } }
    if (-not $InstalledVersion) { $InstalledVersion = '(unknown version)' }
    if ($InstalledVersion -ne $PackageVersion) {
        return [ordered]@{ action = 'upgrade'; message = "upgrading $InstalledVersion -> $PackageVersion (installed phase $phase)" }
    }
    if ($phase -eq 'driver-pending-restart') { return [ordered]@{ action = 'resume'; message = "continuing the installation of $PackageVersion after the driver package restart" } }
    if ($Repair) { return [ordered]@{ action = 'repair'; message = "repairing $PackageVersion (-Repair, phase $phase)" } }
    if ($phase -eq 'verified') { return [ordered]@{ action = 'already'; message = "$PackageVersion is already installed and verified. Run verify.cmd to check it again, install.cmd -Repair to install it again, or uninstall.cmd to remove it." } }
    if ($phase -eq 'installed') { return [ordered]@{ action = 'verify'; message = $null } }
    return [ordered]@{ action = 'repair'; message = "repairing $PackageVersion (phase $phase)" }
}

# GUI plan A2: the one decision whether the restart that a pending phase waits for has happened. Every pending phase
# (test signing set, driver package waiting, installation complete) records the boot it was saved in
# (restart_boot_id). It advances only on positive evidence of a new boot: that boot is known, the current BootId is
# readable, and the two differ. Returns $null then (or when the phase is not pending); else why the restart is still
# pending: 'same-boot', 'saved-unknown' (a state without restart_boot_id) or 'current-unknown' (BootId unreadable).
$script:PendingPhases = @('testsigning-pending', 'driver-pending-restart', 'installed')
function Get-PendingRestart($State, $Boot) {
    if (-not $State -or [string]$State.phase -notin $script:PendingPhases) { return $null }
    if (-not $Boot -or $null -eq $Boot.boot_id) { return 'current-unknown' }
    $saved = $null
    if ($State.PSObject.Properties['restart_boot_id'] -and $null -ne $State.restart_boot_id) { try { $saved = [int64]$State.restart_boot_id } catch { $saved = $null } }
    if ($null -eq $saved) { return 'saved-unknown' }
    if ($saved -eq [int64]$Boot.boot_id) { return 'same-boot' }
    return $null
}

# The inputs of an install that phase 2 needs: the firmware folder (-FirmwareDir), the driver settings given on the
# command line (-DpmMaxMHz, -CuMode) and the switches that select what is installed and how (-NoControlApp, -NoReboot,
# -Force). The run after a restart that the installer asked for (test signing, or a driver package that waits for the
# restart) starts from RunOnce without arguments: it takes them from the state, if the state is of the same package
# version. A value given on its own command line wins. Every run saves its effective inputs; phase 2 clears them when
# it completes.
$script:ResumePhases = @('testsigning-pending', 'driver-pending-restart')
# -Repair is one of them (BD-069): the run after the restart has to know that this install is a repair, or it keeps a
# closure that the tester asked to reopen.
$script:InstallSwitches = @('NoControlApp', 'NoReboot', 'Force', 'Repair', 'ApplySystemTuning', 'NoStartMenuShortcut', 'NoDesktopShortcut', 'NoWallpaper')
# A switch of the list above, and the switch that clears it explicitly. A choice the setup window or a script makes
# must be able to turn a saved choice off again, not only on: without the second switch an install that once left
# the desktop icon out would leave it out for ever, because the state keeps the first answer over every later run.
# The Windows tuning has had this pair from the start (-ApplySystemTuning / -SkipSystemTuning).
$script:InstallSwitchClears = @{ ApplySystemTuning = 'SkipSystemTuning'; NoStartMenuShortcut = 'StartMenuShortcut'; NoDesktopShortcut = 'DesktopShortcut'; NoWallpaper = 'Wallpaper' }
function Get-InstallInputs($Bound, $State, [string]$PackageVersion) {
    foreach ($pair in $script:InstallSwitchClears.GetEnumerator()) {
        if ($Bound.ContainsKey($pair.Key) -and [bool]$Bound[$pair.Key] -and $Bound.ContainsKey($pair.Value) -and [bool]$Bound[$pair.Value]) {
            throw "-$($pair.Key) and -$($pair.Value) cannot be used together"
        }
    }
    $resume = $State -and ([string]$State.phase -in $script:ResumePhases) -and ([string]$State.package_version -eq $PackageVersion)
    $resumeTuning = $State -and ([string]$State.package_version -eq $PackageVersion) -and
        ([string]$State.phase -in @('new', 'testsigning-pending', 'testsigning-active', 'files-copied',
            'driver-pending-restart', 'driver-installed', 'install-incomplete'))
    $in = [pscustomobject]@{ firmware_dir = $null; component_dir = $null; components = $null; models = $null; parameters = @{}; switches = @(); restored = @() }
    if ($Bound.ContainsKey('FirmwareDir') -and $Bound['FirmwareDir']) { $in.firmware_dir = [string]$Bound['FirmwareDir'] }
    elseif ($resume -and $State.firmware_source_dir) { $in.firmware_dir = [string]$State.firmware_source_dir; $in.restored += "-FirmwareDir $($in.firmware_dir)" }
    if ($Bound.ContainsKey('ComponentDir') -and $Bound['ComponentDir']) { $in.component_dir = [string]$Bound['ComponentDir'] }
    elseif ($resume -and $State.component_source_dir) { $in.component_dir = [string]$State.component_source_dir; $in.restored += "-ComponentDir $($in.component_dir)" }
    # The component and model answers of the first run: the questions are asked once, before any change, and the
    # argument-free continuation after a restart takes them from the state. $null means "not answered yet".
    if ($resume -and $null -ne $State.PSObject.Properties['components_chosen'] -and $null -ne $State.components_chosen) {
        $in.components = @($State.components_chosen)
        $in.restored += "components: $(if (@($in.components).Count) { @($in.components) -join ',' } else { 'none' })"
    }
    if ($resume -and $null -ne $State.PSObject.Properties['models_chosen'] -and $null -ne $State.models_chosen) {
        $in.models = @($State.models_chosen)
        $in.restored += "models: $(if (@($in.models).Count) { @($in.models) -join ',' } else { 'none' })"
    }
    if ($Bound.ContainsKey('DpmMaxMHz')) { $in.parameters['DpmMaxMHz'] = [int]$Bound['DpmMaxMHz'] }
    if ($Bound.ContainsKey('CuMode') -and [int]$Bound['CuMode']) { $in.parameters['CuMode'] = [int]$Bound['CuMode'] }
    if (-not $in.parameters.Count -and $resume -and $State.command_line_parameters) {
        foreach ($p in $State.command_line_parameters.PSObject.Properties) { $in.parameters[$p.Name] = [int]$p.Value; $in.restored += "-$($p.Name) $([int]$p.Value)" }
    }
    foreach ($s in $script:InstallSwitches) {
        $clear = [string]$script:InstallSwitchClears[$s]
        if ($clear -and $Bound.ContainsKey($clear) -and [bool]$Bound[$clear]) { continue }
        if ($Bound.ContainsKey($s)) { if ([bool]$Bound[$s]) { $in.switches += $s } }
        elseif (($resume -or ($s -eq 'ApplySystemTuning' -and $resumeTuning)) -and (@($State.install_switches) -contains $s)) { $in.switches += $s; $in.restored += "-$s" }
    }
    return $in
}
function Save-InstallInputs($State, $Inputs) {
    Set-StateValue $State 'firmware_source_dir' $Inputs.firmware_dir
    Set-StateValue $State 'component_source_dir' $Inputs.component_dir
    if ($null -ne $Inputs.components) { Set-StateValue $State 'components_chosen' @($Inputs.components) }
    if ($null -ne $Inputs.models) { Set-StateValue $State 'models_chosen' @($Inputs.models) }
    Set-StateValue $State 'command_line_parameters' $(if ($Inputs.parameters.Count) { [pscustomobject]$Inputs.parameters } else { $null })
    Set-StateValue $State 'install_switches' $(if (@($Inputs.switches).Count) { @($Inputs.switches) } else { $null })
}
# Another package that continues an unfinished install (phase 1 not finished, Get-InstallAction 'install') takes it
# over: the state names this package from now on, so the inputs it saves come back for its own argument-free
# continuation. The phases stay as they are: test signing is still checked and finished first. Returns the version that
# the state named before, or $null when nothing was taken over.
function Set-InstallPackage($State, [string]$PackageVersion) {
    $p = $State.PSObject.Properties['package_version']
    if (-not $p -or -not $p.Value -or ([string]$p.Value -eq $PackageVersion)) { return $null }
    $was = [string]$p.Value
    Set-StateValue $State 'package_version' $PackageVersion
    return $was
}
function Clear-InstallInputs($State) {
    foreach ($n in 'firmware_source_dir', 'component_source_dir', 'components_chosen', 'models_chosen', 'command_line_parameters', 'install_switches') { Set-StateValue $State $n $null }
}

# A fact about the computer before the install (did this file exist?): the first run records it, a re-run over a
# partial install keeps it, because by then the installer itself made the file exist.
function Set-StateValueOnce($State, [string]$Name, $Value) {
    if ($null -eq $State.PSObject.Properties[$Name]) { $State | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
    return $State.$Name
}

# ---------------------------------------------------------------------------------------------------------------
# The HIP runtime (experimental) in <install root>\tools\hip is on by default: install.ps1 appends its folder to the
# machine PATH, so an amdhip64.dll that comes earlier (AMD's own in System32, or an earlier PATH entry) still wins, and
# uninstall.ps1 removes that one entry and no other. The value is read and written as Windows stores it (REG_EXPAND_SZ,
# %SystemRoot% not expanded). A new process sees the entry after the restart that the install asks for.
$script:EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
# Host tests only: the hive the two functions below open. A test points it at HKEY_CURRENT_USER with a scratch
# $script:EnvironmentKey, so no test of ours can write the machine PATH of the computer it runs on.
$script:EnvironmentHive = $null
function Get-EnvironmentHive {
    if ($null -ne $script:EnvironmentHive) { return $script:EnvironmentHive }
    return [Microsoft.Win32.Registry]::LocalMachine
}
function Get-HipPathEntry([string]$Root) { return (Join-Path $Root 'tools\hip') }
# Pure: is $Entry one of the entries of $PathValue (case and a trailing backslash do not count)?
function Test-PathEntry([string]$PathValue, [string]$Entry) {
    $want = $Entry.TrimEnd('\')
    foreach ($e in @($PathValue -split ';')) { if ($e.Trim().TrimEnd('\') -ieq $want) { return $true } }
    return $false
}
# Pure: $PathValue with $Entry at the end, or unchanged when it is there already.
function Add-PathEntry([string]$PathValue, [string]$Entry) {
    if (Test-PathEntry $PathValue $Entry) { return $PathValue }
    $v = $PathValue.TrimEnd(';')
    if ($v) { return "$v;$Entry" }
    return $Entry
}
# Pure: $PathValue without $Entry. Every other entry stays as it is written, empty ones included.
function Remove-PathEntry([string]$PathValue, [string]$Entry) {
    $want = $Entry.TrimEnd('\')
    return ((@($PathValue -split ';') | Where-Object { $_.Trim().TrimEnd('\') -ine $want }) -join ';')
}
function Get-MachinePath {
    $k = (Get-EnvironmentHive).OpenSubKey($script:EnvironmentKey, $false)
    try {
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        if ($k.GetValueNames() -contains 'Path') { $kind = $k.GetValueKind('Path') }
        return [pscustomobject]@{ value = [string]$k.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames); kind = $kind }
    } finally { $k.Close() }
}
function Set-MachinePath([string]$Value, $Kind) {
    $k = (Get-EnvironmentHive).OpenSubKey($script:EnvironmentKey, $true)
    try { $k.SetValue('Path', $Value, $Kind) } finally { $k.Close() }
}

# ---------------------------------------------------------------------------------------------------------------
# Registry defaults. installer\registry-defaults.json holds the one table of defaults; build-release.ps1 copies it into
# manifest.json ("defaults"), where the control application's reset reads it. An upgrade keeps what the tester changed:
#   set       the value is absent: write the default
#   same      the value equals the default: nothing to do
#   update    the value equals what the previous installer wrote (Release\AppliedDefaults, or legacy_applied for
#             tester.1 to tester.7, which kept no record): the tester did not change it, write the new default
#   kept      any other value: the tester's (or the control application's) setting, left alone
#   command   given on the install.cmd command line (-DpmMaxMHz, -CuMode): written
#   chosen    a person asked for this very value and the driver still holds it (Get-UserChoice below): kept
#   installer installer-owned (paths, counters): always written
#   restored  a value the driver package's INF writes that is not in the table: the value from before the install
#   reopened      the driver closed this switch itself and this install is a repair: the default is written again
#   driver-closed the driver closed this switch itself and this install is not a repair: the value stays, and the
#                 report names the closure and its remedy instead of calling it a setting of the tester (BD-069)
# The driver package (pnputil) writes 0 into every gate and counter of its AddReg: the decisions are taken on the values
# from before it (install.ps1 keeps them in the state), the writes then restore what it reset.
function ConvertTo-PairList($Map) {
    if ($null -eq $Map) { return @() }
    if ($Map -is [Collections.IDictionary]) { return @($Map.GetEnumerator() | ForEach-Object { [pscustomobject]@{ Name = [string]$_.Key; Value = $_.Value } }) }
    return @($Map.PSObject.Properties | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Value = $_.Value } })
}
function Test-RegistryValueSame($A, $B) {
    if (($null -eq $A) -or ($null -eq $B)) { return (($null -eq $A) -and ($null -eq $B)) }
    if (($A -is [array]) -or ($B -is [array])) { return ((@($A | ForEach-Object { [string]$_ }) -join "`n") -ieq (@($B | ForEach-Object { [string]$_ }) -join "`n")) }
    if (($A -is [string]) -or ($B -is [string])) { return ([string]$A -ieq [string]$B) }
    return ([int64]$A -eq [int64]$B)
}
function Format-RegistryValue($V) { if ($V -is [array]) { return '[' + (@($V | ForEach-Object { [string]$_ }) -join ', ') + ']' }; return [string]$V }

# The driver's own safety closures (BD-069). The KMD writes a release default away itself after a start it must not
# repeat, and leaves a record of that act in the same key. Without the record the installer reads the 0 as a value the
# tester chose and keeps it, so a repair never brings the desktop back to the GPU.
#   EnableGpuPresentBlit, EnableCddDwmInterop   both written 0 with InteropClosedReason = the reason, after a boot
#       that died with the GPU desktop path in use (driver/kmd/interop.c InteropStart, driver/kmd/interop_policy.c
#       bc250_interop_decide: persist_close; reason 4 'unclean' is the only one the policy persists today). The next
#       start that finds a switch open again deletes the record itself (clear_closed), so a reopen writes the default
#       and deletes the record, the way the control application's "Reopen the GPU desktop path" does.
#   DpmMode   written 0 with DpmClosedReason 3 (an earlier DPM start was never confirmed), 4 (an earlier start ended
#       above the floor) or 8 (the governor gave up after repeated failed clock changes): the three callers of
#       driver/kmd/dpm.c PersistFallback (bc250_dpm_decide force_fixed for 3 and 4, DpmGiveUp for 8). From KMD
#       0.7.208.1 that record outlives its boot, because no later start overwrites it and only a start that reads a
#       DpmMode other than 0 deletes it (bc250_dpm_decide clear_closed). A reopen therefore writes DpmMode 1 and
#       deletes the record, the way it deletes InteropClosedReason.
#       A driver before 0.7.208.1 writes no DpmClosedReason, so DpmLastReason is read instead (legacy_record). That
#       one names the fallback only inside the boot that wrote it: the next start reads DpmMode 0, decides
#       BC250_DPM_REASON_NOT_REQUESTED (1) and writes that over the record (driver/kmd/dpm.c, StoreLogged
#       DPM_SETTING_LAST_REASON at every start with the SMU online). After one more start the installer cannot tell
#       such a driver's guard 0 from a 0 the tester wrote, and the value stays 'kept'. A reopen deletes nothing
#       there, because the record is the reason of the last start and that start owns it.
#   CuMode   NOTE: from tester.31 the release default is 24 (owner, 2026-10-14), and Get-DriverClosure returns
#       nothing where the default equals the closed value, so this entry decides nothing while that default holds:
#       a fallback 24 is 'same' and no repair is offered for it. The entry stays as it is, so the rule below works
#       again by itself if a later release goes back to 40. What a release default of 24 needs instead is
#       Get-UserChoice, further down: the record of a number a person asked for.
#       written 24 with CuModeClosedReason, the reason of the fallback: 3 (a start with all 40 graphics cores
#       was never confirmed), or 2, 6, 7, 8 and 9 for a 40 that did not hold in the hardware (driver/kmd/cumode.c
#       PersistFallback, from bc250_cu_decide force_stock and from bc250_cu_hardware_fallback). From KMD
#       0.7.216.62 that record outlives its boot, because no later start overwrites it and only a start that reads
#       a CuMode other than 24 deletes it (bc250_cu_decide clear_closed). The release default is 40, so without
#       the record one unconfirmed start would hold a tester at 24 compute units through every later upgrade
#       (BD-077, BD-069 for the graphics cores). A reopen writes CuMode 40 and deletes the record; the fallback
#       itself already deleted CuModePending and CuModeConfirmed, so the reopened 40 is a fresh request that its
#       own start confirms.
#       CuMode has no legacy_record, and a driver that writes no CuModeClosedReason therefore keeps its 24 as the
#       tester's own value. CuModeLastReason is not a sound legacy record: every start rewrites it with its own
#       reason, and the reasons of a fallback are not the driver's alone - a tester's own 24 start whose stock
#       restore does not read back writes 9 (RESTORE_FAILED) there as well. Reading such a value as a closure
#       would take the tester's 24 away and start 40 against their choice, which is the worse error of the two:
#       a tester whose 24 is a closure of an older driver gets it back with install.cmd -Repair or the Graphics
#       page of the control application. DpmLastReason is sound because no start but a fallback writes 3, 4 or 8.
# Which codes count depends on who writes the record. Only PersistFallback writes a durable record, so any reason in
# it is the driver's act (any_reason), and a code the table does not name is reported by its number: a later caller
# of PersistFallback with a new reason must not become a 0 that reads as the tester's own setting. A legacy record is
# the reason of the last start, whatever that start decided, so only the reasons of a fallback count there. Where a
# durable record holds nothing (0, or text), the legacy record is read instead.
# The record alone is not a closure: the value has to be the one the driver writes. A value the tester set by hand,
# with no record of a closure, stays 'kept' as before. The KMD's session marker stays the KMD's: the installer never
# reads or writes it, and a marker that outlived its boot closes the switches again at the next start, which the
# install's own verify then reports.
$script:DriverClosures = @{
    EnableGpuPresentBlit = @{ closed = 0; record = 'InteropClosedReason'; any_reason = $true; clear = @('InteropClosedReason')
        names = @{ 2 = 'invalid-setting'; 4 = 'unclean'; 5 = 'registry' }
        text = @{ 4 = 'the driver closed the GPU desktop path after a boot that ended with the path in use' }
        fallback = 'the driver closed the GPU desktop path itself' }
    EnableCddDwmInterop = @{ closed = 0; record = 'InteropClosedReason'; any_reason = $true; clear = @('InteropClosedReason')
        names = @{ 2 = 'invalid-setting'; 4 = 'unclean'; 5 = 'registry' }
        text = @{ 4 = 'the driver closed the GPU desktop path after a boot that ended with the path in use' }
        fallback = 'the driver closed the GPU desktop path itself' }
    DpmMode = @{ closed = 0; record = 'DpmClosedReason'; legacy_record = 'DpmLastReason'; any_reason = $true
        clear = @('DpmClosedReason')
        names = @{ 3 = 'unconfirmed'; 4 = 'unclean'; 8 = 'smu-error' }
        text = @{ 3 = 'the driver went back to the base clock after a start with the load-driven clock that was never confirmed'
                  4 = 'the driver went back to the base clock after a start that ended above it'
                  8 = 'the driver went back to the base clock after repeated clock changes that the firmware refused' }
        fallback = 'the driver went back to the base clock itself' }
    CuMode = @{ closed = 24; record = 'CuModeClosedReason'; any_reason = $true
        clear = @('CuModeClosedReason')
        names = @{ 2 = 'invalid-mask'; 3 = 'unconfirmed'; 6 = 'power-gating'; 7 = 'stock-unexpected'; 8 = 'readback'; 9 = 'restore-failed' }
        text = @{ 3 = 'the driver went back to 24 graphics cores after a start with all 40 that nobody confirmed'
                  6 = 'the driver went back to 24 graphics cores because the firmware kept power gating on, where 40 cannot run'
                  7 = 'the driver went back to 24 graphics cores because the firmware left them in a state it does not know'
                  8 = 'the driver went back to 24 graphics cores because the core registers did not read back'
                  9 = 'the driver went back to 24 graphics cores and could not put the values of the firmware back either' }
        fallback = 'the driver went back to 24 graphics cores itself' }
}
# Pure: the driver closure behind one value, or $null. $Current is the whole key (Read-RegistryValues), because the
# record sits next to the value. $Default is this release's default, so that a default equal to the closed value
# cannot be read as a closure.
function Get-DriverClosure([string]$Name, $Current, $Default) {
    if ($Current -isnot [Collections.IDictionary]) { return $null }
    $c = $script:DriverClosures[$Name]
    if ($null -eq $c) { return $null }
    if (-not $Current.ContainsKey($Name)) { return $null }
    if (-not (Test-RegistryValueSame $Current[$Name] $c.closed)) { return $null }
    if (Test-RegistryValueSame $Default $c.closed) { return $null }
    # The records the driver may leave next to the value, best first: the durable record, then the legacy record of
    # a driver that writes none, if the table names one (the DPM guard before KMD 0.7.208.1 left only DpmLastReason,
    # above). The first record that names a closure decides, so a durable record with nothing in it does not hide
    # the legacy one.
    $records = @($c.record)
    if ($c.ContainsKey('legacy_record')) { $records = @($c.record, $c.legacy_record) }
    foreach ($record in $records) {
        if (-not $Current.ContainsKey($record)) { continue }
        $code = $null
        try { $code = [int]$Current[$record] } catch { continue }
        # Any reason in a durable record is the driver's act, because only the driver writes one. A legacy record
        # needs one of the reasons of a fallback (above).
        if (($record -eq $c.record) -and $c.any_reason) { if ($code -eq 0) { continue } }
        elseif (-not $c.names.ContainsKey($code)) { continue }
        $reason = "reason $code"
        if ($c.names.ContainsKey($code)) { $reason = $c.names[$code] }
        $text = $c.fallback
        if ($c.text.ContainsKey($code)) { $text = $c.text[$code] }
        # A repair deletes the durable record it read. A legacy record belongs to the last start, which writes it
        # again at every start, so nothing deletes it.
        $clear = @()
        if ($record -eq $c.record) { $clear = @($c.clear) }
        return [pscustomobject]@{ record = $record; code = $code; reason = $reason; text = $text; clear = $clear }
    }
    return $null
}
# A value a person asked for, the mirror of a driver closure (BD-077 with the sign reversed). Release\AppliedDefaults
# records what the previous installer wrote, not who decided the value, so a 40 the tester chose in the control
# application and the 40 tester.30 applied as its own release default are the same registry state. With the release
# default at 24 (owner, 2026-10-14) the 'update' rule would then take a chosen 40, or a chosen map of cores, away at
# every upgrade, and nothing else tells the two apart: both end in a confirmed start, so CuModeConfirmed is 40 in
# both, and tester.30's application never wrote a mask.
#   CuMode   CuModeChosen holds the encoded request a person asked for (mode | disable << 8, so 24, 40, or
#       40 | mask << 8). The control application's CU setter writes it on every run (src/CuMode.cs, step S1c), and
#       so does install.cmd -CuMode. "Reset to defaults" removes it, which hands the number of cores back to the
#       installed release. The driver neither reads nor writes it.
# The record alone decides nothing: it has to match the two values next to it exactly, as they are before the
# install. A record that does not (a driver fallback moved CuMode, or somebody edited the key) is stale and is
# ignored, and a driver closure keeps its precedence over the record, because the driver's act is the later one.
# Nothing has to clear a stale record: the encoding is one value per (mode, mask) pair, so a record matches again
# only when the key holds exactly what that person asked for.
$script:UserChoices = @{
    CuMode = @{ record = 'CuModeChosen'; mask = 'CuDisableWgp' }
}
# Pure: the values this install writes on the strength of its command line, which is more than the switches name.
# install.cmd -CuMode N is the tester's word about the number of graphics cores, so the install records it as a
# choice of a person (CuModeChosen), the same record the control application writes. The mask stays whatever the key
# holds, because the installer never writes one, so the record has to describe that same pair or Get-UserChoice
# would read it as stale at the next upgrade. $Before is the key as it is before the install.
function Get-ExplicitParameters([hashtable]$Command, $Before) {
    $e = @{}
    foreach ($k in @($Command.Keys)) { $e[$k] = $Command[$k] }
    if (-not $e.ContainsKey('CuMode')) { return $e }
    $mode = 0
    try { $mode = [int64]$e['CuMode'] } catch { return $e }
    if (($mode -lt 0) -or ($mode -gt 255)) { return $e }
    $mask = 0
    if (($Before -is [Collections.IDictionary]) -and $Before.ContainsKey('CuDisableWgp')) {
        try { $mask = [int64]$Before['CuDisableWgp'] } catch { return $e }
    }
    if (($mask -lt 0) -or ($mask -gt 0xFFFFF)) { return $e }
    $e['CuModeChosen'] = [int](($mode -band 0xFF) -bor ($mask -shl 8))
    return $e
}
# Pure: the record of a person's choice behind one value, or $null. $Current is the whole key, because the record
# and the mask sit next to the value.
function Get-UserChoice([string]$Name, $Current) {
    if ($Current -isnot [Collections.IDictionary]) { return $null }
    $c = $script:UserChoices[$Name]
    if ($null -eq $c) { return $null }
    if (-not $Current.ContainsKey($Name) -or -not $Current.ContainsKey($c.record)) { return $null }
    $mode = $null; $record = $null; $mask = 0
    try { $mode = [int64]$Current[$Name]; $record = [int64]$Current[$c.record] } catch { return $null }
    if ($c.ContainsKey('mask') -and $Current.ContainsKey($c.mask)) { try { $mask = [int64]$Current[$c.mask] } catch { return $null } }
    if (($mode -lt 0) -or ($mode -gt 255) -or ($mask -lt 0)) { return $null }
    if ($record -ne (($mode -band 0xFF) -bor ($mask -shl 8))) { return $null }
    return [pscustomobject]@{ record = $c.record; value = $mode; mask = $mask
        text = "the value $mode is the one a person asked for in the control application or on the install command line" }
}
# Pure: the defaults, what the previous installer wrote, what is in the key now (name -> value; absent = no entry),
# the command-line values and the installer-owned values in; one entry per value out (name, value, decision, write).
function Get-RegistryDefaultPlan {
    param($Defaults, $Previous, [hashtable]$Current = @{}, [hashtable]$Explicit = @{}, $Owned = $null, [hashtable]$After = $null, [string[]]$Restore = @(), [switch]$Reopen)
    $prev = @{}
    foreach ($p in ConvertTo-PairList $Previous) { $prev[$p.Name] = $p.Value }
    $plan = New-Object System.Collections.ArrayList
    $seen = @{}
    foreach ($d in ConvertTo-PairList $Defaults) {
        $seen[$d.Name] = $true
        $has = $Current.ContainsKey($d.Name)
        $cur = $null; if ($has) { $cur = $Current[$d.Name] }
        # A driver closure is read before the 'update' and 'kept' rules: it is neither a value of the previous
        # installer nor a choice of the tester (BD-069).
        $closure = Get-DriverClosure $d.Name $Current $d.Value
        if ($Explicit.ContainsKey($d.Name)) { $decision = 'command'; $value = $Explicit[$d.Name] }
        elseif (-not $has) { $decision = 'set'; $value = $d.Value }
        elseif (Test-RegistryValueSame $cur $d.Value) { $decision = 'same'; $value = $d.Value }
        elseif ($null -ne $closure) {
            if ($Reopen) { $decision = 'reopened'; $value = $d.Value } else { $decision = 'driver-closed'; $value = $cur }
        }
        # Before 'update' and 'kept': a value a person asked for and the key still holds is theirs, whatever the
        # previous installer happened to write (Get-UserChoice above).
        elseif ($null -ne (Get-UserChoice $d.Name $Current)) { $decision = 'chosen'; $value = $cur }
        # Move the old release ceiling, including a manual 1500 choice. Other ceilings stay.
        elseif ($d.Name -eq 'DpmMaxMHz') {
            if (Test-RegistryValueSame $cur 1500) { $decision = 'update'; $value = $d.Value }
            else { $decision = 'kept'; $value = $cur }
        }
        elseif ($prev.ContainsKey($d.Name) -and (Test-RegistryValueSame $cur $prev[$d.Name])) { $decision = 'update'; $value = $d.Value }
        else { $decision = 'kept'; $value = $cur }
        $e = [pscustomobject]@{ name = $d.Name; value = $value; default = $d.Value; current = $cur; present = $has; decision = $decision; write = ($decision -in @('command', 'set', 'update', 'reopened')) }
        if ($decision -in @('reopened', 'driver-closed')) {
            $e | Add-Member -NotePropertyName closure -NotePropertyValue $closure.text
            $e | Add-Member -NotePropertyName closure_record -NotePropertyValue $closure.record
            $e | Add-Member -NotePropertyName closure_code -NotePropertyValue $closure.code
            $e | Add-Member -NotePropertyName closure_reason -NotePropertyValue $closure.reason
            # The record of the closure goes with it, so that the next start does not find a stale one.
            if ($decision -eq 'reopened' -and @($closure.clear).Count) { $e | Add-Member -NotePropertyName clear -NotePropertyValue @($closure.clear) }
        }
        # A value the tester named on the command line is their word about that very value, whatever number they
        # chose, so the driver's record of its own fallback is stale and goes with the write. Without this,
        # install.cmd -CuMode 24 over a closure left the record behind, and every install after it called the
        # tester's own 24 a step back of the driver and offered a repair that takes it away. CuMode is the only
        # closure with a switch that can write the closed value, so this case is new with BD-077.
        elseif ($decision -eq 'command' -and $null -ne $closure -and @($closure.clear).Count) {
            $e | Add-Member -NotePropertyName clear -NotePropertyValue @($closure.clear)
        }
        [void]$plan.Add($e)
    }
    foreach ($e in $Explicit.GetEnumerator()) {
        if ($seen.ContainsKey($e.Key)) { continue }
        # A name from the command line is judged once: without this, a name that is also in $Restore below got a
        # second entry, and the value from before the install was written over the value the tester asked for.
        $seen[$e.Key] = $true
        $has = $Current.ContainsKey($e.Key)
        [void]$plan.Add([pscustomobject]@{ name = [string]$e.Key; value = $e.Value; default = $null; current = $(if ($has) { $Current[$e.Key] } else { $null }); present = $has; decision = 'command'; write = $true })
    }
    foreach ($o in ConvertTo-PairList $Owned) {
        $seen[$o.Name] = $true
        $has = $Current.ContainsKey($o.Name)
        [void]$plan.Add([pscustomobject]@{ name = $o.Name; value = $o.Value; default = $o.Value; current = $(if ($has) { $Current[$o.Name] } else { $null }); present = $has; decision = 'installer'; write = $true })
    }
    # Values outside the table that a reset between the judgement and the write may change (the INF's other AddReg
    # values): the value from before the reset is written back; a name that was absent keeps what the reset wrote.
    # A record that a reopened closure clears is left out: written back, it would undo the clear (BD-069).
    $cleared = @{}
    foreach ($e in $plan) { if ($e.PSObject.Properties['clear']) { foreach ($n in @($e.clear)) { $cleared[$n] = $true } } }
    foreach ($n in @($Restore)) {
        if ($seen.ContainsKey($n) -or $cleared.ContainsKey($n) -or -not $Current.ContainsKey($n)) { continue }
        $seen[$n] = $true
        [void]$plan.Add([pscustomobject]@{ name = $n; value = $Current[$n]; default = $null; current = $Current[$n]; present = $true; decision = 'restored'; write = $false })
    }
    # $Current was read before a reset (the driver package's AddReg writes 0 into every gate): the decisions above come
    # from it, the writes from what the key holds now ($After). A value the reset changed is written back.
    if ($null -ne $After) {
        foreach ($e in $plan) {
            $now = $null; if ($After.ContainsKey($e.name)) { $now = $After[$e.name] }
            $differs = (-not $After.ContainsKey($e.name)) -or -not (Test-RegistryValueSame $now $e.value)
            $e | Add-Member -NotePropertyName rewrite -NotePropertyValue ((-not $e.write) -and $differs)
            # The writes come from $After alone: a value the key already holds is not written again. Without this, a
            # default of this release that the previous installer never wrote (so it is absent from $Current) stayed a
            # 'set' with a write at every judgement from the same snapshot. Format-RegistryPlan reads the decision, not
            # this flag, so the plan the tester reads does not change.
            $e.write = $differs
        }
    }
    return , $plan.ToArray()
}
# Can Write-RegistryPlan write this value back with the type it had? It writes REG_MULTI_SZ for a list of strings,
# REG_SZ for text and REG_DWORD for a number, so a REG_BINARY or REG_QWORD value cannot go back through the plan and
# is left alone (install.ps1 names it in the log). A number outside the REG_DWORD range is not restorable either.
function Test-RestorableRegistryValue($Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [string]) { return $true }
    if ($Value -is [array]) {
        if (@($Value).Count -eq 0) { return $false }       # an empty REG_BINARY looks the same here
        foreach ($x in $Value) { if ($x -isnot [string]) { return $false } }
        return $true
    }
    if ($Value -is [int] -or $Value -is [long] -or $Value -is [uint32] -or $Value -is [uint64] -or $Value -is [int16] -or $Value -is [byte]) {
        return (([int64]$Value -ge [int]::MinValue) -and ([int64]$Value -le [int]::MaxValue))
    }
    return $false
}
# The value names the driver package writes under HKR\Parameters (the INF's AddReg lines), i.e. what pnputil resets.
function Get-InfParameterNames([string]$InfPath) {
    $names = @()
    foreach ($l in [IO.File]::ReadAllLines($InfPath)) { if ($l -match '^\s*HKR\s*,\s*Parameters\s*,\s*([A-Za-z0-9_]+)\s*,') { $names += $Matches[1] } }
    return , $names
}

# ---------------------------------------------------------------------------------------------------------------
# No device restart and no DWM restart under the running desktop (BD-060). WinUI pointer-input loss after the desktop
# compositor (DWM) is terminated and restarted reproduces on this Windows build (22631) also with Microsoft Basic
# Display; restarting Windows recovers (dwm-session.ps1). A GPU restarted in place under the desktop takes DWM's devices
# away (a device-loss event, observed apart from any DWM process restart). The BD-059 session marker
# (Parameters\InteropSession) belongs to the KMD, and the installer never changes it: if a start closes the GPU desktop
# path, "Reopen the GPU desktop path" in the control application opens it again (INSTALL.md), and so does a repair
# install (the driver closures above).

# Legacy compatibility-record readers. Display INFs must not contain Reboot (BD-152); current packages use
# deferred-driver.ps1 / INSTALL_AS_SET. Keep this reader for the records of older rollback packages only.
function Get-InfInstallSections([string[]]$Lines) {
    # [Manufacturer] names the models sections (name, plus name.decoration for each decoration); each model line
    # names its install section.
    $clean = @($Lines | ForEach-Object { ($_ -replace ';.*$', '').Trim() })
    $models = @(); $sections = @(); $current = ''
    foreach ($t in $clean) {
        if ($t -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
        if ($current -eq 'Manufacturer' -and $t -match '=\s*(.+)$') {
            $parts = @($Matches[1] -split ',' | ForEach-Object { $_.Trim() })
            $models += $parts[0]
            if ($parts.Count -gt 1) { foreach ($d in $parts[1..($parts.Count - 1)]) { if ($d) { $models += "$($parts[0]).$d" } } }
        }
    }
    foreach ($t in $clean) {
        if ($t -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
        if ($models -contains $current -and $t -match '=\s*([A-Za-z0-9_.]+)\s*,') { $sections += $Matches[1] }
    }
    return , @($sections | Select-Object -Unique)
}
function Test-InfDefersDeviceRestart([string[]]$Lines) {
    $sections = Get-InfInstallSections $Lines
    if (-not $sections.Count) { return $false }
    foreach ($s in $sections) {
        $in = $false; $found = $false
        foreach ($l in $Lines) {
            $t = ($l -replace ';.*$', '').Trim()
            if ($t -match '^\[(.+)\]$') { $in = ($Matches[1] -eq $s); continue }
            if ($in -and $t -eq 'Reboot') { $found = $true }
        }
        if (-not $found) { return $false }
    }
    return $true
}
# What the exit code of pnputil /add-driver /install establishes (PnPUtil Return Values), and no more: it does not say
# whether a device instance stopped or started. The binding check after it decides what the GPU runs.
function Get-DriverPackageOutcome([int]$Code) {
    switch ($Code) {
        3010 { return [pscustomobject]@{ ok = $true; deferred = $true; text = 'pnputil exit 3010: completed, a system restart is required' } }
        0 { return [pscustomobject]@{ ok = $true; deferred = $false; text = 'pnputil exit 0: completed' } }
        259 { return [pscustomobject]@{ ok = $true; deferred = $false; text = 'pnputil exit 259: no device change reported' } }
        default { return [pscustomobject]@{ ok = $false; deferred = $false; text = "pnputil exit $Code" } }
    }
}

function Format-RegistryPlan($Plan) {
    $parts = @()
    foreach ($e in @($Plan)) {
        switch ($e.decision) {
            'same'      { $parts += "$($e.name)=$(Format-RegistryValue $e.value) (unchanged)" }
            'set'       { $parts += "$($e.name)=$(Format-RegistryValue $e.value) (new)" }
            'update'    { $parts += "$($e.name) $(Format-RegistryValue $e.current) -> $(Format-RegistryValue $e.value) (new default; not changed by the tester)" }
            'kept'      { $parts += "$($e.name)=$(Format-RegistryValue $e.value) KEPT (changed by the tester; default $(Format-RegistryValue $e.default))" }
            'chosen'    { $parts += "$($e.name)=$(Format-RegistryValue $e.value) CHOSEN (asked for in the control application or on the install command line; default $(Format-RegistryValue $e.default))" }
            'reopened'  { $parts += "$($e.name) $(Format-RegistryValue $e.current) -> $(Format-RegistryValue $e.value) (reopened by this repair: $($e.closure), $($e.closure_record) $($e.closure_code) $($e.closure_reason))" }
            'driver-closed' { $parts += "$($e.name)=$(Format-RegistryValue $e.value) KEPT ($($e.closure), $($e.closure_record) $($e.closure_code) $($e.closure_reason); not a setting of the tester; default $(Format-RegistryValue $e.default); remedy: install.cmd -Repair)" }
            'command'   { $parts += "$($e.name)=$(Format-RegistryValue $e.value) (command line)" }
            'installer' { $parts += "$($e.name)=$(Format-RegistryValue $e.value)" }
            'restored'  { $parts += "$($e.name)=$(Format-RegistryValue $e.value) (as before the install)" }
        }
        if ($e.rewrite) { $parts[$parts.Count - 1] += ' [written again over the driver package reset]' }
    }
    return ($parts -join '; ')
}
# The named values of a key (name -> value), empty when the key does not exist. REG_EXPAND_SZ stays unexpanded.
function Read-RegistryValues([string]$Key) {
    $h = @{}
    $k = Get-Item -LiteralPath $Key -ErrorAction SilentlyContinue
    if (-not $k) { return $h }
    foreach ($n in $k.GetValueNames()) { if ($n) { $h[$n] = $k.GetValue($n, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } }
    return $h
}
# Writes the plan's entries marked write. The type follows the value: a list is REG_MULTI_SZ, text REG_SZ, a number
# REG_DWORD.
function Write-RegistryPlan([string]$Key, $Plan) {
    Initialize-RegistryKey $Key
    foreach ($e in @($Plan)) {
        if ($e.write) {
            $v = $e.value
            if ($v -is [array]) { New-ItemProperty -LiteralPath $Key -Name $e.name -Value ([string[]]@($v | ForEach-Object { [string]$_ })) -PropertyType MultiString -Force | Out-Null }
            elseif ($v -is [string]) { New-ItemProperty -LiteralPath $Key -Name $e.name -Value $v -PropertyType String -Force | Out-Null }
            else { New-ItemProperty -LiteralPath $Key -Name $e.name -Value ([int]$v) -PropertyType DWord -Force | Out-Null }
        }
        # A reopened driver closure (BD-069) also clears the record the driver left next to the value, after the
        # value itself. A write that throws then leaves the record where it is, and the closure is still readable
        # at the next install. With the clear first, a failed write would lose the record and the install after it
        # would report the closed value as the tester's own setting.
        if ($e.PSObject.Properties['clear']) {
            foreach ($n in @($e.clear)) { Remove-ItemProperty -LiteralPath $Key -Name $n -Force -ErrorAction SilentlyContinue }
        }
    }
}

# ---------------------------------------------------------------------------------------------------------------
# SHA256 through .NET: works whatever modules the host process can load.
function Get-Sha256([string]$Path) {
    $s = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) -replace '-', '') } finally { $s.Dispose() }
}
# Package integrity: manifest.json lists every file of the package with its SHA256 (build-release.ps1 writes it).
# The setup window is the one file outside that list, because it is the one file the unpacked release puts in the top
# folder (manifest "setup", Get-PackageSetupExe); it is hashed here like every other file. A manifest from a release
# before that layout has no "setup" block and is checked as before.
function Test-PackageManifest {
    param([Parameter(Mandatory)][string]$PackageRoot)
    $path = Join-Path $PackageRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $path)) { return @{ ok = $false; detail = 'manifest.json missing' } }
    $m = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $bad = @()
    $count = 0
    foreach ($f in @($m.files)) {
        $count++
        $p = Join-Path $PackageRoot ($f.path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "missing $($f.path)"; continue }
        if ((Get-Sha256 $p) -ne $f.sha256) { $bad += "changed $($f.path)" }
    }
    if ($m.PSObject.Properties['setup'] -and $m.setup) {
        $count++
        $exe = Get-PackageSetupExe $PackageRoot
        if (-not $exe) { $bad += "missing $([string]$m.setup.file)" }
        elseif ((Get-Sha256 $exe) -ne ([string]$m.setup.sha256)) { $bad += "changed $([string]$m.setup.file)" }
    }
    if ($bad.Count) { return @{ ok = $false; detail = ($bad -join '; '); manifest = $m } }
    return @{ ok = $true; detail = "$count files match manifest.json (version $($m.version))"; manifest = $m }
}

# ---------------------------------------------------------------------------------------------------------------
# GPU firmware. The package does not contain it: manifest.json (firmware) pins the linux-firmware commit, each file's
# path there and its SHA256. The installer downloads the files (or takes them from -FirmwareDir) into a staging folder
# and checks every SHA256 before anything is copied to the firmware folder.
function Get-FirmwareUrls($Firmware, $File) {
    foreach ($t in @($Firmware.url_templates)) { ([string]$t).Replace('{commit}', [string]$Firmware.commit).Replace('{path}', [string]$File.path) }
}
# git.kernel.org answers a browser-like User-Agent (Windows PowerShell's default starts with Mozilla) with an anti-bot
# challenge page instead of the file (measured 2026-10-03); a plain program name gets the file.
$script:DownloadUserAgent = 'amdgpu-wddm-installer'
function Enable-Tls12 {
    # Windows PowerShell 5.1 may still offer only TLS 1.0/1.1 by default; both download hosts need TLS 1.2.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}
# Preflight: does each download host answer? One HEAD request per host for the smallest file, nothing is downloaded.
# Up to $Rounds rounds 10 s apart while no host answers: after the restart the installer runs at logon, sometimes before
# the network is up.
function Test-FirmwareHosts {
    param([Parameter(Mandatory)]$Firmware, [int]$TimeoutSec = 15, [int]$Rounds = 3)
    Enable-Tls12
    $small = @($Firmware.files) | Sort-Object { [long]$_.size } | Select-Object -First 1
    for ($round = 1; $round -le $Rounds; $round++) {
        $ok = @(); $bad = @()
        foreach ($u in Get-FirmwareUrls $Firmware $small) {
            $name = ([Uri]$u).Host
            if ($script:NetworkBlocked) { $bad += "$name network unavailable (test)"; continue }
            try {
                $r = Invoke-WebRequest -Uri $u -Method Head -UseBasicParsing -TimeoutSec $TimeoutSec -UserAgent $script:DownloadUserAgent
                if ([int]$r.StatusCode -eq 200) { $ok += $name } else { $bad += "$name HTTP $([int]$r.StatusCode)" }
            } catch { $bad += "$name $($_.Exception.Message)" }
        }
        if ($ok.Count -or $round -eq $Rounds -or $script:NetworkBlocked) { break }
        Start-Sleep -Seconds 10
    }
    return @{ ok = $ok; bad = $bad }
}
# Puts every firmware file into $Staging (emptied first) and checks its SHA256 against the manifest. With $FromDir:
# the file of the same name from that folder. Otherwise each URL in manifest order, up to $Tries tries per URL; a
# download with another SHA256 moves on to the next URL. Throws when a file cannot be had with the pinned SHA256.
# Writes nothing but $Staging; returns the staged paths.
function Get-FirmwareStaged {
    param([Parameter(Mandatory)]$Firmware, [Parameter(Mandatory)][string]$Staging, [string]$FromDir, [int]$Tries = 3, [int]$TimeoutSec = 60)
    if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
    [void][IO.Directory]::CreateDirectory($Staging)
    Enable-Tls12
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # the 5.1 progress display slows Invoke-WebRequest down many times over
    try {
        $staged = @()
        foreach ($f in @($Firmware.files)) {
            $dst = Join-Path $Staging $f.name
            $want = ([string]$f.sha256).ToUpperInvariant()
            $got = $null
            $why = @()
            if ($FromDir) {
                $src = Join-Path $FromDir $f.name
                if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "firmware: $($f.name) is not in $FromDir" }
                [IO.File]::Copy($src, $dst, $true)
                $h = Get-Sha256 $dst
                if ($h -ne $want) { throw "firmware: $src has SHA256 $h, this release pins $want" }
                $got = $src
            } else {
                foreach ($u in Get-FirmwareUrls $Firmware $f) {
                    $name = ([Uri]$u).Host
                    for ($i = 1; $i -le $Tries; $i++) {
                        if ($script:NetworkBlocked) { $why += "$name`: network unavailable (test)"; break }
                        try { Invoke-WebRequest -Uri $u -OutFile $dst -UseBasicParsing -TimeoutSec $TimeoutSec -UserAgent $script:DownloadUserAgent }
                        catch { $why += "$name try $i`: $($_.Exception.Message)"; continue }
                        $h = Get-Sha256 $dst
                        if ($h -eq $want) { $got = $u } else { $why += "$name`: SHA256 $h"; Remove-Item -LiteralPath $dst -Force }
                        break
                    }
                    if ($got) { break }
                }
                if (-not $got) { throw "firmware: $($f.name) could not be downloaded with the pinned SHA256 $want ($($why -join '; '))" }
            }
            Write-Info ('{0,-26} SHA256 {1} ok  {2}' -f $f.name, $want, $got)
            $staged += $dst
        }
        return $staged
    } finally { $ProgressPreference = $oldProgress }
}

# ---------------------------------------------------------------------------------------------------------------
# System facts. Each returns data; the preflight decides.
# Whether the driver of a device verify found actually started, and why not (GitHub #38).
#
# A device that carries a problem code (Code 43 and the rest) has no running driver. Verify then skips the work that
# cannot succeed on it: the wait for the start-confirm task, which confirms a start that did not happen and would
# only spend its whole 120 s bound, and every probe that needs a started adapter (D3D12 through DXGI, Vulkan through
# the loader, the driver's own interop state). On the tester's PC of #38 a failed start took 2 min 43 s to report,
# 110 s of it in that wait. The verdict does not change: 'driver bound' and 'driver start' carry it, as they always
# did, and each skipped probe is still recorded under its own name with the reason.
#
# ProblemCode: ConfigManagerErrorCode of the device. Status: the Windows device status, for the reason text only.
function Get-VerifyStartState {
    param([int]$ProblemCode = 0, [string]$Status = '')
    $started = ($ProblemCode -eq 0)
    $reason = $null
    if (-not $started) {
        $reason = "the device reports problem code $ProblemCode"
        if ($Status -and $Status -ne 'OK') { $reason = "$reason and Windows reports it as $Status" }
    }
    return [pscustomobject]@{ started = $started; reason = $reason }
}

# BD-161: the 'driver version' check of verify. Win32_PnPSignedDriver reads DriverVersion from the adapter's software
# key, and with the control app's "Report an AMD driver version" on (HKLM\SOFTWARE\amdgpu-wddm\Graphics
# ReportAmdDriverVersion 1) the KMD rewrites that value at each start to the AMD-scheme number (driver/kmd/
# driver_version.c) and keeps the installed one in Bc250DriverVersion and the number it wrote in
# Bc250ReportedDriverVersion. A field verify (Pawel, tester.29) read 40.7.216.100 against 0.7.216.100 and failed a
# working driver. The rewritten reading names this package only when the KMD's own two records say so: the backup is
# the package number and the reading is the number the KMD wrote. No arithmetic on the reading itself.
#
# Reading: DriverVersion as read. Want: the package's kmd_version. Backup, Reported: the two KMD values of the
# software key, $null when absent.
function Test-InstalledDriverVersion {
    param([AllowNull()][string]$Reading, [AllowNull()][string]$Want, [AllowNull()][string]$Backup, [AllowNull()][string]$Reported)
    if ([string]::IsNullOrEmpty($Reading) -or [string]::IsNullOrEmpty($Want)) { return [pscustomobject]@{ ok = $false; note = '' } }
    if ($Reading -eq $Want) { return [pscustomobject]@{ ok = $true; note = '' } }
    if (($Backup -eq $Want) -and ($Reported -eq $Reading)) {
        return [pscustomobject]@{ ok = $true; note = "; the driver reports $Reading in AMD numbering (Report an AMD driver version is on), installed $Backup" }
    }
    return [pscustomobject]@{ ok = $false; note = '' }
}

function Get-Bc250Device {
    # Present devices only, matched on the hardware ID the INF matches.
    $all = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like ($script:HardwareIdPrefix + '*') })
    return $all
}
# ---------------------------------------------------------------------------------------------------------------
# The GPU's HD Audio function on message-signalled interrupts (BD-092).
#
# The second function of the same silicon, PCI\VEN_1002&DEV_13FF, carries the DisplayPort audio endpoint that the
# kernel driver configures (driver/kmd/dpaudio.c). It keeps the inbox HDAudBus driver, and the inbox hdaudbus.inf
# writes MSISupported 0 for it, so Windows gives it a line interrupt. On this board the stream interrupts of that
# function never arrive on a line interrupt: measured on unit A, every WASAPI mode played at 0.33 times its rate and
# a stream took 7 to 9 seconds to start. With MSISupported 1 and a restart of that one device, every mode ran at
# 1.0000 with its events 10 ms apart. Linux snd_hda_intel uses MSI on the same function.
#
# So the installer writes that one value and restarts that one device. Nothing else of the audio function is touched:
# no driver, no other value, no other device, and no restart of the computer. The previous value is written down in
# the installer state, and the uninstaller puts it back (or removes the value when there was none).
#
# A failure here is a warning, never a failed install: DisplayPort audio is a feature of the driver, and the rest of
# the driver does not depend on it.
function Get-GpuAudioDevices {
    # Present functions only, matched on the hardware ID of the audio function.
    return @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like ($script:AudioIdPrefix + '*') })
}
function Get-GpuAudioMsiKey([string]$InstanceId) {
    return "$($script:AudioEnumRoot)\$InstanceId\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
}
# What the computer holds now: one row per present audio function (instance, key, whether the key is there, and the
# value or $null). Reads only; a key or value that cannot be read reads as absent, which the plan treats as 'set'.
function Read-GpuAudioMsi {
    $rows = @()
    foreach ($d in Get-GpuAudioDevices) {
        $key = Get-GpuAudioMsiKey $d.DeviceID
        $present = Test-Path -LiteralPath $key
        $value = $null
        if ($present) {
            $p = Get-ItemProperty -LiteralPath $key -Name $script:AudioMsiValue -ErrorAction SilentlyContinue
            if ($p -and ($null -ne $p.$($script:AudioMsiValue))) { $value = [int]$p.$($script:AudioMsiValue) }
        }
        $rows += [pscustomobject]@{ instance = $d.DeviceID; key = $key; key_present = $present; value = $value }
    }
    # A plain array, not ', $rows': every caller wraps the call in @(), and @() does not unroll a nested array.
    return $rows
}
# Pure: the readings and the value this release wants in, one decision per function out.
#   same   the value is already the one we want: nothing is written and no device is restarted
#   set    the value (or its key) is absent: write it
#   update another value is there: write ours, and remember the old one
function Get-GpuAudioMsiPlan {
    param($Readings, [int]$Wanted = 1)
    $plan = @()
    foreach ($r in @($Readings)) {
        if ($null -eq $r.value) { $decision = 'set' }
        elseif ([int]$r.value -eq $Wanted) { $decision = 'same' }
        else { $decision = 'update' }
        $plan += [pscustomobject]@{ instance = $r.instance; key = $r.key; key_present = [bool]$r.key_present
            previous = $r.value; value = $Wanted; decision = $decision
            write = ($decision -ne 'same'); restart = ($decision -ne 'same') }
    }
    return $plan
}
# Pure: what the uninstaller does with what the install wrote down.
#   restore the value we wrote is still there and there was one before: write the old one back
#   remove  the value we wrote is still there and there was none before: remove it
#   kept    another value is there now: somebody else owns it, we leave it alone
#   absent  the key or the function is gone: nothing to do
function Get-GpuAudioMsiRestorePlan {
    param($Recorded, $Readings, [int]$Wrote = 1)
    $now = @{}
    foreach ($r in @($Readings)) { $now[[string]$r.instance] = $r }
    $plan = @()
    foreach ($e in @($Recorded)) {
        $instance = [string]$e.instance
        $previous = $null
        if ($e.PSObject.Properties['previous'] -and ($null -ne $e.previous)) { $previous = [int]$e.previous }
        $row = $null
        if ($now.ContainsKey($instance)) { $row = $now[$instance] }
        if (($null -eq $row) -or -not $row.key_present) { $decision = 'absent'; $value = $null }
        elseif ($null -eq $row.value) { $decision = 'absent'; $value = $null }
        elseif ([int]$row.value -ne $Wrote) { $decision = 'kept'; $value = [int]$row.value }
        elseif ($null -eq $previous) { $decision = 'remove'; $value = $null }
        else { $decision = 'restore'; $value = $previous }
        $key = $(if ($row) { $row.key } else { Get-GpuAudioMsiKey $instance })
        $plan += [pscustomobject]@{ instance = $instance; key = $key; current = $(if ($row) { $row.value } else { $null })
            value = $value; decision = $decision
            write = ($decision -eq 'restore'); drop = ($decision -eq 'remove'); restart = ($decision -in @('restore', 'remove')) }
    }
    return $plan
}
# One line per function, for the console and the log. The instance id is the device's own, not a secret.
function Format-GpuAudioMsiPlan($Plan) {
    $parts = @()
    foreach ($e in @($Plan)) {
        switch ($e.decision) {
            'same'    { $parts += "$($e.instance): $($script:AudioMsiValue) is already $($e.value)" }
            'set'     { $parts += "$($e.instance): $($script:AudioMsiValue) = $($e.value) (absent before)" }
            'update'  { $parts += "$($e.instance): $($script:AudioMsiValue) $($e.previous) -> $($e.value)" }
            'restore' { $parts += "$($e.instance): $($script:AudioMsiValue) back to $($e.value) (as before the install)" }
            'remove'  { $parts += "$($e.instance): $($script:AudioMsiValue) removed (absent before the install)" }
            'kept'    { $parts += "$($e.instance): $($script:AudioMsiValue) $($e.current) kept (not the value this release wrote)" }
            'absent'  { $parts += "$($e.instance): nothing to put back (the function or its key is gone)" }
        }
    }
    if (-not $parts.Count) { return 'no HD Audio function of the GPU on this computer' }
    return ($parts -join '; ')
}
function Set-GpuAudioMsiValue([string]$Key, [int]$Value) {
    Initialize-RegistryKey $Key
    New-ItemProperty -LiteralPath $Key -Name $script:AudioMsiValue -Value $Value -PropertyType DWord -Force | Out-Null
}
function Remove-GpuAudioMsiValue([string]$Key) {
    Remove-ItemProperty -LiteralPath $Key -Name $script:AudioMsiValue -Force -ErrorAction SilentlyContinue
}
# pnputil /restart-device for one device, then the device back in its normal state, both inside one budget. The
# endpoints of that function disappear for a few seconds and the audio service rebuilds them; no other device and no
# part of the desktop is restarted (BD-060 is about the display device, which this is not).
#
# The budget covers pnputil itself (audit finding K3): it used to cover only the status polling after pnputil had
# returned, so a PnP restart that never came back held the install or the uninstall with no bound at all. A
# timeout is reported as itself - timed_out, with the process id still running - and not as a failed or unknown
# device status, because a stuck kernel PnP transition is a different thing from a device that answered something
# else. The run is not killed; see Invoke-NativeBounded.
function Restart-GpuAudioDevice {
    param([Parameter(Mandatory)][string]$InstanceId, [int]$TimeoutSeconds = $script:AudioRestartWaitSeconds)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $n = Invoke-NativeBounded -File $script:PnpUtilPath -Arguments @('/restart-device', $InstanceId) `
        -TimeoutSeconds $TimeoutSeconds
    $status = $null
    if (-not $n.timed_out) {
        while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            $d = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
            if ($d) { $status = [string]$d.Status; if ($status -eq 'OK') { break } }
            Start-Sleep -Seconds 1
        }
    }
    return [pscustomobject]@{ code = $n.code; text = $n.text; status = $status; timed_out = $n.timed_out
        id = $n.id; restart_seconds = $n.seconds
        seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1)
        ok = ((-not $n.timed_out) -and ($n.code -eq 0) -and ($status -eq 'OK')) }
}
# Did the write reach the computer? Reads the value again, for the one instance.
function Test-GpuAudioMsi {
    param([Parameter(Mandatory)][string]$InstanceId, $Expected)
    $key = Get-GpuAudioMsiKey $InstanceId
    $value = $null
    if (Test-Path -LiteralPath $key) {
        $p = Get-ItemProperty -LiteralPath $key -Name $script:AudioMsiValue -ErrorAction SilentlyContinue
        if ($p -and ($null -ne $p.$($script:AudioMsiValue))) { $value = [int]$p.$($script:AudioMsiValue) }
    }
    if ($null -eq $Expected) { return [pscustomobject]@{ ok = ($null -eq $value); value = $value } }
    return [pscustomobject]@{ ok = ($null -ne $value) -and ([int]$value -eq [int]$Expected); value = $value }
}

function Get-TestSigningActive {
    # The options the running boot was started with; readable without elevation.
    $o = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
    return ([string]$o -match 'TESTSIGNING')
}
function Get-TestSigningConfigured {
    # The explicit setting on the current loader entry; $null when the provider cannot read it.
    # BCD WMI uses a Boolean, unlike the localized bcdedit display value.
    # https://learn.microsoft.com/previous-versions/windows/desktop/bcd/bcdlibraryelementtypes
    # https://learn.microsoft.com/previous-versions/windows/desktop/bcd/bcdbooleanelement
    if (-not (Test-IsAdmin)) { return $null }
    try {
        $provider = Get-WmiObject -Namespace 'root\WMI' -Class BcdStore -List -EnableAllPrivileges -ErrorAction Stop
        $opened = $provider.OpenStore('') # Empty path denotes the system store.
        if ($opened.ReturnValue -isnot [bool] -or -not $opened.ReturnValue -or $null -eq $opened.Store) { return $null }
        # GUID_CURRENT_BOOT_ENTRY from Microsoft's Windows-classic-samples BCD Constants.cs.
        $current = $opened.Store.OpenObject('{fa926493-6f1c-4193-a414-58f0b2456d1e}')
        if ($current.ReturnValue -isnot [bool] -or -not $current.ReturnValue -or $null -eq $current.Object) { return $null }
        $enumerated = $current.Object.EnumerateElements()
        if ($enumerated.ReturnValue -isnot [bool] -or -not $enumerated.ReturnValue) { return $null }
        # BcdLibraryBoolean_AllowPrereleaseSignatures. Successful enumeration distinguishes
        # an absent explicit setting from a failed GetElement call. Inherited options are separate.
        $setting = @($enumerated.Elements | Where-Object { $_.Type -eq [uint32]0x16000049 })
        if ($setting.Count -eq 0) { return $false }
        if ($setting.Count -ne 1 -or $setting[0].Boolean -isnot [bool]) { return $null }
        return $setting[0].Boolean
    } catch { return $null }
}
function Get-SecureBootState {
    # 'on', 'off', 'legacy-bios' or 'unknown'
    return (Read-SecureBootState).state
}
# Secure Boot from the firmware variable that holds it: SecureBoot in the EFI global variable namespace, one byte,
# 1 on and 0 off. A firmware without Secure Boot does not define the variable: the stock BC-250 firmware is one
# (BD-125), where Confirm-SecureBootUEFI throws "Variable is currently undefined" and the SecureBoot\State key is
# absent. Only the Win32 error number of the read decides, never the (translated) message text.
# https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getfirmwareenvironmentvariableexw
# Confirm-SecureBootUEFI and the registry answer only when the variable read itself fails. The result names the
# source that decided. The three readers are parameters for the host test (test-engine-units.ps1).
function Read-SecureBootState {
    param([scriptblock]$ReadVariable = { Read-SecureBootVariable },
          [scriptblock]$ConfirmUefi = { Confirm-SecureBootUEFI -ErrorAction Stop },
          [scriptblock]$ReadRegistry = { (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue).UEFISecureBootEnabled })
    function Result([string]$State, [string]$Detail, [string]$Source) { [pscustomobject]@{ state = $State; detail = $Detail; source = $Source } }
    $variable = 'UEFI variable SecureBoot'
    try {
        $v = & $ReadVariable
        if ($null -ne $v.bytes) {
            $bytes = [byte[]]$v.bytes
            if ($bytes.Length -eq 1 -and $bytes[0] -eq 1) { return (Result 'on' "on (source: $variable = 1)" 'uefi-variable') }
            if ($bytes.Length -eq 1 -and $bytes[0] -eq 0) { return (Result 'off' "off (source: $variable = 0)" 'uefi-variable') }
            $why = "$variable holds an unexpected value: $(([BitConverter]::ToString($bytes)) -replace '-', ' ')"
        } elseif ($v.error -eq 203) {
            # ERROR_ENVVAR_NOT_FOUND: the firmware does not define the variable.
            return (Result 'off' "off (not built into the firmware; source: $variable not present, Win32 error 203)" 'uefi-variable')
        } elseif ($v.error -eq 1) {
            # ERROR_INVALID_FUNCTION: Windows started from a legacy BIOS, which has no UEFI variables.
            return (Result 'legacy-bios' "legacy-bios (source: $variable, Win32 error 1: no UEFI variables)" 'uefi-variable')
        } else {
            $why = "$variable read failed with Win32 error $($v.error)"
            if ($v.privilege_error) { $why += " (SeSystemEnvironmentPrivilege: Win32 error $($v.privilege_error))" }
        }
    } catch { $why = "$variable read did not run: $($_.Exception.GetType().FullName)" }
    try {
        $on = & $ConfirmUefi
        if ($on -is [bool]) { return (Result $(if ($on) { 'on' } else { 'off' }) "$(if ($on) { 'on' } else { 'off' }) (source: Confirm-SecureBootUEFI, because the $why)" 'confirm-securebootuefi') }
        $confirm = "Confirm-SecureBootUEFI returned no Boolean"
    }
    catch [System.PlatformNotSupportedException] { return (Result 'legacy-bios' "legacy-bios (source: Confirm-SecureBootUEFI, because the $why)" 'confirm-securebootuefi') }
    catch { $confirm = 'Confirm-SecureBootUEFI: {0} 0x{1:X8}' -f $_.Exception.GetType().FullName, $_.Exception.HResult }
    $reg = & $ReadRegistry
    if ($reg -eq 1) { return (Result 'on' "on (source: registry SecureBoot\State UEFISecureBootEnabled = 1, because the $why)" 'registry') }
    if ($reg -eq 0) { return (Result 'off' "off (source: registry SecureBoot\State UEFISecureBootEnabled = 0, because the $why)" 'registry') }
    return (Result 'unknown' "the $why; $confirm; no SecureBoot\State UEFISecureBootEnabled value" 'none')
}
# The raw read: bytes, or the Win32 error (and the error of enabling SeSystemEnvironmentPrivilege, which the read needs).
function Read-SecureBootVariable {
    if (-not ('AmdgpuWddmInstaller.FirmwareVariable' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AmdgpuWddmInstaller {
public static class FirmwareVariable {
    [StructLayout(LayoutKind.Sequential, Pack = 4)] struct TokenPrivilege { public uint Count; public long Luid; public uint Attributes; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivilege state, uint length, out TokenPrivilege previous, out uint returned);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern uint GetFirmwareEnvironmentVariableExW(string name, string guid, byte[] buffer, uint size, out uint attributes);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    // The variable's bytes, or null and the Win32 error. SeSystemEnvironmentPrivilege is enabled for the read and put
    // back as it was; privilegeError is 0, or the error of enabling it (1300: the account does not hold it).
    public static byte[] Read(string name, string guid, out int error, out int privilegeError) {
        IntPtr token = IntPtr.Zero;
        var previous = new TokenPrivilege();
        bool adjusted = false;
        privilegeError = 0;
        // TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY; SE_PRIVILEGE_ENABLED.
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) { privilegeError = Marshal.GetLastWin32Error(); token = IntPtr.Zero; }
        try {
            if (token != IntPtr.Zero) {
                var p = new TokenPrivilege { Count = 1, Attributes = 2 };
                uint returned;
                if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out p.Luid)) privilegeError = Marshal.GetLastWin32Error();
                else if (!AdjustTokenPrivileges(token, false, ref p, (uint)Marshal.SizeOf(typeof(TokenPrivilege)), out previous, out returned)) privilegeError = Marshal.GetLastWin32Error();
                else { privilegeError = Marshal.GetLastWin32Error(); adjusted = privilegeError == 0; }
            }
            var buffer = new byte[64];
            uint attributes;
            uint size = GetFirmwareEnvironmentVariableExW(name, guid, buffer, (uint)buffer.Length, out attributes);
            if (size == 0) { error = Marshal.GetLastWin32Error(); return null; }
            error = 0;
            Array.Resize(ref buffer, (int)size);
            return buffer;
        } finally {
            if (adjusted && previous.Count != 0) {
                TokenPrivilege ignored; uint returned;
                AdjustTokenPrivileges(token, false, ref previous, (uint)Marshal.SizeOf(typeof(TokenPrivilege)), out ignored, out returned);
            }
            if (token != IntPtr.Zero) CloseHandle(token);
        }
    }
}
}
'@
    }
    $err = 0
    $privilegeErr = 0
    # EFI_GLOBAL_VARIABLE (UEFI specification, section 3.3 Globally Defined Variables).
    $bytes = [AmdgpuWddmInstaller.FirmwareVariable]::Read('SecureBoot', '{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}', [ref]$err, [ref]$privilegeErr)
    return [pscustomobject]@{ bytes = $bytes; error = $err; privilege_error = $privilegeErr }
}
function Get-BitLockerState {
    # 'off', 'on', 'suspended' or 'unknown'
    # Read live method results, not the cached WMI properties or localized manage-bde output.
    # https://learn.microsoft.com/windows/win32/secprov/getprotectionstatus-win32-encryptablevolume
    # https://learn.microsoft.com/windows/win32/secprov/getconversionstatus-win32-encryptablevolume
    if (-not (Test-IsAdmin)) { return 'unknown' }
    try {
        if ($env:SystemDrive -notmatch '^[A-Za-z]:$') { return 'unknown' }
        $volumes = @(Get-CimInstance -Namespace 'root\CIMV2\Security\MicrosoftVolumeEncryption' -ClassName Win32_EncryptableVolume -Filter "DriveLetter='$($env:SystemDrive)'" -ErrorAction Stop)
        if ($volumes.Count -ne 1) { return 'unknown' }
        $protection = Invoke-CimMethod -InputObject $volumes[0] -MethodName GetProtectionStatus -ErrorAction Stop
        if ($null -eq $protection.ReturnValue -or [uint32]$protection.ReturnValue -ne 0 -or $null -eq $protection.ProtectionStatus) { return 'unknown' }
        switch ([uint32]$protection.ProtectionStatus) {
            1 { return 'on' }
            0 {
                $conversion = Invoke-CimMethod -InputObject $volumes[0] -MethodName GetConversionStatus -Arguments @{ PrecisionFactor = [uint32]0 } -ErrorAction Stop
                if ($null -eq $conversion.ReturnValue -or [uint32]$conversion.ReturnValue -ne 0 -or $null -eq $conversion.ConversionStatus) { return 'unknown' }
                if ([uint32]$conversion.ConversionStatus -eq 0) { return 'off' }
                if ([uint32]$conversion.ConversionStatus -eq 1) { return 'suspended' }
                # A conversion in progress/paused can change protection at completion.
                return 'unknown'
            }
            default { return 'unknown' }
        }
    } catch { return 'unknown' }
}
function Test-BitLockerConsentRequired {
    param([AllowNull()][string]$State)
    # Only verified unencrypted or suspended protection can skip this boot-change decision.
    return ($State -notin @('off', 'suspended'))
}
# What the installer does about BitLocker protection before it changes the start options, without asking anybody
# (owner, 2026-10-12: "Przestan wymagac potwierdzen od uzytkownika"). The install needs test signing, test signing
# needs a boot-option change, and a boot-option change on a protected volume makes Windows ask for the recovery key
# at the next start. So the installer suspends protection itself for the restarts it makes, and BitLocker turns
# itself on again afterwards (Suspend-BitLocker -RebootCount; Microsoft BitLocker PowerShell documentation).
#
# A decision only; the caller acts on it, so the rule is testable without a protected volume:
#   action  'none'      nothing to do: protection off, or already suspended
#           'suspend'   protection is on: suspend it for $Restarts restarts before the boot options change
#           'have-key'  the command line said the person has the recovery key (-BitLocker HaveKey): change nothing
#           'unknown'   the state could not be read: go on, and say so in one information line. A person who cannot
#                       read the state cannot answer a question about it either, and refusing the install here left
#                       every PC whose state is unreadable without a driver.
#   reboots the -RebootCount of a 'suspend': the restarts this install makes, at least 1 and at most 15 (the limit
#           of the cmdlet), so protection is back as soon as the install is through.
#   message one plain sentence for the console and the log.
# State: 'off', 'on', 'suspended' or 'unknown' (Get-BitLockerState). Requested: the -BitLocker switch, '' when absent.
function Get-BitLockerPlan {
    param([AllowNull()][string]$State, [AllowNull()][string]$Requested, [int]$Restarts = 2)
    $reboots = [Math]::Max(1, [Math]::Min(15, $Restarts))
    if (-not (Test-BitLockerConsentRequired $State)) {
        return [pscustomobject]@{ action = 'none'; reboots = 0; message = "BitLocker: nothing to do (protection $State)" }
    }
    if ($Requested -eq 'HaveKey') {
        return [pscustomobject]@{ action = 'have-key'; reboots = 0
            message = 'BitLocker: the command line states that the recovery key is at hand, so protection stays as it is' }
    }
    if ($State -eq 'on') {
        return [pscustomobject]@{ action = 'suspend'; reboots = $reboots
            message = "BitLocker: protection is on, so it is paused for the $reboots restart(s) of this installation and turns itself on again afterwards" }
    }
    return [pscustomobject]@{ action = 'unknown'; reboots = 0
        message = 'BitLocker: the protection state of this drive could not be read. The installation goes on; keep your recovery key at hand in case Windows asks for it after a restart' }
}
# A visible countdown that ends in a restart by itself (owner, 2026-10-12: automatic restarts, never a required
# click). Enter restarts at once, any other key puts the restart off, and a console that has no key reader (a
# scheduled run, redirected input) simply waits the countdown out and restarts. Returns 'now' or 'later'.
function Wait-RestartCountdown {
    param([int]$Seconds = 15)
    $left = $Seconds
    while ($left -gt 0) {
        Write-Host ("`r   Windows restarts by itself in {0,2} s. Enter = restart now, any other key = later.   " -f $left) -NoNewline
        $until = [DateTime]::UtcNow.AddSeconds(1)
        while ([DateTime]::UtcNow -lt $until) {
            $key = $null
            try { if ([Console]::KeyAvailable) { $key = [Console]::ReadKey($true) } } catch { $key = $null }
            if ($null -ne $key) {
                Write-Host ''
                if ($key.Key -eq 'Enter') { return 'now' }
                return 'later'
            }
            Start-Sleep -Milliseconds 100
        }
        $left--
    }
    Write-Host ''
    return 'now'
}
function Get-MemoryIntegrityState {
    $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -Name Enabled -ErrorAction SilentlyContinue).Enabled
    if ($v -eq 1) { return 'on' }
    return 'off'
}
# ---- the shortcuts of the control application -------------------------------------------------------------------
# The owner's instruction of 2026-10-12: "pytajmy tez czy zrobic skrot na pulpicie (domyslnie i menu start i
# pulpit)". The setup window offers both, both on by default, and neither of them gates the Install button. On the
# command line -NoStartMenuShortcut and -NoDesktopShortcut leave one or the other out.
#
# One list, used by the install, by the uninstall and by the footprint probe, so a shortcut the install writes
# cannot be one the uninstall does not know. The recovery entry stays in the Start menu only: it is a repair tool,
# not a second icon on the desktop.
#   kind      'control', 'recovery' or 'desktop icon'
#   where     'Start menu' or 'desktop', as the log and the dry run name the place
#   label      the whole name of this shortcut as a person reads it; the one field a line prints, because
#             "$where $kind" reads "desktop desktop icon" for the third row
#   switch     the install switch that leaves it out
#   arguments  the arguments of the shortcut's target
#   legacy     $true for a name an older release wrote (below); the install never writes such a row
#
# The names (owner, 2026-10-13: "Ikonka na pulpicie i w menu powinna miec nazwe zaczynajaca sie od Nagi"): the
# shortcuts carry the product name Nagi. Earlier releases wrote the same three shortcuts as
# "amdgpu-wddm Control". Those three old names stay in this list as legacy rows, so an install over an older release
# and the uninstall remove them, and the footprint sees them. A legacy row is removed only when its target is our own
# control application (Get-LegacyShortcutCleanup): a file of that name that points anywhere else is not ours and stays.
$script:ShortcutBrand = 'Nagi'
$script:ControlShortcutName = 'Nagi Control App'
$script:LegacyShortcutBrand = 'amdgpu-wddm'
$script:LegacyControlShortcutName = 'amdgpu-wddm Control'
function Get-ControlShortcutPaths {
    $startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
    $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
    if (-not $desktop) { $desktop = Join-Path $env:Public 'Desktop' }
    $out = @()
    foreach ($n in @(@($script:ControlShortcutName, $false), @($script:LegacyControlShortcutName, $true))) {
        $name = [string]$n[0]; $legacy = [bool]$n[1]
        $old = $(if ($legacy) { 'old ' } else { '' })
        $out += [pscustomobject]@{ kind = 'control'; where = 'Start menu'; label = "${old}Start menu entry"; switch = 'NoStartMenuShortcut'; arguments = ''; legacy = $legacy; path = (Join-Path $startMenu "$name.lnk") }
        $out += [pscustomobject]@{ kind = 'recovery'; where = 'Start menu'; label = "${old}Start menu repair entry"; switch = 'NoStartMenuShortcut'; arguments = '--recovery'; legacy = $legacy; path = (Join-Path $startMenu "$name (recovery).lnk") }
        $out += [pscustomobject]@{ kind = 'desktop icon'; where = 'desktop'; label = "${old}desktop icon"; switch = 'NoDesktopShortcut'; arguments = ''; legacy = $legacy; path = (Join-Path $desktop "$name.lnk") }
    }
    return $out
}

# The target of a shortcut file, as Windows reads it (IShellLink through WScript.Shell): the full path, or $null when
# the file is not there or cannot be read. Reading creates nothing: CreateShortcut opens an existing .lnk and writes
# only on Save, which this never calls.
function Read-ShortcutTarget([string]$Link) {
    if (-not (Test-Path -LiteralPath $Link -PathType Leaf)) { return $null }
    try { $t = [string](New-Object -ComObject WScript.Shell).CreateShortcut($Link).TargetPath } catch { return $null }
    if (-not $t) { return $null }
    return $t
}

# Pure: whether a shortcut target is one of our own executables. The same file compares equal whatever its letter
# case, its separators or a trailing dot segment; a relative or empty target is never ours.
function Test-OwnShortcutTarget([string]$Target, [string[]]$OwnTargets) {
    if (-not $Target) { return $false }
    if (-not [IO.Path]::IsPathRooted($Target)) { return $false }
    try { $t = [IO.Path]::GetFullPath($Target) } catch { return $false }
    foreach ($o in @($OwnTargets)) {
        if (-not $o) { continue }
        try { $f = [IO.Path]::GetFullPath([string]$o) } catch { continue }
        if ([string]::Equals($t, $f, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

# The control application's executable in every install root that this computer may know: the root of this run, the
# root the state or the release key recorded, and the default one. An older release wrote its shortcuts to the
# executable of its own root, which is one of these.
function Get-OwnControlTargets {
    param([string[]]$InstallRoots, [string]$ExeName = 'amdgpu_wddm_control.exe')
    $roots = @($InstallRoots) + @((Join-Path $env:ProgramFiles $script:ReleaseName))
    $out = @()
    foreach ($r in $roots) { if ($r) { $p = Join-Path ([string]$r) "control\$ExeName"; if ($out -notcontains $p) { $out += $p } } }
    return $out
}

# Pure: what to do with each legacy shortcut row that exists. ReadTarget is a script block that returns the target of
# one .lnk path ($null when unreadable); the install and the uninstall pass Read-ShortcutTarget, the unit tests their
# own. A row whose target is ours is removed; any other row is kept, with the reason, because a file of that name
# that points elsewhere was not written by us.
function Get-LegacyShortcutCleanup {
    param($Shortcuts, [scriptblock]$ReadTarget, [string[]]$OwnTargets)
    $out = @()
    foreach ($s in @($Shortcuts)) {
        if (-not $s -or -not [bool]$s.legacy) { continue }
        if (-not (Test-Path -LiteralPath $s.path -PathType Leaf)) { continue }
        $target = & $ReadTarget $s.path
        if (Test-OwnShortcutTarget $target $OwnTargets) { $out += [pscustomobject]@{ row = $s; action = 'remove'; target = $target; reason = 'it points at our control application' } }
        elseif ($target) { $out += [pscustomobject]@{ row = $s; action = 'keep'; target = $target; reason = "it points at $target, which is not ours" } }
        else { $out += [pscustomobject]@{ row = $s; action = 'keep'; target = $null; reason = 'its target cannot be read' } }
    }
    return $out
}

# Removes the legacy shortcuts of the control application that are ours (Get-LegacyShortcutCleanup) and says which
# ones stay. The install calls it after it wrote the new names, the uninstall with the rest of the shortcuts.
function Remove-LegacyControlShortcuts([string[]]$OwnTargets) {
    $rows = @(Get-LegacyShortcutCleanup -Shortcuts (Get-ControlShortcutPaths) -ReadTarget { param($p) Read-ShortcutTarget $p } -OwnTargets $OwnTargets)
    if (-not $rows.Count) { Write-Info "shortcuts of an older release ($($script:LegacyControlShortcutName)): none"; return }
    foreach ($c in $rows) {
        if ($c.action -eq 'remove') {
            Invoke-Change "remove the $($c.row.label) $($c.row.path): the name of an older release; $($c.reason)" {
                Remove-Item -LiteralPath $c.row.path -Force -ErrorAction SilentlyContinue
            } | Out-Null
        } else { Write-Info "kept: $($c.row.path), $($c.reason)" }
    }
}

# Pure: which shortcuts an install writes, for the two switches. Both absent = both places. A legacy row is never
# written.
function Get-ControlShortcutPlan {
    param([bool]$NoStartMenu, [bool]$NoDesktop, $Shortcuts = (Get-ControlShortcutPaths))
    $out = @()
    foreach ($s in @($Shortcuts)) {
        if ([bool]$s.legacy) { continue }
        if ($s.switch -eq 'NoStartMenuShortcut' -and $NoStartMenu) { continue }
        if ($s.switch -eq 'NoDesktopShortcut' -and $NoDesktop) { continue }
        $out += $s
    }
    return $out
}

# Writes one shortcut to the control application, and returns the path it wrote.
function New-ControlShortcut {
    param([Parameter(Mandatory)][string]$Link, [Parameter(Mandatory)][string]$Target, [string]$Arguments = '')
    [void][IO.Directory]::CreateDirectory((Split-Path $Link))
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($Link)
    $s.TargetPath = $Target
    if ($Arguments) { $s.Arguments = $Arguments }
    $s.WorkingDirectory = (Split-Path $Target)
    # The application's own icon, the one its window and its task-bar button show (art\icon\nagi.ico).
    $s.IconLocation = "$Target,0"
    $s.Save()
    return $Link
}

# The Nagi wallpaper (owner, 2026-10-13: after a successful install the wallpaper is set, unless the person cleared
# the box of the installer). The engine runs elevated, for whoever started it, and a picture is a setting of each
# person's own session, so the engine sets nothing itself: it records the choice for Nagi Control App in
# <SoftwareKey>\Release, Wallpaper (DWORD 1 or 0) and WallpaperStamp (REG_SZ, one value per install run). The
# application sets the picture once per stamp for each person who signs in, after the installer's state says
# 'verified' (tools/win/amdgpu_wddm_control/src/Wallpaper.cs). Pure: the two values.
function Get-WallpaperRecord([bool]$Selected, [string]$Version, [DateTime]$Utc) {
    return [ordered]@{ Wallpaper = $(if ($Selected) { 1 } else { 0 }); WallpaperStamp = ('{0} {1}' -f $Version, $Utc.ToUniversalTime().ToString('yyyyMMddTHHmmssZ')) }
}
# Writes the record. A failure is one line and never fails the install: the wallpaper is an extra.
function Set-WallpaperRecord([bool]$Selected, [string]$Version) {
    $rec = Get-WallpaperRecord -Selected $Selected -Version $Version -Utc ([DateTime]::UtcNow)
    $k = "$($script:SoftwareKey)\Release"
    Write-Info ("wallpaper: $(if ($Selected) { 'the Nagi wallpaper, set by Nagi Control App once for each person after the install is verified' } else { 'not set (-NoWallpaper)' })")
    Invoke-Change "$k`: Wallpaper = $($rec.Wallpaper), WallpaperStamp = $($rec.WallpaperStamp)" {
        try {
            Initialize-RegistryKey $k
            New-ItemProperty -LiteralPath $k -Name Wallpaper -Value $rec.Wallpaper -PropertyType DWord -Force | Out-Null
            New-ItemProperty -LiteralPath $k -Name WallpaperStamp -Value $rec.WallpaperStamp -PropertyType String -Force | Out-Null
        } catch { Write-Log "   wallpaper record not written: $($_.Exception.Message)" }
    } | Out-Null
}

# ---- the Microsoft Visual C++ runtime our programs need ---------------------------------------------------------
# Our control application, the CLI and the user-mode drivers are built against the Microsoft Visual C++ 2015-2022
# runtime. Until now a PC without it was refused at the preflight and the tester had to fetch the runtime from
# Microsoft. The owner's instruction of 2026-10-12 is "powinien byc auto-installer!!!": the package carries
# Microsoft's own installer (payload\redist\vc_redist.x64.exe, release-sources.json) and the install runs it.
# Running the installer is the consent for it, like every other change this engine makes.
$script:VcRuntimeFiles = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
$script:VcRuntimePackagePath = 'payload\redist\vc_redist.x64.exe'
# The version the package carries, and the lowest version we accept in System32. A runtime older than this is
# replaced: the 14.x runtime is binary compatible upwards, so Microsoft's installer simply moves it forward.
$script:VcRuntimeVersion = '14.44.35211.0'
$script:VcRuntimeMinMajor = 14
$script:VcRuntimeMinMinor = 44
$script:VcRuntimeSha256 = 'CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B'
$script:VcRuntimeSize = 25635768
$script:VcRuntimeSigner = 'Microsoft Corporation'
# Microsoft's command line and its exit codes (vc_redist.x64.exe /install /quiet /norestart):
#   0     installed, or this version is already there
#   1638  a newer version of the runtime is installed: nothing to do
#   3010, 1641  installed, and Windows wants a restart; this install restarts anyway
#   anything else is a failure, and the install stops with the code in the message
$script:VcRuntimeArguments = @('/install', '/quiet', '/norestart')
# 1638 means "a same or newer runtime is registered", and Microsoft's bootstrapper then installs nothing. A PC whose
# System32 lost one of the three files keeps that answer for ever, so the install runs the same file once more with
# /repair, which puts the files of the registered runtime back.
$script:VcRuntimeRepairArguments = @('/repair', '/quiet', '/norestart')
$script:VcRuntimeTimeoutSeconds = 600

# The file version of one System32 DLL as four numbers, or $null when the file is not there or has no version.
function Get-FileVersionNumbers([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $v = (Get-Item -LiteralPath $Path).VersionInfo
    if (-not $v) { return $null }
    return [pscustomobject]@{ major = [int]$v.FileMajorPart; minor = [int]$v.FileMinorPart; build = [int]$v.FileBuildPart; revision = [int]$v.FilePrivatePart }
}

# Pure: does a reading of the three runtime files mean the installer has to install the runtime?
# -Readings is a hashtable file name -> the object of Get-FileVersionNumbers ($null = the file is not there).
# Returns { install, missing, old, present, detail }: 'missing' is a file System32 does not have, 'old' a file
# older than the minimum. One of either makes 'install' true, because Microsoft's installer replaces the set.
function Get-VcRuntimeDecision {
    param([Parameter(Mandatory)][hashtable]$Readings, [int]$MinMajor = $script:VcRuntimeMinMajor, [int]$MinMinor = $script:VcRuntimeMinMinor)
    $missing = @(); $old = @(); $present = @()
    foreach ($f in $script:VcRuntimeFiles) {
        $v = $null
        if ($Readings.ContainsKey($f)) { $v = $Readings[$f] }
        if ($null -eq $v) { $missing += $f; continue }
        $text = "$f $($v.major).$($v.minor).$($v.build).$($v.revision)"
        if (($v.major -lt $MinMajor) -or ($v.major -eq $MinMajor -and $v.minor -lt $MinMinor)) { $old += $text } else { $present += $text }
    }
    $detail = @()
    if ($missing.Count) { $detail += "missing in System32: $($missing -join ', ')" }
    if ($old.Count) { $detail += "older than $MinMajor.$MinMinor`: $($old -join ', ')" }
    if ($present.Count) { $detail += "present: $($present -join ', ')" }
    return [pscustomobject]@{ install = [bool]($missing.Count -or $old.Count); missing = $missing; old = $old; present = $present; detail = ($detail -join '; ') }
}

# The same decision for the running PC.
# Host tests only, and only in a dry run (AMDGPU_WDDM_TEST_VCRUNTIME): 'missing' reads as a System32 without the three
# files, 'old' as a 14.40 runtime, 'ok' as the current one. A dry run changes nothing, so this lets the host tests walk
# the step that installs the runtime on a PC that already has it; a real install always reads System32.
function Get-VcRuntimeState {
    if ($script:DryRunMode -and $env:AMDGPU_WDDM_TEST_VCRUNTIME) {
        $fake = @{}
        foreach ($f in $script:VcRuntimeFiles) {
            switch ([string]$env:AMDGPU_WDDM_TEST_VCRUNTIME) {
                'missing' { $fake[$f] = $null }
                'old' { $fake[$f] = [pscustomobject]@{ major = 14; minor = 40; build = 33810; revision = 0 } }
                default { $fake[$f] = [pscustomobject]@{ major = 14; minor = 44; build = 35211; revision = 0 } }
            }
        }
        return Get-VcRuntimeDecision -Readings $fake
    }
    $readings = @{}
    foreach ($f in $script:VcRuntimeFiles) { $readings[$f] = Get-FileVersionNumbers (Join-Path $env:windir "System32\$f") }
    return Get-VcRuntimeDecision -Readings $readings
}

# Is the file the package carries Microsoft's installer, unchanged? The SHA-256 first, then the Authenticode
# signature: Valid, and signed by Microsoft Corporation. -SignatureReader is for the tests, which hand in readings
# instead of asking Windows; the default asks Windows.
# Returns { ok, problem, sha256, status, signer }.
function Test-VcRuntimeInstaller {
    param([Parameter(Mandatory)][string]$Path, [string]$ExpectedSha256 = $script:VcRuntimeSha256,
          [string]$ExpectedSigner = $script:VcRuntimeSigner, [scriptblock]$SignatureReader)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{ ok = $false; problem = "the package does not carry $Path"; sha256 = $null; status = $null; signer = $null }
    }
    $sha = Get-Sha256 $Path
    if ($sha -ne $ExpectedSha256) {
        return [pscustomobject]@{ ok = $false; problem = "SHA256 of $Path is $($sha.Substring(0, 16)), the installer expects $($ExpectedSha256.Substring(0, 16))"; sha256 = $sha; status = $null; signer = $null }
    }
    $reader = $(if ($SignatureReader) { $SignatureReader } else { { param($p) Get-AuthenticodeSignature -LiteralPath $p } })
    $sig = & $reader $Path
    $status = [string]$(if ($sig) { $sig.Status } else { 'unknown' })
    $signer = [string]$(if ($sig -and $sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' })
    if ($status -ne 'Valid') {
        return [pscustomobject]@{ ok = $false; problem = "the signature of $Path is not valid (status $status)"; sha256 = $sha; status = $status; signer = $signer }
    }
    if ($signer -notmatch ("CN=" + [regex]::Escape($ExpectedSigner) + "(,|$)")) {
        return [pscustomobject]@{ ok = $false; problem = "$Path is not signed by $ExpectedSigner (signer '$signer')"; sha256 = $sha; status = $status; signer = $signer }
    }
    return [pscustomobject]@{ ok = $true; problem = $null; sha256 = $sha; status = $status; signer = $signer }
}

# Pure: what one exit code of Microsoft's installer means. { ok, restart, message }.
function Get-VcRuntimeOutcome([int]$Code) {
    switch ($Code) {
        0 { return [pscustomobject]@{ ok = $true; restart = $false; message = 'the Microsoft Visual C++ runtime is installed' } }
        1638 { return [pscustomobject]@{ ok = $true; restart = $false; message = 'a newer Microsoft Visual C++ runtime is already installed: nothing to do' } }
        3010 { return [pscustomobject]@{ ok = $true; restart = $true; message = 'the Microsoft Visual C++ runtime is installed and Windows wants a restart, which this installation makes anyway' } }
        1641 { return [pscustomobject]@{ ok = $true; restart = $true; message = 'the Microsoft Visual C++ runtime is installed and Windows wants a restart, which this installation makes anyway' } }
        default { return [pscustomobject]@{ ok = $false; restart = $false; message = "Microsoft's installer for the Visual C++ runtime ended with code $Code. Install the Microsoft Visual C++ 2015-2022 Redistributable (x64) yourself, then run this installer again." } }
    }
}

# Runs Microsoft's installer and reads its exit code. -Runner is for the tests (it gets the path and the arguments
# and returns the exit code); the default runs the program under a deadline, so a hung installer cannot hold the
# installation for ever. Returns the object of Get-VcRuntimeOutcome with the code and the output in it.
function Invoke-VcRuntimeInstall {
    param([Parameter(Mandatory)][string]$Path, [scriptblock]$Runner, [double]$TimeoutSeconds = $script:VcRuntimeTimeoutSeconds,
          [string[]]$Arguments = $script:VcRuntimeArguments)
    $code = $null; $text = ''
    if ($Runner) { $code = [int](& $Runner $Path $Arguments) }
    else {
        $n = Invoke-NativeBounded -File $Path -Arguments $Arguments -TimeoutSeconds $TimeoutSeconds
        $text = [string]$n.text
        if ($n.timed_out) {
            return [pscustomobject]@{ ok = $false; restart = $false; code = $null; text = $text
                message = "Microsoft's installer for the Visual C++ runtime did not finish within $TimeoutSeconds s. Install the Microsoft Visual C++ 2015-2022 Redistributable (x64) yourself, then run this installer again." }
        }
        $code = [int]$n.code
    }
    $out = Get-VcRuntimeOutcome $code
    return [pscustomobject]@{ ok = $out.ok; restart = $out.restart; message = $out.message; code = $code; text = $text }
}

# ---- which Windows this driver is made for ---------------------------------------------------------------------
# The owner's policy of 2026-10-11 is n-2: "wystarczy nam wsparcie n-2 czyli maks do 23H2 jak obecnie + nowsze ale
# canary to raczej nie". Supported are Windows 11 23H2 (22631), 24H2 (26100), 25H2 (26200) and the general builds
# after them. A Windows 11 older than 22631 is a line that says so and advises an update (owner, 2026-10-12: "tylko
# warning, nie blokada"), never a refusal. An Insider build and an LTSC edition are lines of the same kind. The one
# refusal is Windows 10 and older (below build 22000), and 32-bit Windows.
$script:WindowsSupportedBuilds = @(22631, 26100, 26200)       # 23H2, 24H2, 25H2
$script:WindowsSupportedNames = '23H2, 24H2, 25H2'
$script:WindowsMinimumBuild = 22000                            # below this: Windows 10 or older, refused
$script:WindowsLowestSupportedBuild = 22631                    # 22000-22630: supported versions named, install goes on
$script:WindowsInsiderBuild = 27000                            # at or above: a build Microsoft has not released yet
# An Insider channel on one of the supported builds (Release Preview on 23H2, 24H2 or 25H2) is the version this
# driver is tested on, so it gets no Insider line: only a build outside $script:WindowsSupportedBuilds does.
$script:WindowsSelfHostKey = 'HKLM:\SOFTWARE\Microsoft\WindowsSelfHost\Applicability'
$script:WindowsCurrentVersionKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$script:WindowsLtscEditions = @('EnterpriseS', 'IoTEnterpriseS')

# Pure: what this engine says about a Windows. The readings come from Get-WindowsSupportState on a real PC and
# from the tests on every other. Returns { x64, build, standing, findings }, where findings is the list the
# preflight turns into its lines, in order: { id, severity, detail }. Only 'fail' refuses the install.
#   standing: 'not-x64' | 'too-old' | 'below-supported' | 'supported'
function Get-WindowsSupportFindings {
    param([Parameter(Mandatory)][int]$Build, [string]$Caption = 'Windows', [bool]$Is64Bit = $true,
          [string]$EditionId = '', [string]$SelfHostBranch = '', [string]$DisplayVersion = '')
    $what = "$Caption build $Build"
    if ($DisplayVersion) { $what = "$Caption $DisplayVersion (build $Build)" }
    $findings = @()
    $standing = 'supported'
    if (-not $Is64Bit) {
        $standing = 'not-x64'
        $findings += [pscustomobject]@{ id = 'windows.not-x64'; severity = 'fail'; detail = "$what is 32-bit; x64 is required" }
    } elseif ($Build -lt $script:WindowsMinimumBuild) {
        $standing = 'too-old'
        $findings += [pscustomobject]@{ id = 'windows.too-old'; severity = 'fail'
            detail = "$what; Windows 11 (build $($script:WindowsMinimumBuild) or later) is required" }
    } elseif ($Build -lt $script:WindowsLowestSupportedBuild) {
        $standing = 'below-supported'
        $findings += [pscustomobject]@{ id = 'windows.below-supported'; severity = 'warn'
            detail = "$what is older than the versions this driver is tested on (Windows 11 $($script:WindowsSupportedNames), builds $($script:WindowsSupportedBuilds -join ', ')). Update Windows. The installation continues." }
    } else {
        $findings += [pscustomobject]@{ id = 'windows.ok'; severity = 'ok'; detail = $what }
    }
    # Beyond the version itself: the channel and the edition. Both are information, whatever the version said, and
    # neither is asked of the user. A refused Windows gets no second line: nothing is going to be installed there.
    if ($standing -ne 'not-x64' -and $standing -ne 'too-old') {
        # The channel alone is not the whole signal: a PC enrolled in the Insider programme (Release Preview above
        # all) can still run one of the supported general builds, and that build is what this driver is tested on.
        # The line is therefore for a build that is not one of the supported ones: an enrolled PC on a build of its
        # own, or a build Microsoft has not released at all.
        $enrolledOnOtherBuild = $SelfHostBranch -and (@($script:WindowsSupportedBuilds) -notcontains $Build)
        if ($enrolledOnOtherBuild -or $Build -ge $script:WindowsInsiderBuild) {
            $which = if ($SelfHostBranch) { "Insider channel $SelfHostBranch" } else { "build $Build, which Microsoft has not released yet" }
            $findings += [pscustomobject]@{ id = 'windows.insider'; severity = 'warn'
                detail = "${which}: an Insider build of Windows is not supported (Windows 11 $($script:WindowsSupportedNames)). The installation continues." }
        }
        if (@($script:WindowsLtscEditions) -contains $EditionId) {
            $findings += [pscustomobject]@{ id = 'windows.ltsc'; severity = 'warn'
                detail = "edition ${EditionId}: this driver is not tested on the LTSC editions of Windows. The installation continues." }
        }
    }
    return [pscustomobject]@{ x64 = $Is64Bit; build = $Build; standing = $standing; findings = $findings }
}

# The same for the running PC. The channel comes from the key Windows Update writes for an Insider PC, the edition
# from CurrentVersion. A value that is not there reads as empty, which is the ordinary case.
function Get-WindowsSupportState {
    param($Os)
    if (-not $Os) { $Os = Get-CimInstance Win32_OperatingSystem }
    $cv = Get-ItemProperty -LiteralPath $script:WindowsCurrentVersionKey -ErrorAction SilentlyContinue
    $sh = Get-ItemProperty -LiteralPath $script:WindowsSelfHostKey -ErrorAction SilentlyContinue
    $branch = ''
    if ($sh) { foreach ($n in 'BranchName', 'Ring', 'ContentType') { if (-not $branch -and $sh.$n) { $branch = [string]$sh.$n } } }
    return Get-WindowsSupportFindings -Build ([int]$Os.BuildNumber) -Caption ([string]$Os.Caption) `
        -Is64Bit ([bool][Environment]::Is64BitOperatingSystem) `
        -EditionId $(if ($cv) { [string]$cv.EditionID } else { '' }) -SelfHostBranch $branch `
        -DisplayVersion $(if ($cv -and $cv.DisplayVersion) { [string]$cv.DisplayVersion } else { '' })
}
# The UserModeDriverName slots of the GPU's software key: D3D9, D3D10, D3D11, D3D12. The D3D9 slot holds '<>', the
# display kernel's "no user-mode driver" marker: the D3D9 runtime gets no driver name for the adapter and falls back to
# Microsoft's D3D9On12 layer on our D3D12 driver. An empty slot did the same from tester.18 to tester.29, but newer
# display kernels refuse an empty first entry for a full adapter: 26100.9549 stops the adapter with "Invalid user mode
# driver name" (0xC0000182, Code 43), and a field report shows the same on 22631.7584; the lab's 22631.6199 accepted
# it. '<>' passes that length test, and the kernel answers the D3D9 name query for it as for an empty slot. A slot
# that names a UMD does not reach D3D9On12: the stub of the releases up to tester.17 (bc250umd.dll) and d3d9on12.dll
# itself both end CreateDeviceEx with D3DERR_NOTAVAILABLE (tools/win/d3d9probe, unit A, 2026-10-06 and 2026-10-12).
$script:D3d9SlotName = '<>'
function Get-UmdNames([string]$InstallRoot) {
    $router = Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'
    return @($script:D3d9SlotName, $router, $router, (Join-Path $InstallRoot 'd3d12\amdgpu_wddm_d3d12.dll'))
}
# One UserModeDriverName value as the log and the dry run print it: an empty slot is ''.
function Format-UmdNames([string[]]$Names) {
    return (@($Names | ForEach-Object { if ($_) { $_ } else { "''" } }) -join ' | ')
}
# The 64-bit registration as verify checks it; returns the problems found (none: registered).
function Test-UmdRegistration {
    param([Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$ClassKey)
    $problems = @()
    $c = Get-ItemProperty -LiteralPath $ClassKey -ErrorAction SilentlyContinue
    $want = Get-UmdNames $InstallRoot
    if ($null -eq $c.UserModeDriverName) { $problems += 'UserModeDriverName is missing' }
    else {
        $have = @($c.UserModeDriverName)
        if (($have.Count -ne $want.Count) -or (($have -join '|') -ne ($want -join '|'))) { $problems += "UserModeDriverName is $(Format-UmdNames $have), expected $(Format-UmdNames $want)" }
    }
    $icd = Join-Path $InstallRoot 'vulkan\radeon_icd.json'
    if ((@($c.VulkanDriverName) -join '|') -ne $icd) { $problems += "VulkanDriverName is '$(@($c.VulkanDriverName) -join ' | ')'" }
    return $problems
}
# BD-064: 32-bit processes. UserModeDriverNameWow mirrors the four slots of UserModeDriverName with the x86 builds:
# the D3D9 slot, the D3D10 and D3D11 routers, and the x86 D3D12 shell. As on x64 the D3D12 slot names the shell
# itself, not a router, and the shell loads its engine and ICD from its own directory (wow64\d3d12), so the D3D12 pair
# needs no Vulkan ICD registration. With the D3D12 slot a 32-bit D3D9 application gets D3D9On12 on our GPU, the same
# route as a 64-bit one. The x86 router and the x86 D3D12 shell read the 64-bit policy keys; the router also reads its
# own *Wow path values. The x86 builds link the C runtime statically: no x86 Visual C++ runtime is needed.
function Get-WowUmdNames([string]$InstallRoot) {
    $router = Join-Path $InstallRoot 'wow64\desktop\bc250d3d_router.dll'
    return @($script:D3d9SlotName, $router, $router, (Join-Path $InstallRoot 'wow64\d3d12\amdgpu_wddm_d3d12.dll'))
}
function Get-WowFiles([string]$InstallRoot) {
    return @('desktop\bc250d3d_router.dll', 'desktop\bc250d3d.dll', 'd3d11\amdgpu_wddm_d3d11.dll', 'd3d11\amdgpu_wddm_dxvk.dll', 'd3d11\amdgpu_wddm_radv.dll',
        'd3d12\amdgpu_wddm_d3d12.dll', 'd3d12\amdgpu_wddm_vkd3d.dll', 'd3d12\amdgpu_wddm_radv.dll', 'vulkan\vulkan_radeon.dll' | ForEach-Object { Join-Path $InstallRoot "wow64\$_" })
}
# The D3D9 stub that the releases up to 0.7.213.102-tester.17 installed in System32 and SysWOW64. No slot names it any
# more; install and uninstall take it away (unless it was there before the first install of ours), and the footprint
# reports a leftover.
function Get-LegacyStubPaths {
    return @(
        [pscustomobject]@{ path = (Join-Path $env:windir 'System32\bc250umd.dll'); flag = 'stub_existed' }
        [pscustomobject]@{ path = (Join-Path $env:windir 'SysWOW64\bc250umd.dll'); flag = 'stub_wow_existed' })
}
# The machine field of a PE image: 0x14C x86, 0x8664 x64; $null when the file is missing or not a PE image.
function Get-PeMachine([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $b = New-Object byte[] 4096
        $n = $fs.Read($b, 0, $b.Length)
        if ($n -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
        $pe = [BitConverter]::ToInt32($b, 60)
        if ($pe -lt 0 -or $pe + 6 -gt $n -or [BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return $null }
        return [int][BitConverter]::ToUInt16($b, $pe + 4)
    } finally { $fs.Dispose() }
}
# The undecorated export names of a PE image (x86 or x64), read from its export directory. The release gate and
# verify use it on the OpenGL ICD (BD-076); test-wow64.ps1 carries the same reader for the x86 payload.
function Get-PeExportNames([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($b, 60)
    $magic = [BitConverter]::ToUInt16($b, $pe + 24)
    $dd = $pe + 24 + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $rva = [BitConverter]::ToUInt32($b, $dd)
    if (-not $rva) { return @() }
    $nsec = [BitConverter]::ToUInt16($b, $pe + 6)
    $sec = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
    $toOff = {
        param([uint32]$r)
        for ($i = 0; $i -lt $nsec; $i++) {
            $s = $sec + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12); $sz = [BitConverter]::ToUInt32($b, $s + 8); $raw = [BitConverter]::ToUInt32($b, $s + 20)
            if ($r -ge $va -and $r -lt $va + [Math]::Max($sz, [BitConverter]::ToUInt32($b, $s + 16))) { return [int]($r - $va + $raw) }
        }
        throw "RVA $r outside the sections of $Path"
    }
    $ed = & $toOff $rva
    $n = [BitConverter]::ToUInt32($b, $ed + 24)
    $names = & $toOff ([BitConverter]::ToUInt32($b, $ed + 32))
    $out = @()
    for ($i = 0; $i -lt $n; $i++) {
        $o = & $toOff ([BitConverter]::ToUInt32($b, $names + 4 * $i))
        $e = $o; while ($b[$e]) { $e++ }
        $out += [Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    return $out
}
# The 32-bit registration as verify checks it; returns the problems found (none: registered).
function Test-WowRegistration {
    param([Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$ClassKey, [string]$KhronosKey = $script:KhronosKeyWow,
        [string]$SoftwareKey = $script:SoftwareKey, [string[]]$Files = (Get-WowFiles $InstallRoot))
    $problems = @()
    $c = Get-ItemProperty -LiteralPath $ClassKey -ErrorAction SilentlyContinue
    $want = Get-WowUmdNames $InstallRoot
    if ($null -eq $c.UserModeDriverNameWow) { $problems += 'UserModeDriverNameWow is missing' }
    else {
        $have = @($c.UserModeDriverNameWow)
        if (($have.Count -ne $want.Count) -or (($have -join '|') -ne ($want -join '|'))) { $problems += "UserModeDriverNameWow is $(Format-UmdNames $have), expected $(Format-UmdNames $want)" }
    }
    $icd = Join-Path $InstallRoot 'wow64\vulkan\radeon_icd.json'
    if ((@($c.VulkanDriverNameWow) -join '|') -ne $icd) { $problems += "VulkanDriverNameWow is '$(@($c.VulkanDriverNameWow) -join ' | ')'" }
    $k = Get-Item -LiteralPath $KhronosKey -ErrorAction SilentlyContinue
    if (-not $k -or $k.GetValue($icd) -ne 0) { $problems += "$KhronosKey has no '$icd' = 0" }
    $cpu = (Get-ItemProperty -LiteralPath "$SoftwareKey\DesktopRouter" -Name CpuUmdPathWow -ErrorAction SilentlyContinue).CpuUmdPathWow
    if ($cpu -ne (Join-Path $InstallRoot 'wow64\desktop\bc250d3d.dll')) { $problems += "DesktopRouter CpuUmdPathWow is '$cpu'" }
    $gpu = (Get-ItemProperty -LiteralPath "$SoftwareKey\AppRouter" -Name GpuUmdPathWow -ErrorAction SilentlyContinue).GpuUmdPathWow
    if ($gpu -ne (Join-Path $InstallRoot 'wow64\d3d11\amdgpu_wddm_d3d11.dll')) { $problems += "AppRouter GpuUmdPathWow is '$gpu'" }
    foreach ($f in $Files) {
        $m = Get-PeMachine $f
        if ($null -eq $m) { $problems += "$f missing or not an image" } elseif ($m -ne 0x14C) { $problems += ('{0} is not x86 (machine 0x{1:X})' -f $f, $m) }
    }
    return $problems
}
# ---------------------------------------------------------------------------------------------------------------
# The OpenGL installable client driver (BD-076, GitHub issue #8). Windows' own opengl32.dll asks the adapter which
# ICD to load with D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO) and gets a D3DKMT_OPENGLINFO back: the ICD's file
# name, its version and its flags (d3dkmthk.h lines 1851-1856 and KMTQAITYPE_UMOPENGLINFO = 2 at line 2365, WDK
# 10.0.26100; ref\ddi-display\d3dkmthk.md section _D3DKMT_OPENGLINFO). The three fields come out of the registry: the
# operating system reads OpenGLDriverName, OpenGLVersion and OpenGLFlags from the display adapter's own software
# (class) key, and the ...Wow names for a 32-bit process
# (ref\windows-driver-docs\windows-driver-docs-pr\display\loading-an-opengl-installable-client-driver.md: line 28
# gives the key as HKLM/System/CurrentControlSet/Control/Class/{Adapter GUID}/0000/, lines 31-40 the two tables).
# Read that page exactly: it introduces the first table as the entries "for 32-bit Windows Vista display drivers
# that are used on 32-bit Windows Vista" and the second as the same drivers "used on 64-bit Windows Vista", and it
# says nothing about a 64-bit ICD on 64-bit Windows. That the non-Wow triple is the native one and the Wow triple
# the one a WoW64 process gets is read from three other places, not from that page: the WDDM 2.1 driver-store
# example, which writes OpenGLDriverName with a 64-bit DLL and OpenGLDriverNameWoW beside it
# (display\wddm-2-1-features.md lines 214-215), AMD's shipping INF, which does the same on an x64-only package, and
# this installer's own UserModeDriverName / UserModeDriverNameWow pair, which the same query path reads and which
# works on the lab. The index in the page is 0000; the real key is whichever four-digit subkey the device has, and
# Get-DeviceDriverKey below takes it from the device instead of guessing.
# No DDI of ours takes part, and the driver is not asked: these values are the whole registration. Every OpenGL
# application of the computer whose architecture this release registers then gets our ICD, with no DLL beside its
# executable; an application of any other architecture keeps the renderer Windows gives it.
#
# Why the installer writes them and the INF does not: the ICD files live under the install root
# (%ProgramFiles%\amdgpu-wddm\opengl and ...\wow64\opengl), which an INF AddReg line cannot name (it can only name
# the driver store, %13%), and the installer already owns the neighbouring values of the same key
# (UserModeDriverName, UserModeDriverNameWow, VulkanDriverName, VulkanDriverNameWow: install.ps1). The INF's own
# user-mode AddReg block is the disabled M7 stage-A experiment (driver\kmd\bc250kmd.inf), not a registration route.
#
# The value type: the Learn page above calls OpenGLDriverName a REG_SZ, which predates WDDM; Microsoft's own WDDM 2.1
# driver-store example writes a REG_MULTI_SZ
# (ref\windows-driver-docs\windows-driver-docs-pr\display\wddm-2-1-features.md lines 214-215), and so does every
# shipping WDDM display INF that was read for this (AMD's own Adrenalin INF writes OpenGLDriverName and
# OpenGLDriverNameWoW as REG_MULTI_SZ with OpenGLVersion 1 and OpenGLFlags 3). UserModeDriverName, which this
# installer writes as a REG_MULTI_SZ, is read by the same query path and works on the lab, so the ICD name is written
# the same way. release-sources.json can say "value_type": "REG_SZ" instead, which is the one-line fallback if a lab
# run ever shows that the runtime does not load the ICD.
#
# The absolute path is the part of this registration that no reference writes: Microsoft's example writes %13%
# (the driver store), AMD's primary INF a bare atio6axx.dll, and VirtualBox a bare VBoxGL with no extension at all.
# So if a lab run shows the ICD is never loaded, the order to try is: value_type REG_SZ (this file, one edit), and
# then the driver store with an INF AddReg line, which means the ICD files have to ride in the driver package.
# A bare file name is NOT a step of that chain for this release: the runtime would then load it through the loader
# search path of an arbitrary application, and %ProgramFiles%\amdgpu-wddm\opengl is not on anybody's search path.
# The directory that is would be System32 and SysWOW64, and this installer deliberately writes nothing there
# (docs\testing\INSTALL.md). The same query also carries a componentized-driver form, where the value name is
# prefixed with D3DKMT_COMPONENTIZED_INDICATOR (L'#') and the subkey D3DKMT_SUBKEY_OPENGL (L"OpenGL") holds the
# values (d3dkmthk.h lines 1847 and 1849); our driver package is not componentized, so the plain key is the one read.
#
# The Wow spelling: the Learn table spells it OpenGLDriverNameWow, the WDDM 2.1 example OpenGLDriverNameWoW, and
# AMD's one INF uses both spellings in different sections of itself. Registry value names are not case sensitive, so
# the two name one value; this installer writes the spelling of the Learn table.
#
# OpenGLVersion is the number the runtime compares before it loads the ICD. Mesa's WGL front end takes the number and
# ignores it (DrvValidateVersion, src\gallium\frontends\wgl\stw_device.c), so the value only has to be the one the
# runtime accepts: 1, which is what AMD's INF and VirtualBox's Mesa registration both write. OpenGLFlags is a bit
# mask: bit 0 (0x1) "is set for compatibility", bit 1 (0x2) tells the runtime not to call the ICD's finish function
# before its swap-buffers function (the Learn table above). This release writes 1: bit 0 only, so the runtime keeps
# its finish call before every swap, which is the conservative half of AMD's 3.
#
# Which architectures a release registers: manifest.json "opengl_icd" carries the list ("architectures"), and the
# release writes the three values of each architecture in it and nothing else. This release lists x64 alone.
# The reason is measured, not assumed. On unit A, 2026-10-12 13:05-13:08Z, the x64 ICD passed every case of
# tools\win\gltest (core 4.6, renderer "zink Vulkan 1.4(AMD BC-250 (RADV GFX1013) (MESA_RADV))", draw, present,
# threads and compute exact), and the x86 ICD failed every case with one debug string from the loaded DLL:
# "MESA: error: ZINK: failed to load vulkan-1.dll". The 32-bit half of the registration was correct down to the
# kernel answer (glprobe: the kernel named the x86 file, version 1, flags 0x1, opengl32 mapped it and
# DrvValidateVersion returned TRUE), so the ICD itself is not the fault: this computer has no 32-bit Vulkan loader,
# and zink has no Vulkan to run on. A 32-bit registration would therefore trade the Windows software renderer,
# which is OpenGL 1.1 but draws, for an ICD that loads and gives a 32-bit program no context at all. So the list
# decides, not the presence of a file, and a later release that ships a 32-bit Vulkan loader adds "x86" to it with
# no other change: every function below works from the list, and the x86 half of them is tested either way
# (tools\release\test-opengl-icd.ps1).
$script:OpenGlPackagePath    = 'payload/opengl/amdgpu_wddm_gl.dll'
$script:OpenGlPackagePathWow = 'payload/wow64/opengl/amdgpu_wddm_gl.dll'
$script:OpenGlVersion        = 1
$script:OpenGlFlags          = 1
$script:OpenGlValueType      = 'REG_MULTI_SZ'
# The architectures of this release, and every architecture this installer knows. A release switch without an
# architecture list gets the first of these, which is this release's own answer: x64 only.
$script:OpenGlArchitectures     = @('x64')
$script:OpenGlArchitecturesKnown = @('x64', 'x86')
# The six values of the registration, the 64-bit three first. The order is the order the installer writes them in and
# the dry run prints them in. All six, whatever this release registers: the record and the restore cover every value
# of ours that any release of this family may have written, so an architecture that leaves the list is taken away
# again instead of being left behind naming a file that is gone.
$script:OpenGlValueNames = @('OpenGLDriverName', 'OpenGLVersion', 'OpenGLFlags', 'OpenGLDriverNameWow', 'OpenGLVersionWow', 'OpenGLFlagsWow')
# The three values of each architecture, in the order they are written.
$script:OpenGlValueNamesByArch = @{ x64 = @('OpenGLDriverName', 'OpenGLVersion', 'OpenGLFlags')
    x86 = @('OpenGLDriverNameWow', 'OpenGLVersionWow', 'OpenGLFlagsWow') }
# The name value of each half: the value that names the ICD file, for itself and for the two numbers beside it. A
# number alone says nothing about who wrote it (every registration of this family writes 1 and 1), so a number is
# always judged with the name value of its own half. Used by the record and by the restore plan, which is why it is
# one table here and not a copy in each of them.
$script:OpenGlValueHalf = @{ OpenGLDriverName = 'OpenGLDriverName'; OpenGLVersion = 'OpenGLDriverName'; OpenGLFlags = 'OpenGLDriverName'
    OpenGLDriverNameWow = 'OpenGLDriverNameWow'; OpenGLVersionWow = 'OpenGLDriverNameWow'; OpenGLFlagsWow = 'OpenGLDriverNameWow' }
# The entry points the OpenGL runtime calls in an ICD, as Mesa's WGL front end exports them
# (src\gallium\targets\wgl\gallium_wgl.def.in; the @N suffixes of that file are stripped for a Visual C++ build, so
# the names are undecorated in both architectures). A file that lacks one of these is no ICD, whatever its name says,
# and the release gate refuses the package.
# All nineteen, not the twelve a context needs: Windows' own opengl32.dll holds twenty Drv names in its string table
# (the nineteen below and DrvSwapMultipleBuffers), and every shipping ICD measured here exports the same nineteen -
# NVIDIA's nvoglv64.dll and this project's own build - so a file that is short of one of them is a file whose module
# definition file was cut, not a driver the runtime can be trusted to drive. DrvSwapMultipleBuffers is the one name
# that is optional: nvoglv64.dll does not export it either.
$script:OpenGlDrvExports = @('DrvCopyContext', 'DrvCreateContext', 'DrvCreateLayerContext', 'DrvDeleteContext',
    'DrvDescribeLayerPlane', 'DrvDescribePixelFormat', 'DrvGetLayerPaletteEntries', 'DrvGetProcAddress',
    'DrvPresentBuffers', 'DrvRealizeLayerPalette', 'DrvReleaseContext', 'DrvSetCallbackProcs', 'DrvSetContext',
    'DrvSetLayerPaletteEntries', 'DrvSetPixelFormat', 'DrvShareLists', 'DrvSwapBuffers', 'DrvSwapLayerBuffers',
    'DrvValidateVersion')

# What this release does about the OpenGL ICD: manifest.json "opengl_icd" (release-sources.json in the build), the
# same switch shape as the H.264 encoder's. Pure but for the two file probes, which only run with a package root.
function Get-OpenGlReleaseSwitch {
    param($Manifest, [string]$PackageRoot)
    $s = $null
    if ($Manifest -and $Manifest.PSObject.Properties['opengl_icd']) { $s = $Manifest.opengl_icd }
    $register = $false
    if ($s -and $s.register) { $register = $true }
    $rel = $script:OpenGlPackagePath
    if ($s -and $s.package_path) { $rel = [string]$s.package_path }
    $relWow = $script:OpenGlPackagePathWow
    if ($s -and $s.package_path_wow) { $relWow = [string]$s.package_path_wow }
    if ($rel -notmatch '^payload/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+\.dll$') { throw "opengl_icd package_path '$rel': expected payload/<directory>/<name>.dll" }
    if ($relWow -notmatch '^payload/wow64/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+\.dll$') { throw "opengl_icd package_path_wow '$relWow': expected payload/wow64/<directory>/<name>.dll" }
    $version = $script:OpenGlVersion
    if ($s -and $null -ne $s.version) { $version = [int]$s.version }
    $flags = $script:OpenGlFlags
    if ($s -and $null -ne $s.flags) { $flags = [int]$s.flags }
    $type = $script:OpenGlValueType
    if ($s -and $s.value_type) { $type = [string]$s.value_type }
    if (@('REG_MULTI_SZ', 'REG_SZ') -notcontains $type) { throw "opengl_icd value_type '$type': expected REG_MULTI_SZ or REG_SZ" }
    if ($version -lt 1) { throw "opengl_icd version '$version': expected 1 or more" }
    if ($flags -lt 0) { throw "opengl_icd flags '$flags': expected 0 or more" }
    # The architecture list. A switch that names none of the known architectures, or a name this installer does not
    # know, is a refusal and not a release that silently registers nothing: a package whose list is a typo would
    # otherwise install the files and write no value at all.
    $arch = $script:OpenGlArchitectures
    if ($s -and $s.PSObject.Properties['architectures'] -and $null -ne $s.architectures) { $arch = @($s.architectures | ForEach-Object { [string]$_ }) }
    if (-not @($arch).Count) { throw "opengl_icd architectures: expected at least one of $($script:OpenGlArchitecturesKnown -join ', ')" }
    $unknown = @($arch | Where-Object { $script:OpenGlArchitecturesKnown -notcontains $_ })
    if ($unknown.Count) { throw "opengl_icd architectures '$($unknown -join ', ')': expected $($script:OpenGlArchitecturesKnown -join ' or ')" }
    if (@($arch | Select-Object -Unique).Count -ne @($arch).Count) { throw "opengl_icd architectures '$($arch -join ', ')': each architecture once" }
    # In this installer's own order, whatever order the package wrote them in, so every line and every plan of a run
    # names them the same way.
    $arch = @($script:OpenGlArchitecturesKnown | Where-Object { $arch -contains $_ })
    $source = $null; $sourceWow = $null; $present = $false; $presentWow = $false
    if ($PackageRoot) {
        $source = Join-Path $PackageRoot ($rel -replace '/', '\')
        $sourceWow = Join-Path $PackageRoot ($relWow -replace '/', '\')
        $present = Test-Path -LiteralPath $source -PathType Leaf
        $presentWow = Test-Path -LiteralPath $sourceWow -PathType Leaf
    }
    return [ordered]@{
        register         = $register
        architectures    = $arch                                              # the architectures this release registers
        register_x64     = [bool]($arch -contains 'x64')
        register_x86     = [bool]($arch -contains 'x86')
        package_path     = $rel
        package_path_wow = $relWow
        payload_dir      = ($rel -split '/')[1]                               # the payload directory install.ps1 copies
        install_path     = ($rel -replace '^payload/', '') -replace '/', '\'  # under the install root
        install_path_wow = ($relWow -replace '^payload/', '') -replace '/', '\'
        source           = $source
        source_wow       = $sourceWow
        present          = $present
        present_wow      = $presentWow
        version          = $version
        flags            = $flags
        value_type       = $type
    }
}
# The files a release has to carry, one row per architecture it registers: the package path, the file in the package,
# whether it is there, the PE machine the gates expect and the path under the install root. Pure but for the probes
# Get-OpenGlReleaseSwitch already did. Every gate and the installer walk this list instead of naming x64 and x86
# themselves, so adding "x86" to the list is the whole change a 32-bit release needs.
function Get-OpenGlArchFiles {
    param([Parameter(Mandatory)]$Switch)
    $rows = @()
    foreach ($a in @($Switch.architectures)) {
        if ($a -eq 'x64') {
            $rows += [pscustomobject]@{ arch = 'x64'; package_path = $Switch.package_path; source = $Switch.source; present = [bool]$Switch.present
                machine = 0x8664; install_path = $Switch.install_path; values = $script:OpenGlValueNamesByArch['x64'] }
        } else {
            $rows += [pscustomobject]@{ arch = 'x86'; package_path = $Switch.package_path_wow; source = $Switch.source_wow; present = [bool]$Switch.present_wow
                machine = 0x14C; install_path = $Switch.install_path_wow; values = $script:OpenGlValueNamesByArch['x86'] }
        }
    }
    return $rows
}
# The values of the architectures this release does NOT register: what an install has to take away again, so that a
# computer that had a wider release of ours does not keep a value naming a file this package no longer carries.
function Get-OpenGlUnregisteredValueNames {
    param([Parameter(Mandatory)]$Switch)
    return @($script:OpenGlArchitecturesKnown | Where-Object { @($Switch.architectures) -notcontains $_ } |
        ForEach-Object { $script:OpenGlValueNamesByArch[$_] })
}
# What the payload of a release that registers the ICD has to look like: the file of every architecture of the list,
# and no file of an architecture outside it. Returns the problems found (none: the payload matches the list). Pure;
# the file probes are the ones Get-OpenGlReleaseSwitch already did, so this needs a switch read with a package root.
# The second rule is not symmetry for its own sake. The x86 ICD rides in payload\wow64, and the installer copies that
# tree whole, so a file of an architecture the release does not register is copied into the install root and deleted
# again by the narrowing step of the same run, while manifest.json carries a component row that says it is installed:
# the orphan plan of the next release then works from an install map that never matched the computer. One of the two
# has to change, and the package itself says which: add the architecture to the list, or drop that files row.
# A release that registers nothing (opengl_icd.register false, the bisect lever) is not held to either rule: it takes
# the registration and the files of an earlier install away, and its files rows stay as they are.
function Get-OpenGlPayloadProblems {
    param([Parameter(Mandatory)]$Switch)
    if (-not $Switch.register) { return @() }
    $problems = @()
    $missing = @(@(Get-OpenGlArchFiles -Switch $Switch) | Where-Object { -not $_.present } | ForEach-Object { $_.package_path })
    if ($missing.Count) {
        $problems += ('the OpenGL ICD is registered for ' + (@($Switch.architectures) -join ' and ') + ', but the package has no ' + ($missing -join ' and ') +
            ': add the files row(s) of the built ICD, drop that architecture from opengl_icd.architectures, or set opengl_icd.register false')
    }
    foreach ($a in @($script:OpenGlArchitecturesKnown | Where-Object { @($Switch.architectures) -notcontains $_ })) {
        $path = $(if ($a -eq 'x64') { [string]$Switch.package_path } else { [string]$Switch.package_path_wow })
        $there = $(if ($a -eq 'x64') { [bool]$Switch.present } else { [bool]$Switch.present_wow })
        if ($there) {
            $problems += ("the package carries $path, and opengl_icd.architectures registers " + (@($Switch.architectures) -join ' and ') +
                " only: add '$a' to that list, or drop the files row of that file")
        }
    }
    return $problems
}

# What a run has to do about the ICD. Pure: the switch and what is on the computer in, the action out.
#   register   this release registers it: the files and the three values of every architecture of its list go on
#   rollback   this release does not register it, and an earlier install left values or files: they go
#   none       this release does not register it and there is nothing to take away
function Get-OpenGlAction {
    param([Parameter(Mandatory)]$Switch, [AllowEmptyCollection()][string[]]$PresentValues = @(), [bool]$DirPresent = $false)
    if ($Switch.register) { return 'register' }
    if (@($PresentValues).Count -or $DirPresent) { return 'rollback' }
    return 'none'
}

# The values this release writes, with their types: three per architecture of the switch's list, and nothing for an
# architecture that is not in it. Pure.
function Get-OpenGlRegistrationPlan {
    param([Parameter(Mandatory)]$Switch, [Parameter(Mandatory)][string]$InstallRoot)
    $t = 'MultiString'
    if ($Switch.value_type -eq 'REG_SZ') { $t = 'String' }
    $rows = @()
    foreach ($f in @(Get-OpenGlArchFiles -Switch $Switch)) {
        $p = Join-Path $InstallRoot $f.install_path
        if ($t -eq 'MultiString') { $p = [string[]]@($p) }
        $rows += [pscustomobject]@{ name = $f.values[0]; type = $t; value = $p }
        $rows += [pscustomobject]@{ name = $f.values[1]; type = 'DWord'; value = [int]$Switch.version }
        $rows += [pscustomobject]@{ name = $f.values[2]; type = 'DWord'; value = [int]$Switch.flags }
    }
    return $rows
}
# One line for Invoke-Change and the dry run.
function Format-OpenGlRegistrationPlan {
    param([AllowEmptyCollection()]$Plan)
    return (@($Plan | ForEach-Object { "$($_.name) = $(Format-RegistryValue $_.value)" }) -join '; ')
}
function Write-OpenGlRegistration {
    param([Parameter(Mandatory)][string]$ClassKey, [Parameter(Mandatory)]$Plan)
    foreach ($i in @($Plan)) {
        switch ($i.type) {
            'DWord' { New-ItemProperty -LiteralPath $ClassKey -Name $i.name -Value ([int]$i.value) -PropertyType DWord -Force | Out-Null }
            'MultiString' { New-ItemProperty -LiteralPath $ClassKey -Name $i.name -Value ([string[]]$i.value) -PropertyType MultiString -Force | Out-Null }
            default { New-ItemProperty -LiteralPath $ClassKey -Name $i.name -Value ([string]$i.value) -PropertyType String -Force | Out-Null }
        }
    }
}
# One ICD file: an image of the expected machine that exports every Drv* entry point. Returns the problems found
# (none: usable). 0x8664 is x64, 0x14C is x86 (Get-PeMachine above).
function Test-OpenGlIcdFile {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][int]$Machine)
    $problems = @()
    $m = Get-PeMachine $Path
    if ($null -eq $m) { return @("$Path missing or not an image") }
    if ($m -ne $Machine) { $problems += ('{0} is machine 0x{1:X}, expected 0x{2:X}' -f $Path, $m, $Machine) }
    $x = @()
    try { $x = @(Get-PeExportNames $Path) } catch { return ($problems + @("$Path exports cannot be read: $($_.Exception.Message)")) }
    $missing = @($script:OpenGlDrvExports | Where-Object { $x -notcontains $_ })
    if ($missing.Count) {
        $hint = ''
        if (@($x | Where-Object { $_ -match '^Drv.+@\d+$' }).Count) { $hint = ' (the file exports decorated Drv names, so its module definition file was not generated for Visual C++)' }
        $problems += "$Path does not export $($missing -join ', ')$hint"
    }
    return $problems
}
# The registration as verify checks it: the values of the architectures this release registers, and the ICD file of
# each of them. Returns the problems found (none: registered).
# The values of an architecture this release does not register are not checked here and are not a finding: a value of
# that name on the computer belongs to whoever wrote it (another vendor's registration on the same key), and our own
# is taken away by the install step, not by verify.
function Test-OpenGlRegistration {
    param([Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$ClassKey, [Parameter(Mandatory)]$Switch)
    $problems = @()
    $plan = Get-OpenGlRegistrationPlan -Switch $Switch -InstallRoot $InstallRoot
    $c = Read-RegistryValues $ClassKey
    foreach ($i in $plan) {
        if (-not $c.ContainsKey($i.name)) { $problems += "$($i.name) is missing"; continue }
        if (-not (Test-RegistryValueSame $c[$i.name] $i.value)) { $problems += "$($i.name) is $(Format-RegistryValue $c[$i.name]), expected $(Format-RegistryValue $i.value)" }
    }
    foreach ($f in @(Get-OpenGlArchFiles -Switch $Switch)) {
        $problems += @(Test-OpenGlIcdFile -Path (Join-Path $InstallRoot $f.install_path) -Machine $f.machine)
    }
    return $problems
}
# Whether each half of the registration on a key is ours: the name value of the half names the install root, so a
# release of this family wrote it. The only test available without the installer's own record, and the one both the
# record and the restore plan use. Pure.
function Get-OpenGlHalfIsOurs {
    param([hashtable]$Current = @{}, [string]$InstallRoot)
    $o = @{}
    foreach ($nm in @('OpenGLDriverName', 'OpenGLDriverNameWow')) {
        $o[$nm] = $false
        if ($InstallRoot -and $Current.ContainsKey($nm)) {
            $o[$nm] = [bool](@($Current[$nm] | ForEach-Object { [string]$_ }) -match [regex]::Escape($InstallRoot))
        }
    }
    return $o
}
# Which of the six values a key carries now, with the value itself and its registry type: the record install.ps1
# keeps once (the values from before the first install of ours) and the list uninstall.ps1 works from.
# The type is recorded because a value cannot be put back without it. The .NET type of the value does not carry it:
# a REG_EXPAND_SZ path and a REG_SZ path are both a String, a REG_BINARY value and a REG_MULTI_SZ list are both an
# array, and a REG_QWORD and a REG_DWORD are both a number. Another vendor's registration, or a repair tool's, may
# use any of them, and a restore that changes the type corrupts the value it claims to have put back: an expandable
# path stops being expanded, and four bytes come back as four strings.
#
# -InstallRoot names this install's own root, and with it the record tells a value of ours from a value of somebody
# else's: a name value that names the install root was written by a release of this family, so it is NOT "the value
# from before the install", and neither are the two numbers of its own half. Without that test a record taken on a
# computer that already carries our own registration - a lost or reset state file, a repair after one - would make the
# uninstaller and the narrowing step of an install "restore" our own value, and that value then names a file the same
# run deletes: a program of that architecture would get no OpenGL driver at all, not even the software renderer of
# Windows, which is exactly the harm the two steps exist to prevent (BD-076 review). Without -InstallRoot every value
# of the key is recorded as it stands, which is what a caller that only wants to read the key gets.
function Get-OpenGlValueRecord {
    param([Parameter(Mandatory)][string]$ClassKey, [string]$InstallRoot)
    $c = Read-RegistryValues $ClassKey
    $k = Get-Item -LiteralPath $ClassKey -ErrorAction SilentlyContinue
    $ours = Get-OpenGlHalfIsOurs -Current $c -InstallRoot $InstallRoot
    return @($script:OpenGlValueNames | ForEach-Object {
        $mine = [bool]$ours[$script:OpenGlValueHalf[$_]]
        $has = $c.ContainsKey($_) -and (-not $mine)
        $kind = $null
        if ($has -and $k) { try { $kind = [string]$k.GetValueKind($_) } catch { $kind = $null } }
        [pscustomobject]@{ name = $_; present = $has; previous = $(if ($has) { $c[$_] } else { $null }); kind = $kind; ours = $mine }
    })
}
# The registry type a restore writes: the recorded one when the record carries it, otherwise the type the value's own
# .NET type suggests, which is what a state file of an earlier release holds. Pure.
function Get-OpenGlRestoreKind {
    param($Value, [string]$Kind)
    $known = @('String', 'ExpandString', 'MultiString', 'Binary', 'DWord', 'QWord')
    if ($Kind -and ($known -contains $Kind)) { return $Kind }
    if ($Value -is [array]) { return 'MultiString' }
    if ($Value -is [string]) { return 'String' }
    return 'DWord'
}
# What the uninstaller does with each of the six values. Pure: the record from before the first install, what this
# release wrote, and what the key holds now in; one row per value out.
#   restore   this release wrote the value and there was one before the install: the old value goes back
#   remove    this release wrote the value and there was none before the install
#   keep      the value is not the one this release wrote: it belongs to whoever wrote it (the same rule TdrDelay
#             and the HD Audio interrupt follow)
#   none      there is nothing there and nothing was recorded
# Without a record of this release's own writes (an install by an older release, or a lost state file) the two name
# values are judged by the install root they name, and the four numbers are judged with the name value of their own
# half: OpenGLVersion and OpenGLFlags belong to OpenGLDriverName, and the Wow three to OpenGLDriverNameWow. A number
# alone says nothing (every registration writes 1 and 1), so without the name value of its half it is kept. This is
# what makes an uninstall without a state record symmetrical with the install: all six values of our own registration
# go, not the two name values alone, which is also what docs\testing\INSTALL.md tells the tester.
# -Names narrows the plan to some of the six. The uninstaller leaves it out and gets all six. An install uses it for
# the values of an architecture this release does not register (Get-OpenGlUnregisteredValueNames), so a computer that
# had a wider release of ours does not keep a value naming a file this package no longer carries.
function Get-OpenGlRestorePlan {
    param([AllowEmptyCollection()]$Previous = @(), [AllowEmptyCollection()]$Wrote = @(), [hashtable]$Current = @{}, [string]$InstallRoot,
        [AllowEmptyCollection()][string[]]$Names = $script:OpenGlValueNames)
    $prev = @{}
    foreach ($p in @($Previous)) { if ($p -and $p.name) { $prev[[string]$p.name] = $p } }
    $wroteMap = @{}
    foreach ($w in @($Wrote)) { if ($w -and $w.name) { $wroteMap[[string]$w.name] = $w.value } }
    # The name value of each half, and whether it names the install root. Used only where there is no record.
    $halfName = $script:OpenGlValueHalf
    $halfIsOurs = Get-OpenGlHalfIsOurs -Current $Current -InstallRoot $InstallRoot
    $rows = @()
    foreach ($n in @($script:OpenGlValueNames | Where-Object { @($Names) -contains $_ })) {
        $has = $Current.ContainsKey($n)
        $now = $(if ($has) { $Current[$n] } else { $null })
        $ours = $false
        $why = ''
        if ($wroteMap.ContainsKey($n)) {
            $ours = Test-RegistryValueSame $now $wroteMap[$n]
            if (-not $ours) { $why = 'not the value this release wrote' }
        } elseif ($has -and $InstallRoot) {
            $ours = [bool]$halfIsOurs[$halfName[$n]]
            if (-not $ours) {
                $why = 'no record of this release writing it'
                if ($n -ne $halfName[$n]) { $why = "no record of this release writing it, and $($halfName[$n]) does not name the install root" }
            }
        } elseif ($has) {
            $why = 'no record of this release writing it'
        }
        # The value is gone already (another driver install, or a tester): there is nothing to take away.
        if (-not $has) { $rows += [pscustomobject]@{ name = $n; action = 'none'; value = $null; kind = $null; why = 'absent' }; continue }
        if (-not $ours) { $rows += [pscustomobject]@{ name = $n; action = 'keep'; value = $now; kind = $null; why = $why }; continue }
        if ($prev.ContainsKey($n) -and $prev[$n].present) {
            $k = $null
            if ($prev[$n].PSObject.Properties['kind']) { $k = [string]$prev[$n].kind }
            $rows += [pscustomobject]@{ name = $n; action = 'restore'; value = $prev[$n].previous; kind = (Get-OpenGlRestoreKind -Value $prev[$n].previous -Kind $k); why = 'the value from before the install' }
            continue
        }
        $rows += [pscustomobject]@{ name = $n; action = 'remove'; value = $null; kind = $null; why = 'there was none before the install' }
    }
    return $rows
}
# One line for Invoke-Change, the dry run and the evidence of a lab arm. The row is held in a variable of its own:
# inside a switch statement $_ is the switch's own input, so $_.name would be empty and the line would name no value.
function Format-OpenGlRestorePlan {
    param([AllowEmptyCollection()]$Plan)
    $rows = @($Plan | Where-Object { $_.action -ne 'none' })
    if (-not $rows.Count) { return 'nothing of ours' }
    return (@($rows | ForEach-Object {
        $r = $_
        switch ($r.action) {
            'restore' { "$($r.name) back to $(Format-RegistryValue $r.value)$(if ($r.PSObject.Properties['kind'] -and $r.kind) { " ($($r.kind))" })" }
            'remove' { "remove $($r.name)" }
            default { "$($r.name) kept ($($r.why))" }
        }
    }) -join '; ')
}
# Every display class key that carries one of the six values: the adapter's own key when the device is there, and
# every other display class key, because the registration must be taken away even when the device is not.
# The uninstaller needs this: a registration left behind names a DLL the uninstaller has just deleted, and then every
# OpenGL application of the computer gets neither our driver nor the software renderer that no registration gives it.
# The device can be missing or doubled (the GPU disabled in Device Manager, a ghost entry, a WMI hiccup), so the keys
# are found by what they hold, not by the device. Which values of a key are ours is still decided by
# Get-OpenGlRestorePlan, from the installer's own record and the install root the name value carries, so another
# vendor's registration on another adapter's key is seen and kept.
function Get-OpenGlRegisteredClassKeys {
    param([string]$PreferredKey)
    $keys = @()
    if ($PreferredKey) { $keys += $PreferredKey }
    $root = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$($script:DisplayClassGuid)"
    foreach ($k in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
        $p = 'HKLM:\' + ($k.Name -replace '^HKEY_LOCAL_MACHINE\\', '')
        if ($keys -notcontains $p) { $keys += $p }
    }
    return @($keys | Where-Object {
        $c = Read-RegistryValues $_
        @($script:OpenGlValueNames | Where-Object { $c.ContainsKey($_) }).Count -gt 0
    })
}
# Puts each value back with the registry type it had, which Get-OpenGlValueRecord recorded. Without a recorded type
# (a state file of an earlier release) the type follows the value, as it did before.
function Write-OpenGlRestore {
    param([Parameter(Mandatory)][string]$ClassKey, [AllowEmptyCollection()]$Plan)
    foreach ($r in @($Plan)) {
        if ($r.action -eq 'restore') {
            $kind = $null
            if ($r.PSObject.Properties['kind']) { $kind = [string]$r.kind }
            $type = Get-OpenGlRestoreKind -Value $r.value -Kind $kind
            switch ($type) {
                'MultiString' { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([string[]]@($r.value | ForEach-Object { [string]$_ })) -PropertyType MultiString -Force | Out-Null }
                'Binary' { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([byte[]]@($r.value | ForEach-Object { [byte]$_ })) -PropertyType Binary -Force | Out-Null }
                'DWord' { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([int]$r.value) -PropertyType DWord -Force | Out-Null }
                'QWord' { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([long]$r.value) -PropertyType QWord -Force | Out-Null }
                'ExpandString' { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([string]$r.value) -PropertyType ExpandString -Force | Out-Null }
                default { New-ItemProperty -LiteralPath $ClassKey -Name $r.name -Value ([string]$r.value) -PropertyType String -Force | Out-Null }
            }
        } elseif ($r.action -eq 'remove') {
            Remove-ItemProperty -LiteralPath $ClassKey -Name $r.name -ErrorAction SilentlyContinue
        }
    }
}

function Get-DeviceDriverKey {
    # The device's software (class) key, e.g. HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-...}\0001
    param([Parameter(Mandatory)][string]$InstanceId)
    $p = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver' -ErrorAction SilentlyContinue
    if (-not $p -or -not $p.Data) { return $null }
    return ('HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + [string]$p.Data)
}
function Get-DeviceServiceName {
    param([Parameter(Mandatory)][string]$InstanceId)
    $p = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue
    if ($p) { return [string]$p.Data }
    return $null
}
# A lab installation (the project's development unit) carries files under C:\BC250\m15, m14 or m10. The release
# installer does not merge into one. The folders found are named in the check's detail (the support file).
function Get-LabInstallPaths {
    @('C:\BC250\m15', 'C:\BC250\m14', 'C:\BC250\m10' | Where-Object { Test-Path -LiteralPath $_ })
}
# DISM returns package objects, including packages not bound to a device. Never parse translated pnputil labels.
# https://learn.microsoft.com/powershell/module/dism/get-windowsdriver
function Read-DriverStorePackages {
    # A test inventory is accepted only by a dry run. A real uninstall always reads Windows.
    if ($script:DryRunMode -and $env:AMDGPU_WDDM_TEST_DRIVER_STORE) {
        $recorded = Get-Content -LiteralPath $env:AMDGPU_WDDM_TEST_DRIVER_STORE -Raw -ErrorAction Stop | ConvertFrom-Json
        return $recorded
    }
    return @(Get-WindowsDriver -Online -ErrorAction Stop)
}
# One row per package of ours: published oem INF name and DriverVer version. An unreadable inventory throws.
function Get-OurDriverPackageList {
    return @(Get-OurDriverPackageRows -Packages @(Read-DriverStorePackages))
}
# Pure: validate the structured inventory before returning any removal candidate.
function Get-OurDriverPackageRows {
    param([AllowEmptyCollection()][object[]]$Packages)
    $r = @()
    $seen = @{}
    foreach ($p in $Packages) {
        if (-not $p -or -not $p.OriginalFileName) { throw 'Driver-store inventory has a package without an original INF name' }
        if (-not [string]::Equals([IO.Path]::GetFileName([string]$p.OriginalFileName), 'bc250kmd.inf', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $published = [string]$p.Driver
        if ($published -notmatch '\Aoem[0-9]+\.inf\z') { throw 'Driver-store inventory has an invalid published INF name for our driver' }
        if ($seen.ContainsKey($published)) { throw "Driver-store inventory repeats $published" }
        $seen[$published] = $true
        $version = [string]$p.Version
        if (-not ($version -as [version])) { throw "Driver-store inventory has no valid version for $published" }
        $r += [pscustomobject]@{ published = $published.ToLowerInvariant(); version = $version }
    }
    return $r
}
# The same, by published name only (uninstall.ps1, the preflight of the uninstaller).
function Get-OurDriverPackages {
    return @(@(Get-OurDriverPackageList) | ForEach-Object { $_.published })
}
# The driver-store package (oem<n>.inf) that a device uses now. $null when neither reading gives one.
function Get-DeviceDriverPackage {
    param([Parameter(Mandatory)][string]$InstanceId)
    try {
        $d = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction Stop).Data
        if ($d) { return [string]$d }
    } catch { }
    try {
        $d = @(Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID='$($InstanceId -replace '\\', '\\')'" -ErrorAction Stop) | Select-Object -First 1
        if ($d -and $d.InfName) { return [string]$d.InfName }
    } catch { }
    return $null
}
# Pure: which of our driver-store packages an install removes (BD-089). Every release adds one bc250kmd.inf package,
# Windows binds the newest and the older ones stay in the store. Kept: the package the GPU is bound to now, and the
# package of the previous repair set, which a rollback installs again. Removed: every other package of ours.
# Nothing at all is removed when the bound package is not known, when it is not one of ours, or when a package of ours
# carries no version this reading could parse: the rollback target cannot be told apart then, and a driver store that
# grows is the smaller problem. $PreviousVersion is the DriverVer of the previous repair set ($null when there is none,
# and then the newest of the other packages is the release before this one).
function Get-DriverStoreRemovePlan {
    param($Packages, [string]$BoundPublished, [string]$PreviousVersion)
    $all = @(@($Packages) | Where-Object { $_ -and $_.published })
    $names = @($all | ForEach-Object { $_.published })
    if (-not $BoundPublished) { return [pscustomobject]@{ keep = $names; remove = @(); previous = $null; why = 'the driver package that the GPU uses cannot be read' } }
    if ($names -notcontains $BoundPublished) { return [pscustomobject]@{ keep = $names; remove = @(); previous = $null; why = "the driver package of the GPU ($BoundPublished) is not one of ours" } }
    $others = @($all | Where-Object { $_.published -ne $BoundPublished })
    $unknown = @($others | Where-Object { -not $_.version -or -not ($_.version -as [version]) })
    if ($unknown.Count) { return [pscustomobject]@{ keep = $names; remove = @(); previous = $null; why = "$($unknown.Count) package(s) of ours carry no version this reading understands ($(@($unknown | ForEach-Object { $_.published }) -join ', '))" } }
    $rollback = @()
    if ($PreviousVersion) { $rollback = @($others | Where-Object { $_.version -eq $PreviousVersion }) }
    $why = $null
    if ($PreviousVersion -and -not $rollback.Count) { $why = "the previous repair set ($PreviousVersion) has no package in the store" }
    if (-not $rollback.Count -and $others.Count) {
        $rollback = @(@($others | Sort-Object { [version]$_.version } -Descending)[0])
        if (-not $PreviousVersion) { $why = 'no previous repair set is recorded: the newest of the other packages is kept for a rollback' }
    }
    $keep = @($BoundPublished) + @($rollback | ForEach-Object { $_.published })
    return [pscustomobject]@{ keep = @($keep | Select-Object -Unique); remove = @($others | Where-Object { $keep -notcontains $_.published } | ForEach-Object { $_.published }); previous = @($rollback | ForEach-Object { $_.published })[0]; why = $why }
}
# The DriverVer that a kept repair set installs: its own manifest.json. $null when the set or its manifest is gone.
function Get-RepairSetDriverVersion([string]$Version) {
    if (-not $Version) { return $null }
    $m = Join-Path (Get-ClosureDir $Version) 'manifest.json'
    if (-not (Test-Path -LiteralPath $m)) { return $null }
    try { return [string](Get-Content -LiteralPath $m -Raw | ConvertFrom-Json).kmd_version } catch { return $null }
}
# Our Vulkan ICD registrations in both views of the registry: the value names that name a folder of ours. The
# installer writes one name per view, the full path of radeon_icd.json under the install root; an older release of
# ours wrote the same name under its own install root. Nothing of another vendor carries our release name.
function Get-OurVulkanIcdValues {
    $r = @()
    foreach ($key in @($script:KhronosKey, $script:KhronosKeyWow)) {
        $item = Get-Item -LiteralPath $key -ErrorAction SilentlyContinue
        if (-not $item) { continue }
        foreach ($n in @($item.GetValueNames())) {
            if ($n -and ($n -like "*$($script:ReleaseName)*") -and ($n -like '*radeon_icd.json')) { $r += [pscustomobject]@{ key = $key; name = $n } }
        }
    }
    return $r
}
# Our scheduled tasks. The installer registers the two of Get-LogonTaskPlan below; the sweep takes any task of ours,
# so a task that an older release named differently goes with it.
function Get-OurScheduledTasks {
    try { return @(Get-ScheduledTask -ErrorAction Stop | Where-Object { $_.TaskName -like "$($script:ReleaseName)*" } | ForEach-Object { $_.TaskName }) } catch { return @() }
}
# BD-080: the logon tasks of a tester PC as data. install.ps1 registers exactly what this function returns, and
# tools\release\test-session-checks.ps1 holds the rules of Test-LogonTaskPlan against it, so the account the
# confirmation runs under is gated without a registration on the test PC.
#
# Why two tasks. The confirmation needs the KMD escape BC250_ESCAPE_RUN_START_HEALTH, and the kernel refuses its
# CONFIRM operation to a caller who is not an administrator (driver\kmd\start_health.c StartHealthRequest, with
# CallerIsAdmin of driver\kmd\display.c), so the confirmation runs with an administrator's token. Up to and
# including 0.7.216.100-tester.28 the one task carried the Administrators group (S-1-5-32-544) as its principal,
# which means it runs at the logon of an administrator and at no other logon. A standard user therefore confirmed
# nothing, the boot-loop guard counted every start (BC250_MAX_UNCONFIRMED_STARTS 2, driver\kmd\guard.c) and the
# third start of the machine refused to start the driver, which leaves Windows on Microsoft Basic Display with no
# way back for the user who has no administrator account. That is BD-080.
#
# The confirmation itself needs no interactive account: it reads the start health and writes the confirmation
# through an escape. It runs as Local System (S-1-5-18), whose token is an administrator's, with the trigger that
# fires at the logon of any user, and headless (-NoWindow), because the account has no desktop in that session.
# The window is the other half, and it is the half that needs the user's own session and no rights at all: a small
# top-most window whose text changes keeps DWM presenting, so the KMD's milestone (ready for 60 s with a
# presentation completion within 15 s, BC250_START_HEALTH_MIN_MS and BC250_START_HEALTH_FRESH_MS in
# driver\kmd\bc250kmd_escape.h) is reached on an idle desktop. It is registered for the Users group
# (S-1-5-32-545), which every interactive account belongs to, at the limited run level, and it touches neither
# the driver nor the registry. Without it the confirmation still keeps the machine out of Basic Display through
# the fallback of start-confirm.ps1, at the price of one start at the floor clock.
#
# The accounts are named by their SID here, never by a name: a name of a built-in account is translated on a German,
# Polish or Japanese Windows, a SID is not, so this plan and the installer carry no translated text (the project's
# rule against parsing translated OS text). The SID does not survive the registration: New-ScheduledTaskPrincipal
# translates it into the local name before the task is stored, so a Polish Windows keeps the Users group as
# 'Uzytkownicy' in the task itself. Whoever reads a registered task back (a lab check, a support report) therefore
# translates the SID on that computer and compares the result, and never compares the string 'S-1-5-32-545'.
# The logon type of a group principal is Windows' own ('Group'); the installer states it in this plan and does not
# pass it to New-ScheduledTaskPrincipal, which takes it with -UserId.
function Get-LogonTaskPlan {
    param([Parameter(Mandatory)][string]$ConfirmScript, [Parameter(Mandatory)][string]$WindowScript, [int]$Seconds = 120)
    $host51 = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File'
    return @(
        [ordered]@{
            role      = 'confirm'
            name      = $script:TaskName
            execute   = 'powershell.exe'
            arguments = "$host51 `"$ConfirmScript`" -NoWindow"
            principal = [ordered]@{ kind = 'user'; id = 'S-1-5-18'; logon_type = 'ServiceAccount'; run_level = 'Highest' }
            trigger   = [ordered]@{ kind = 'at-logon'; user = $null }
            minutes   = 3
            what      = "at the logon of any user, as Local System (S-1-5-18), headless: $ConfirmScript"
        },
        [ordered]@{
            role      = 'window'
            name      = $script:WindowTaskName
            execute   = 'powershell.exe'
            arguments = "$host51 `"$WindowScript`" -Seconds $Seconds"
            principal = [ordered]@{ kind = 'group'; id = 'S-1-5-32-545'; logon_type = 'Group'; run_level = 'Limited' }
            trigger   = [ordered]@{ kind = 'at-logon'; user = $null }
            minutes   = 3
            what      = "at the logon of any user, with that user's own rights (Users, S-1-5-32-545): $WindowScript"
        }
    )
}
# The rules of the plan above, as the list of what is wrong with a plan; an empty list is a plan that holds them.
# The host test runs it over the installer's plan and over a plan with the principal of the defect, which must fail.
function Test-LogonTaskPlan {
    param([Parameter(Mandatory)]$Plan)
    $bad = New-Object System.Collections.ArrayList
    $tasks = @($Plan)
    foreach ($t in $tasks) {
        $n = [string]$t.name
        if ($n -notlike "$($script:ReleaseName)*") { [void]$bad.Add("task '$n' is not named after the release: the uninstaller's sweep (Get-OurScheduledTasks) would leave it on the computer") }
        if ([string]$t.trigger.kind -ne 'at-logon') { [void]$bad.Add("task '$n' triggers on '$($t.trigger.kind)', not at logon") }
        if ($t.trigger.user) { [void]$bad.Add("task '$n' triggers at the logon of '$($t.trigger.user)' only; every logon has to run it") }
        if ([string]$t.principal.id -eq 'S-1-5-32-544') { [void]$bad.Add("task '$n' is registered for the Administrators group (S-1-5-32-544): it never runs at the logon of a standard user (BD-080)") }
        if ([int]$t.minutes -le 0) { [void]$bad.Add("task '$n' has no execution time limit") }
        if ([string]$t.execute -ne 'powershell.exe') { [void]$bad.Add("task '$n' runs '$($t.execute)', not powershell.exe") }
    }
    $confirm = @($tasks | Where-Object { $_.role -eq 'confirm' })
    if ($confirm.Count -ne 1) { [void]$bad.Add("the plan has $($confirm.Count) confirmation tasks, not one"); return @($bad) }
    $c = $confirm[0]
    if (([string]$c.principal.kind -ne 'user') -or ([string]$c.principal.id -ne 'S-1-5-18')) { [void]$bad.Add("the confirmation runs as $($c.principal.kind) $($c.principal.id), not as Local System (S-1-5-18)") }
    if ([string]$c.principal.logon_type -ne 'ServiceAccount') { [void]$bad.Add("the confirmation asks for logon type '$($c.principal.logon_type)', not ServiceAccount") }
    if ([string]$c.principal.run_level -ne 'Highest') { [void]$bad.Add("the confirmation asks for run level '$($c.principal.run_level)': the KMD refuses CONFIRM to a caller who is not an administrator") }
    if ([string]$c.arguments -notmatch '(?i)(^|\s)-NoWindow(\s|$)') { [void]$bad.Add('the confirmation does not run headless (-NoWindow): its account has no desktop in the logon session') }
    if ([string]$c.arguments -match '(?i)(^|\s)-Probe(\s|$)') { [void]$bad.Add('the confirmation runs read-only (-Probe): it would confirm no start') }
    if ([string]$c.arguments -match '(?i)(^|\s)-FallbackHoldSeconds(\s|$)') { [void]$bad.Add('the confirmation changes the hold of the boot-loop fallback on its command line; the bound belongs to start-confirm.ps1') }
    $window = @($tasks | Where-Object { $_.role -eq 'window' })
    if ($window.Count -ne 1) { [void]$bad.Add("the plan has $($window.Count) window tasks, not one"); return @($bad) }
    $w = $window[0]
    if (([string]$w.principal.kind -ne 'group') -or ([string]$w.principal.id -ne 'S-1-5-32-545')) { [void]$bad.Add("the window is registered for $($w.principal.kind) $($w.principal.id), not for the Users group (S-1-5-32-545) that every interactive account belongs to") }
    if ([string]$w.principal.run_level -ne 'Limited') { [void]$bad.Add("the window asks for run level '$($w.principal.run_level)': it needs no rights, and a standard user has none to give it") }
    if ([string]$w.name -eq [string]$c.name) { [void]$bad.Add('both tasks carry the same name') }
    return @($bad)
}
# Our test certificates in the two machine stores, by subject: the certificate of this release and of any older one.
function Get-OurCertificates {
    $r = @()
    foreach ($store in 'Root', 'TrustedPublisher') {
        try { $r += @(Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction Stop | Where-Object { $_.Subject -like "*$($script:ReleaseName)*" } | ForEach-Object { [pscustomobject]@{ store = $store; thumbprint = $_.Thumbprint; subject = $_.Subject } }) } catch { }
    }
    return $r
}
# What of this release is on the computer. uninstall.ps1 prints it as its last step, so that a tester (and the host
# tests) can read in one place whether anything of ours is left; the control application's support report reads the
# same items. Every probe reads only and never throws. One row per item:
#   item    a short name
#   present $true when something of ours is there
#   detail  what was found, or why nothing could be read
#   kept    $true when the uninstaller leaves it on purpose (then present is not a leftover)
#   known   $false when the probe could not read; this never means that an item is gone
function Read-ReleaseFootprintItem {
    param([string]$Item, [scriptblock]$Read, [bool]$Kept = $false)
    try {
        $x = & $Read
        return [pscustomobject]@{ item = $Item; present = [bool]$x.present; detail = [string]$x.detail; kept = $Kept; known = $true }
    } catch {
        return [pscustomobject]@{ item = $Item; present = $false; detail = "not read: $($_.Exception.Message)"; kept = $Kept; known = $false }
    }
}
function Get-ReleaseFootprint {
    param([string]$InstallRoot, $State, [string[]]$MftKeys = @())
    $rows = New-Object System.Collections.ArrayList
    $probe = {
        param([string]$Item, [scriptblock]$Read, [bool]$Kept = $false)
        [void]$rows.Add((Read-ReleaseFootprintItem -Item $Item -Read $Read -Kept $Kept))
    }
    & $probe 'install root' { $p = (Test-Path -LiteralPath $InstallRoot); @{ present = $p; detail = "$InstallRoot$(if ($p) { ' is there' } else { ' is gone' })" } }
    foreach ($d in 'System32', 'SysWOW64') {
        $dir = Join-Path $env:windir $d
        & $probe "$d stub" {
            $stub = Join-Path $dir 'bc250umd.dll'
            $old = @(Get-ChildItem -LiteralPath $dir -File -Filter 'bc250umd.dll.old-*' -ErrorAction SilentlyContinue).Count
            $p = (Test-Path -LiteralPath $stub) -or ($old -gt 0)
            @{ present = $p; detail = "$stub$(if (Test-Path -LiteralPath $stub) { ' is there' } else { ' is gone' })$(if ($old) { ", $old old copy/copies" } else { '' })" }
        }
    }
    & $probe 'driver store' { $pk = @(Get-OurDriverPackages); @{ present = ($pk.Count -gt 0); detail = $(if ($pk.Count) { "$($pk.Count) package(s) of bc250kmd.inf: $($pk -join ', ')" } else { 'no package of ours' }) } }
    & $probe 'driver service' { $k = (Split-Path $script:ParametersKey); $p = (Test-Path -LiteralPath $k); @{ present = $p; detail = "$k$(if ($p) { ' is there' } else { ' is gone' })" } }
    # The one value this release changes on a device that is not ours (BD-092): still as this release set it, or back.
    & $probe 'DP audio interrupt' {
        $recorded = @()
        if ($State -and $State.PSObject.Properties['hda_msi'] -and ($null -ne $State.hda_msi)) { $recorded = @($State.hda_msi) }
        if (-not $recorded.Count) { return @{ present = $false; detail = "no record of a change to $($script:AudioMsiValue) of the GPU's HD Audio function" } }
        $left = @(Get-GpuAudioMsiRestorePlan -Recorded $recorded -Readings (Read-GpuAudioMsi) -Wrote 1 | Where-Object { $_.write -or $_.drop })
        @{ present = ($left.Count -gt 0); detail = $(if ($left.Count) { Format-GpuAudioMsiPlan $left } else { "$($script:AudioMsiValue) of the GPU's HD Audio function is as it was before the install" }) }
    }
    & $probe 'policy keys' {
        $found = @(@($script:SoftwareKey, "HKLM:\SOFTWARE\WOW6432Node\$($script:ReleaseName)") | Where-Object { Test-Path -LiteralPath $_ })
        @{ present = ($found.Count -gt 0); detail = $(if ($found.Count) { $found -join ', ' } else { "$($script:SoftwareKey) is gone" }) }
    }
    & $probe 'Vulkan registration' { $v = @(Get-OurVulkanIcdValues); @{ present = ($v.Count -gt 0); detail = $(if ($v.Count) { @($v | ForEach-Object { "$($_.key) '$($_.name)'" }) -join ', ' } else { 'no entry of ours in either view' }) } }
    & $probe 'H.264 encoder keys' { @{ present = (@($MftKeys).Count -gt 0); detail = $(if (@($MftKeys).Count) { @($MftKeys) -join ', ' } else { 'no key of ours' }) } }
    & $probe 'scheduled task' { $t = @(Get-OurScheduledTasks); @{ present = ($t.Count -gt 0); detail = $(if ($t.Count) { $t -join ', ' } else { 'no task of ours' }) } }
    & $probe 'RunOnce entry' { $p = [bool](Get-ItemProperty -LiteralPath $script:RunOnceKey -Name $script:RunOnceName -ErrorAction SilentlyContinue); @{ present = $p; detail = "$($script:RunOnceKey) $($script:RunOnceName)$(if ($p) { ' is there' } else { ' is gone' })" } }
    # Both lists of names: the ones this release writes, and the ones of an older release (legacy rows), which count
    # only when they point at our control application, as the uninstall removes them only then.
    & $probe 'shortcuts' {
        $own = @(Get-OwnControlTargets -InstallRoots @($InstallRoot))
        $lnks = @(@(Get-ControlShortcutPaths) | Where-Object { (Test-Path -LiteralPath $_.path -PathType Leaf) -and (-not [bool]$_.legacy -or (Test-OwnShortcutTarget (Read-ShortcutTarget $_.path) $own)) })
        @{ present = ($lnks.Count -gt 0); detail = $(if ($lnks.Count) { @($lnks | ForEach-Object { "$($_.where): $(Split-Path $_.path -Leaf)" }) -join ', ' } else { 'no shortcut of ours in the Start menu or on the desktop' }) }
    }
    & $probe 'installer state' { $p = (Test-Path -LiteralPath $script:StateDir); @{ present = $p; detail = "$($script:StateDir)$(if ($p) { ' is there (state, kept repair sets, verify reports)' } else { ' is gone' })" } }
    & $probe 'per-user data' {
        $u = @(Get-OurUserDataDirs | Where-Object { @(Get-ChildItem -LiteralPath $_ -Force | Where-Object { $_.Name -notin $script:UserDataKept }).Count -gt 0 })
        @{ present = ($u.Count -gt 0); detail = $(if ($u.Count) { $u -join ', ' } else { 'no per-user release data other than retained tuning recovery journals' }) }
    }
    & $probe 'per-user tuning recovery' {
        $u = @(Get-OurUserDataDirs | ForEach-Object { Join-Path $_ 'system-tuning' } | Where-Object { Test-Path -LiteralPath $_ })
        @{ present = ($u.Count -gt 0); detail = $(if ($u.Count) { 'per-user tuning journals retained for their owners' } else { 'no per-user tuning journal' }) }
    } $true
    # The Nagi wallpaper picture stays: it can be the desktop background of its account, which the uninstall leaves
    # alone (owner, 2026-10-13).
    & $probe 'per-user wallpaper' {
        $u = @(Get-OurUserDataDirs | ForEach-Object { Join-Path $_ 'wallpaper' } | Where-Object { Test-Path -LiteralPath $_ })
        @{ present = ($u.Count -gt 0); detail = $(if ($u.Count) { 'the Nagi wallpaper picture stays for the accounts that may show it as their background' } else { 'no per-user wallpaper picture' }) }
    } $true
    & $probe 'certificates' { $c = @(Get-OurCertificates); @{ present = ($c.Count -gt 0); detail = $(if ($c.Count) { @($c | ForEach-Object { "$($_.store) $($_.thumbprint)" }) -join ', ' } else { 'no certificate of ours in Root or TrustedPublisher' }) } }
    # Kept on purpose: the firmware folder and C:\BC250 when they were there before the install, and the control
    # application's own files (the tester's setting backups and action log).
    $fwKept = [bool]($State -and $State.firmware_dir_existed)
    & $probe 'GPU firmware' { $p = (Test-Path -LiteralPath $script:FirmwareInstallDir); @{ present = $p; detail = "$($script:FirmwareInstallDir)$(if (-not $p) { ' is gone' } elseif ($fwKept) { ' is there, and it was there before the install' } else { ' is there' })" } } $fwKept
    & $probe 'control application data' { $d = Join-Path (Split-Path $script:StateDir) 'control'; $p = (Test-Path -LiteralPath $d); @{ present = $p; detail = "$d$(if ($p) { ' is there (the tester''s setting backups and action log)' } else { ' is gone' })" } } $true
    foreach ($name in 'system-tuning','system-tuning-recovery') {
        & $probe $name { $d = Join-Path (Split-Path $script:StateDir) $name; @{ present = (Test-Path -LiteralPath $d); detail = "$d is retained when present for optional Windows settings recovery" } } $true
    }
    return $rows.ToArray()
}

# BD-146: cancel only deletions of exact files this install will copy. Keep pairs intact, in order, including
# duplicate and foreign entries. MoveFileEx defines an empty target as deletion; a nonempty target is a rename.
# https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-movefileexw
# An uninstaller also schedules directories: Windows removes those only when empty, so they stay in the list.
function Get-PendingRemovalPath([string]$Path) {
    if ($Path.StartsWith('\??\') -or $Path.StartsWith('\\?\')) { $Path = $Path.Substring(4) }
    # Our destinations are full local paths. Do not resolve links, short names, variables or device paths.
    if ($Path -notmatch '^[A-Za-z]:\\' -or $Path -match '(^|\\)\.{1,2}(\\|$)') { return $null }
    return $Path
}
function Get-PendingRemovalPlan {
    param([AllowEmptyCollection()][string[]]$Operations = @(), [AllowEmptyCollection()][string[]]$Destinations = @())
    if ($Operations.Count % 2) { throw 'The pending file operations list has an incomplete pair. No entries were changed.' }
    $ours = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($d in $Destinations) {
        $p = Get-PendingRemovalPath $d
        if (-not $p) { throw "The install destination is not a full local file path: $d" }
        [void]$ours.Add($p)
    }
    $keep = New-Object 'Collections.Generic.List[string]'
    $cancel = New-Object 'Collections.Generic.List[string]'
    for ($i = 0; $i -lt $Operations.Count; $i += 2) {
        $source = $Operations[$i]; $target = $Operations[$i + 1]
        if ([string]::IsNullOrEmpty($source)) { throw 'The pending file operations list has an empty source. No entries were changed.' }
        $p = Get-PendingRemovalPath $source
        if ($target -eq '' -and $p -and $ours.Contains($p)) { $cancel.Add($source) }
        else { $keep.Add($source); $keep.Add($target) }
    }
    return [pscustomobject]@{ kept = $keep.ToArray(); cancelled = $cancel.ToArray() }
}
function Test-PendingFileOperationsEqual([string[]]$Left, [string[]]$Right) {
    if ($Left.Count -ne $Right.Count) { return $false }
    for ($i = 0; $i -lt $Left.Count; $i++) { if ($Left[$i] -cne $Right[$i]) { return $false } }
    return $true
}
# Registry transactions serialize this read/modify/write with other installers. A conflict aborts our write
# instead of discarding someone else's newly queued operation. No file I/O occurs inside the transaction.
# https://learn.microsoft.com/windows/win32/api/winreg/nf-winreg-regopenkeytransactedw
function Open-PendingFileTransaction {
    param([Microsoft.Win32.RegistryKey]$Root, [string]$SubKey)
    if (-not ('AmdgpuWddmInstaller.PendingFileTransaction' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
namespace AmdgpuWddmInstaller {
    public sealed class PendingFileTransaction : IDisposable {
        [DllImport("KtmW32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateTransaction(IntPtr attributes, IntPtr id, uint options, uint isolation,
            uint flags, uint timeout, string description);
        [DllImport("KtmW32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool CommitTransaction(IntPtr transaction);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegOpenKeyTransacted(SafeRegistryHandle root, string subkey, uint options, int access,
            out IntPtr key, IntPtr transaction, IntPtr reserved);
        IntPtr transaction;
        public RegistryKey Key { get; private set; }
        public PendingFileTransaction(RegistryKey root, string subkey) {
            transaction = CreateTransaction(IntPtr.Zero, IntPtr.Zero, 0, 0, 0, 5000, "amdgpu-wddm pending file removals");
            if (transaction == new IntPtr(-1)) { transaction = IntPtr.Zero; throw new Win32Exception(Marshal.GetLastWin32Error()); }
            try {
                IntPtr key;
                int error = RegOpenKeyTransacted(root.Handle, subkey, 0, 3, out key, transaction, IntPtr.Zero); // KEY_QUERY_VALUE | KEY_SET_VALUE
                if (error != 0) throw new Win32Exception(error);
                Key = RegistryKey.FromHandle(new SafeRegistryHandle(key, true));
            } catch { Dispose(); throw; }
        }
        public void Commit() {
            if (!CommitTransaction(transaction)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public void Dispose() {
            if (Key != null) { Key.Dispose(); Key = null; }
            if (transaction != IntPtr.Zero) { CloseHandle(transaction); transaction = IntPtr.Zero; }
        }
    }
}
'@
    }
    return (New-Object AmdgpuWddmInstaller.PendingFileTransaction -ArgumentList $Root, $SubKey)
}
# Production opens only Session Manager, inside Invoke-Change before any payload copy. Host tests name their
# own HKCU scratch key. Remove-PathOrSchedule keeps no separate pending-removal journal.
function Repair-PendingFileRemovals {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Destinations,
          [Microsoft.Win32.RegistryKey]$Root = [Microsoft.Win32.Registry]::LocalMachine,
          [string]$SubKey = 'SYSTEM\CurrentControlSet\Control\Session Manager')
    $transaction = Open-PendingFileTransaction -Root $Root -SubKey $SubKey
    try {
        $Key = $transaction.Key
        $name = 'PendingFileRenameOperations'
        $raw = $Key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($null -eq $raw) { return 0 }
        if ($Key.GetValueKind($name) -ne [Microsoft.Win32.RegistryValueKind]::MultiString -or $raw -isnot [string[]]) {
            throw 'The pending file operations value is not REG_MULTI_SZ. No entries were changed.'
        }
        $plan = Get-PendingRemovalPlan -Operations $raw -Destinations $Destinations
        if (-not $plan.cancelled.Count) { return 0 }
        # Keep an empty MULTI_SZ rather than delete the value: the same write path serves every plan.
        $Key.SetValue($name, [string[]]$plan.kept, [Microsoft.Win32.RegistryValueKind]::MultiString)
        $written = $Key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($null -eq $written -or $written -isnot [string[]] -or
            -not (Test-PendingFileOperationsEqual $plan.kept $written)) {
            throw 'The pending file operations could not be verified. No entries were changed.'
        }
        $transaction.Commit()
        foreach ($p in $plan.cancelled) { Write-Log "   cancelled pending removal: $p" }
        Write-Info "Kept $($plan.cancelled.Count) install file(s) from being deleted at the next restart."
        return $plan.cancelled.Count
    } finally { $transaction.Dispose() }
}
# Only real file sources in this run's selected trees are destinations. A disabled optional component, a stale
# file in the install root and a file in another product's folder are never admitted by a directory prefix.
function Get-InstallCopyDestinations {
    param([Parameter(Mandatory)][string]$Package, [Parameter(Mandatory)][string]$InstallRoot,
          [string[]]$PayloadDirs, [string[]]$FirmwareFiles = @(), [string]$FirmwareRoot = $script:FirmwareInstallDir)
    $trees = @(@{ source = (Join-Path $Package 'installer'); target = (Join-Path $InstallRoot 'installer') },
        @{ source = (Join-Path $Package 'licenses'); target = (Join-Path $InstallRoot 'licenses') })
    foreach ($d in $PayloadDirs) { $trees += @{ source = (Join-Path $Package "payload\$d"); target = (Join-Path $InstallRoot $d) } }
    foreach ($tree in $trees) {
        foreach ($f in Get-ChildItem -LiteralPath $tree.source -Recurse -File -ErrorAction Stop) {
            Join-Path $tree.target $f.FullName.Substring($tree.source.TrimEnd('\').Length + 1)
        }
    }
    foreach ($f in 'manifest.json', 'uninstall.cmd', 'verify.cmd') { Join-Path $InstallRoot $f }
    Join-Path $InstallRoot 'licenses\THIRD-PARTY.md'
    foreach ($f in $FirmwareFiles) { Join-Path $FirmwareRoot (Split-Path $f -Leaf) }
}

# MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT): delete a file that a running process (DWM) still has mapped.
function Remove-FileAtReboot([string]$Path) {
    if (-not ('AmdgpuWddmInstaller.Native' -as [type])) {
        Add-Type -Namespace AmdgpuWddmInstaller -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@
    }
    # [NullString]::Value, not $null: PowerShell passes $null to a string parameter as "", and MoveFileEx then
    # fails with ERROR_PATH_NOT_FOUND (3) instead of scheduling the delete (lab, 2026-10-06: six DLLs that DWM
    # held survived the restart after an uninstall).
    return [AmdgpuWddmInstaller.Native]::MoveFileEx($Path, [NullString]::Value, 4)
}

# Per-user data of the release: the D3D12 engine's disk shader cache, the recent-launch list and the DWM
# observations live in %LOCALAPPDATA%\amdgpu-wddm of every user who ran a program on the GPU. One folder per
# profile in the ProfileList. A host test sets $script:ProfilePathsOverride to fixture profile folders.
# Two folders in it stay at an uninstall (Remove-UserDataExceptTuning): the user's Windows tuning journal, and the
# Nagi wallpaper picture, which can be that account's desktop background (owner, 2026-10-13: the uninstall leaves
# the person's current wallpaper alone).
$script:UserDataKept = @('system-tuning', 'wallpaper')
function Get-OurUserDataDirs {
    $profiles = @()
    if ($null -ne $script:ProfilePathsOverride) { $profiles = @($script:ProfilePathsOverride) }
    else {
        foreach ($k in @(Get-ChildItem -LiteralPath $script:ProfileListKey -ErrorAction SilentlyContinue)) {
            $p = (Get-ItemProperty -LiteralPath $k.PSPath -Name ProfileImagePath -ErrorAction SilentlyContinue).ProfileImagePath
            if ($p) { $profiles += [Environment]::ExpandEnvironmentVariables($p) }
        }
    }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($p in $profiles) {
        $d = Join-Path $p 'AppData\Local\amdgpu-wddm'
        if ((Test-Path -LiteralPath $d -PathType Container) -and -not $out.Contains($d)) { $out.Add($d) }
    }
    return $out.ToArray()
}
# Installs one file so that a re-run with the same package always finishes:
#  - target with the same SHA256: nothing to do ("already current");
#  - target absent or writable: copied;
#  - target in use (a DLL that DWM or an application has loaded cannot be overwritten, but it can be renamed):
#    renamed to <name>.old-<utc>, the new file copied in, the old one scheduled for deletion at the next restart
#    (needs administrator rights; otherwise, and as a second chance, verify removes *.old-* files).
# Returns 'current', 'copied' or 'replaced-in-use'.
function Copy-FileSafe {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination, [bool]$ScheduleOld = $script:ScheduleOldCopies)
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        if ((Get-Sha256 $Destination) -eq (Get-Sha256 $Source)) { Write-Log "   already current: $Destination"; return 'current' }
        try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop; Write-Log "   copied: $Destination"; return 'copied' }
        catch {
            Write-Log "   cannot overwrite $Destination ($($_.Exception.Message)): replacing by rename"
            $old = $Destination + '.old-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
            [IO.File]::Move($Destination, $old)
            try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop }
            catch { [IO.File]::Move($old, $Destination); throw }
            $scheduled = $false
            if ($ScheduleOld) { try { $scheduled = Remove-FileAtReboot $old } catch { $scheduled = $false } }
            Write-Info "in use, replaced: $Destination (old copy $(Split-Path $old -Leaf) $(if ($scheduled) { 'is deleted at the next restart' } else { 'is deleted by verify' }))"
            return 'replaced-in-use'
        }
    }
    [void][IO.Directory]::CreateDirectory((Split-Path $Destination))
    Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
    Write-Log "   copied: $Destination"
    return 'copied'
}
# Every file of a directory tree, through Copy-FileSafe.
function Copy-TreeSafe {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    $n = @{ current = 0; copied = 0; 'replaced-in-use' = 0 }
    foreach ($f in Get-ChildItem -LiteralPath $Source -Recurse -File) {
        $rel = $f.FullName.Substring($Source.TrimEnd('\').Length + 1)
        $n[(Copy-FileSafe -Source $f.FullName -Destination (Join-Path $Destination $rel))]++
    }
    Write-Info ("{0}: {1} copied, {2} already current, {3} replaced in use" -f $Destination, $n.copied, $n.current, $n['replaced-in-use'])
    return $n
}
# Old copies left by Copy-FileSafe (<name>.old-<utc>). Verify and uninstall call it; a copy still in use stays for
# the deletion scheduled at restart. Returns the number removed.
function Remove-OldCopies([string]$Directory, [string]$Filter = '*.old-*', [switch]$Recurse) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return 0 }
    $n = 0
    foreach ($i in @(Get-ChildItem -LiteralPath $Directory -File -Filter $Filter -Recurse:$Recurse -ErrorAction SilentlyContinue)) {
        if ($i.Name -notmatch '\.old-\d{8}T\d{6,9}Z$') { continue }
        try { Remove-Item -LiteralPath $i.FullName -Force -ErrorAction Stop; $n++; Write-Log "   removed old copy $($i.FullName)" } catch { }
    }
    return $n
}

function Remove-PathOrSchedule([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $files = @()
    if (Test-Path -LiteralPath $Path -PathType Container) { $files = @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force | ForEach-Object FullName) } else { $files = @($Path) }
    $pending = 0
    foreach ($f in $files) {
        try { Remove-Item -LiteralPath $f -Force -ErrorAction Stop }
        catch { if (Remove-FileAtReboot $f) { $pending++ } else { Write-Warn2 "could not remove or schedule $f" } }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $dirs = @(Get-ChildItem -LiteralPath $Path -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object FullName) + @($Path)
        foreach ($d in $dirs) {
            try { Remove-Item -LiteralPath $d -Force -ErrorAction Stop } catch { [void](Remove-FileAtReboot $d) }
        }
    }
    if ($pending) { Write-Info "$pending file(s) in use: removal scheduled for the next restart" }
}

# ---------------------------------------------------------------------------------------------------------------
# What an older release left behind (BD-089). <InstallRoot>\manifest.json is the manifest of the release installed
# there, because the installer copies it with the payload. The files of that release are therefore known by name and
# by SHA256, and an install over it can take away what it installed and this package does not install any more.
# Only a file that the previous manifest names is ever removed: the install root also holds files that are not ours
# (a tester's own copy, a lab kit), and a sweep by wildcard would take them too.

# Pure: where one file of a package lands on the computer, or $null when the installer copies it nowhere under the
# install root. The driver package goes into the driver store, the certificate into two certificate stores, the GPU
# firmware into its own folder, Microsoft's runtime installer (payload\redist) is run from the package, and
# install.cmd, the documents and the setup window's own folder are read from the package and never copied.
function Get-InstalledPathOfPackageFile {
    param([Parameter(Mandatory)][string]$PackagePath, [Parameter(Mandatory)][string]$InstallRoot,
        [string]$SystemDir = (Join-Path $env:windir 'System32'), [string]$SysWowDir = (Join-Path $env:windir 'SysWOW64'))
    $p = ([string]$PackagePath) -replace '/', '\'
    $p = $p.TrimStart('\')
    if (-not $p) { return $null }
    if ($p -match '\.\.') { return $null }
    if ($p -match '^payload\\system32\\(.+)$') { return (Join-Path $SystemDir $Matches[1]) }
    if ($p -match '^payload\\syswow64\\(.+)$') { return (Join-Path $SysWowDir $Matches[1]) }
    # payload\redist carries Microsoft's installer for the Visual C++ runtime: the install runs it from the package
    # and copies no file of it, so nothing of this folder has an installed path either (BD-089 orphan plan).
    if ($p -match '^payload\\(kmd|cert|firmware|redist)\\') { return $null }
    if ($p -match '^payload\\(.+\\.+)$') { return (Join-Path $InstallRoot $Matches[1]) }
    if ($p -match '^(installer|licenses)\\(.+)$') { return (Join-Path $InstallRoot $p) }
    if ($p -ieq 'THIRD-PARTY.md') { return (Join-Path $InstallRoot 'licenses\THIRD-PARTY.md') }
    if ($p -ieq 'uninstall.cmd' -or $p -ieq 'verify.cmd' -or $p -ieq 'manifest.json') { return (Join-Path $InstallRoot $p) }
    return $null
}
# Pure: every file of a manifest that the installer copies, as a map from the lower-case installed path to the row
# (path as it is written, the package path, the SHA256 the manifest records).
function Get-ManifestInstallMap {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)][string]$InstallRoot,
        [string]$SystemDir = (Join-Path $env:windir 'System32'), [string]$SysWowDir = (Join-Path $env:windir 'SysWOW64'))
    $map = New-Object Collections.Specialized.OrderedDictionary
    foreach ($f in @($Manifest.files)) {
        if (-not $f -or -not $f.path) { continue }
        $dest = Get-InstalledPathOfPackageFile -PackagePath ([string]$f.path) -InstallRoot $InstallRoot -SystemDir $SystemDir -SysWowDir $SysWowDir
        if (-not $dest) { continue }
        $key = $dest.ToLowerInvariant()
        if ($map.Contains($key)) { continue }
        $map.Add($key, [pscustomobject]@{ path = $dest; package_path = ([string]$f.path); sha256 = ([string]$f.sha256).ToUpperInvariant() })
    }
    return $map
}
# Pure: the files that the previous release installed and this package does not install. One row per file: the
# installed path, the SHA256 that the previous manifest recorded for it, and the version that installed it. A row is
# returned only for a path under the install root, System32 or SysWOW64, so that a manifest cannot name a file
# anywhere else on the computer.
function Get-OrphanFilePlan {
    param([Parameter(Mandatory)]$PreviousManifest, [Parameter(Mandatory)]$NewManifest, [Parameter(Mandatory)][string]$InstallRoot,
        [string]$SystemDir = (Join-Path $env:windir 'System32'), [string]$SysWowDir = (Join-Path $env:windir 'SysWOW64'))
    $from = [string]$PreviousManifest.version
    $old = Get-ManifestInstallMap -Manifest $PreviousManifest -InstallRoot $InstallRoot -SystemDir $SystemDir -SysWowDir $SysWowDir
    $new = Get-ManifestInstallMap -Manifest $NewManifest -InstallRoot $InstallRoot -SystemDir $SystemDir -SysWowDir $SysWowDir
    $roots = @($InstallRoot, $SystemDir, $SysWowDir) | Where-Object { $_ } | ForEach-Object { ([IO.Path]::GetFullPath($_).TrimEnd('\') + '\').ToLowerInvariant() }
    $rows = @()
    foreach ($key in @($old.Keys)) {
        if ($new.Contains($key)) { continue }
        $row = $old[$key]
        $full = ([IO.Path]::GetFullPath($row.path)).ToLowerInvariant()
        if (-not @($roots | Where-Object { $full.StartsWith($_) }).Count) { continue }
        $rows += [pscustomobject]@{ path = $row.path; package_path = $row.package_path; sha256 = $row.sha256; from_version = $from }
    }
    return $rows
}
# The rows of Get-OrphanFilePlan against the computer. state: 'remove' when the file is there with the SHA256 that the
# previous manifest recorded, 'changed' when its bytes are other ones (somebody replaced it after that install: it is
# kept and reported), 'absent' when it is not there any more. Reads only.
function Resolve-OrphanFilePlan {
    param($Plan)
    $rows = @()
    foreach ($row in @($Plan)) {
        $state = 'absent'
        $have = $null
        if (Test-Path -LiteralPath $row.path -PathType Leaf) {
            try { $have = Get-Sha256 $row.path } catch { $have = $null }
            if ($have -and $row.sha256 -and $have -eq $row.sha256) { $state = 'remove' } else { $state = 'changed' }
        }
        $rows += [pscustomobject]@{ path = $row.path; package_path = $row.package_path; sha256 = $row.sha256; from_version = $row.from_version; state = $state; sha256_now = $have }
    }
    return $rows
}

# ---------------------------------------------------------------------------------------------------------------
# A normal restart of Windows (ExitWindowsEx EWX_REBOOT with a planned reason, never EWX_FORCE): programs are asked to
# close and can keep unsaved work. The command-line installer calls it after the tester typed Y; a run that the setup
# window drives never restarts (the window asks the user and calls the same API). Returns $null when Windows accepted
# the request, otherwise the reason.
function Request-PlannedRestart {
    if (-not ('AmdgpuWddmInstaller.Restart' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AmdgpuWddmInstaller {
public static class Restart {
    [StructLayout(LayoutKind.Sequential, Pack = 4)] struct TokenPrivilege { public uint Count; public long Luid; public uint Attributes; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivilege state, uint length, IntPtr previous, IntPtr returnLength);
    [DllImport("user32.dll", SetLastError = true)] static extern bool ExitWindowsEx(uint flags, uint reason);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    // EWX_REBOOT without EWX_FORCE; SHTDN_REASON_FLAG_PLANNED | MAJOR_APPLICATION | MINOR_RECONFIG.
    public static string Request() {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) return "OpenProcessToken error " + Marshal.GetLastWin32Error();
        try {
            var p = new TokenPrivilege { Count = 1, Attributes = 2 };
            if (!LookupPrivilegeValue(null, "SeShutdownPrivilege", out p.Luid)) return "LookupPrivilegeValue error " + Marshal.GetLastWin32Error();
            if (!AdjustTokenPrivileges(token, false, ref p, 0, IntPtr.Zero, IntPtr.Zero)) return "AdjustTokenPrivileges error " + Marshal.GetLastWin32Error();
            int e = Marshal.GetLastWin32Error();
            if (e != 0) return "this account may not restart Windows (error " + e + ")";
        } finally { CloseHandle(token); }
        return ExitWindowsEx(0x2, 0x80000000u | 0x00040000u | 0x00000004u) ? null : "ExitWindowsEx error " + Marshal.GetLastWin32Error();
    }
}
}
'@
    }
    return [AmdgpuWddmInstaller.Restart]::Request()
}

# A command line for RunOnce (GUI plan A1): the program in quotes on its own, then each argument, quoted when it holds
# a space or is empty. Nothing is joined into one quoted string.
function Format-CommandLine([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @()) {
    if ($Exe -match '"') { throw "a RunOnce program path may not contain a quote: $Exe" }
    $parts = @('"' + $Exe + '"')
    foreach ($a in @($Arguments)) {
        if ($a -match '"') { throw "a RunOnce argument may not contain a quote: $a" }
        # Anything but plain letters, digits and path punctuation is quoted (spaces, &, ^, %, (, ), ...); inside quotes
        # a trailing backslash would escape the closing quote, so trailing backslashes are doubled.
        if ($a -eq '' -or $a -match '[^A-Za-z0-9_.:\\/=,+-]') { $parts += ('"' + ($a -replace '(\\+)$', '$1$1') + '"') } else { $parts += $a }
    }
    return ($parts -join ' ')
}

# ---------------------------------------------------------------------------------------------------------------
# The continuation closure and the kept repair set (GUI plan A1, WU-051, WU-058). Before the first restart that the
# installer asks for, the whole package (every file manifest.json lists, and manifest.json) is copied into
# %ProgramData%\amdgpu-wddm\installer\packages\<version> and each SHA256 is checked there; with -FirmwareDir the 9
# firmware files go into its firmware\ folder, also checked. The run after the restart starts from there, so the
# downloaded folder, a USB stick or a temporary extraction may be gone by then. The state folder is writable by
# administrators only (Set-StateDirAccess). When phase 2 completes, the closure of the installed package becomes the
# active repair set (firmware included, also after a download) and the one before it is kept as the previous set;
# older sets are removed. A prepared offline folder (prepare-offline.ps1) has the same layout. Neither is the
# unpacked release: both keep the package root as their own folder, with setup.exe in it (Get-PackageSetupExe).
#
# Where the setup window is (owner, 2026-10-11): the unpacked release holds setup.exe in its top folder and
# everything else in files\, so the package root (the folder with manifest.json) is files\ and the exe is one level
# above it. A kept repair set and a prepared folder are not unpacked by a tester: they keep the package root as
# their own folder, with the setup copy in it. Get-PackageSetupExe answers both shapes, so nothing else has to know
# which one it holds.
#
# The package root's own copy is answered first, and the folder above it is read only when the root is the files\
# folder of an unpacked release. A closure, a repair set and a prepared folder hold their own copy, and their parent
# folder is chosen by whoever made them (prepare-offline.ps1 takes any -Destination): a setup.exe of another build
# lying there must neither hide the package's own copy (Test-PackageManifest would report a good folder as changed)
# nor stand in for a tampered one (the integrity check would pass the file that RunOnce and Release\RepairSetup run).
$script:SetupExeName = 'setup.exe'
$script:PackageFilesDir = 'files'
function Get-PackageSetupExe([string]$PackageRoot) {
    if (-not $PackageRoot) { return $null }
    $own = Join-Path $PackageRoot $script:SetupExeName
    if (Test-Path -LiteralPath $own -PathType Leaf) { return ([IO.Path]::GetFullPath($own)) }
    if ((Split-Path $PackageRoot -Leaf) -ieq $script:PackageFilesDir) {
        $up = Join-Path $PackageRoot (Join-Path '..' $script:SetupExeName)
        if (Test-Path -LiteralPath $up -PathType Leaf) { return ([IO.Path]::GetFullPath($up)) }
    }
    return $null
}
$script:RepairIndexSchema = 'amdgpu-wddm.repair-sets/1'
function Get-PackagesDir { return (Join-Path $script:StateDir 'packages') }
function Get-ClosureDir([string]$Version) { return (Join-Path (Get-PackagesDir) $Version) }
function Test-SamePath([string]$A, [string]$B) {
    if (-not $A -or -not $B) { return $false }
    return ([IO.Path]::GetFullPath($A).TrimEnd('\') -ieq [IO.Path]::GetFullPath($B).TrimEnd('\'))
}
# <package>\firmware of a prepared folder or a repair set, $null when there is none. Its contents are the preflight's
# business: a folder with a missing or changed file is refused before any change.
function Get-PackageFirmwareDir([string]$PackageRoot) {
    $d = Join-Path $PackageRoot 'firmware'
    if (Test-Path -LiteralPath $d -PathType Container) { return $d }
    return $null
}
# The problems of a firmware folder against the manifest's pin: one text per missing or changed file.
function Test-FirmwareFolder($Firmware, [string]$Dir) {
    $bad = @()
    foreach ($f in @($Firmware.files)) {
        $p = Join-Path $Dir $f.name
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "$($f.name) missing" }
        elseif ((Get-Sha256 $p) -ne ([string]$f.sha256).ToUpperInvariant()) { $bad += "$($f.name) has another SHA256" }
    }
    return , $bad
}
# Copies the firmware files into <closure>\firmware (from a folder, or from a list of checked files) and checks each
# SHA256 there. Throws on a missing or changed file.
function Save-ClosureFirmware {
    param([Parameter(Mandatory)]$Firmware, [Parameter(Mandatory)][string]$Closure, [string]$FromDir, [string[]]$FromFiles = @())
    $dst = Join-Path $Closure 'firmware'
    if (-not ($FromDir -and (Test-SamePath $FromDir $dst))) {
        foreach ($f in @($Firmware.files)) {
            $src = $null
            if ($FromDir) { $src = Join-Path $FromDir $f.name }
            else { $src = @($FromFiles | Where-Object { (Split-Path $_ -Leaf) -ieq $f.name }) | Select-Object -First 1 }
            if (-not $src -or -not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "firmware: $($f.name) is not available for $dst" }
            [void](Copy-FileSafe -Source $src -Destination (Join-Path $dst $f.name))
        }
    }
    $bad = Test-FirmwareFolder $Firmware $dst
    if ($bad.Count) { throw "firmware in ${dst}: $($bad -join '; ')" }
    return $dst
}
# Copies the package into the closure (or only checks it when this run starts from there) and returns the closure.
function Save-ContinuationClosure {
    param([Parameter(Mandatory)][string]$PackageRoot, [Parameter(Mandatory)]$Manifest, [string]$FirmwareDir, [string]$Destination)
    if (-not $Destination) { $Destination = Get-ClosureDir ([string]$Manifest.version) }
    $src = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')
    $dst = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    if (-not (Test-SamePath $src $dst)) {
        foreach ($f in @($Manifest.files)) {
            $rel = ([string]$f.path) -replace '/', '\'
            [void](Copy-FileSafe -Source (Join-Path $src $rel) -Destination (Join-Path $dst $rel))
        }
        [void](Copy-FileSafe -Source (Join-Path $src 'manifest.json') -Destination (Join-Path $dst 'manifest.json'))
        # The setup window travels with the package: in the unpacked release it is one level above the package root,
        # in the closure it goes next to the manifest, where Get-ContinuationCommand and the repair entry look for it.
        $setupSrc = Get-PackageSetupExe $src
        if ($setupSrc) { [void](Copy-FileSafe -Source $setupSrc -Destination (Join-Path $dst $script:SetupExeName)) }
    }
    if ((Get-Sha256 (Join-Path $dst 'manifest.json')) -ne (Get-Sha256 (Join-Path $src 'manifest.json'))) { throw "continuation closure ${dst}: manifest.json is not this package's" }
    $check = Test-PackageManifest -PackageRoot $dst
    if (-not $check.ok) { throw "continuation closure ${dst}: $($check.detail)" }
    if ($FirmwareDir) { [void](Save-ClosureFirmware -Firmware $Manifest.firmware -Closure $dst -FromDir $FirmwareDir) }
    Write-Info "continuation closure $dst`: $(@($Manifest.files).Count) files checked$(if ($FirmwareDir) { ', firmware checked' })"
    return $dst
}
# The command that RunOnce runs at the next logon. kind 'continue': the installer of the closure; kind 'verify': the
# installed verify.cmd. A run of the setup window continues in the setup window of the closure, which starts the engine.
# The command line path starts Windows PowerShell directly, never cmd.exe: a folder name with &, ^, %, ( or ) cannot
# split the command, because no command interpreter reads it. The argument contract (RunOnce, docs/gui/
# interfaces-setup.md section 1): powershell.exe -NoProfile -ExecutionPolicy Bypass -File <dir>\installer\install.ps1
# -HoldWindow [-Verify], each part quoted by Format-CommandLine and read back by CommandLineToArgvW. -HoldWindow keeps
# the console open at the end, as the pause of install.cmd and verify.cmd does.
function Get-ContinuationCommand {
    param([ValidateSet('continue', 'verify')][string]$Kind, [string]$Closure, [string]$InstallRoot, [bool]$Gui)
    if ($Gui -and $Closure -and (Test-Path -LiteralPath (Join-Path $Closure $script:SetupExeName))) {
        return [pscustomobject]@{ exe = (Join-Path $Closure $script:SetupExeName); arguments = @('--continue') }
    }
    $ps = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $common = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')
    if ($Kind -eq 'verify') { return [pscustomobject]@{ exe = $ps; arguments = @($common + @((Join-Path $InstallRoot 'installer\install.ps1'), '-HoldWindow', '-Verify')) } }
    return [pscustomobject]@{ exe = $ps; arguments = @($common + @((Join-Path $Closure 'installer\install.ps1'), '-HoldWindow')) }
}
function Read-RepairSetIndex {
    $p = Join-Path (Get-PackagesDir) 'index.json'
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { $j = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json; if ($j.schema -eq $script:RepairIndexSchema) { return $j } } catch { }
    return $null
}
# Pure: which sets stay. The new active set and the set that was active before it (when it is another version and
# still present); every other version is removed.
function Get-RepairSetPlan {
    param($Index, [string]$ActiveVersion, [string[]]$Present = @())
    $prev = $null
    if ($Index -and $Index.active -and [string]$Index.active.version -ne $ActiveVersion) { $prev = [string]$Index.active.version }
    elseif ($Index -and $Index.previous -and [string]$Index.previous.version -ne $ActiveVersion) { $prev = [string]$Index.previous.version }
    if ($prev -and ($Present -notcontains $prev)) { $prev = $null }
    $keep = @($ActiveVersion) + @($(if ($prev) { $prev }))
    return [pscustomobject]@{ active = $ActiveVersion; previous = $prev; remove = @($Present | Where-Object { $keep -notcontains $_ }) }
}
# After phase 2: the closure becomes the active repair set, with the firmware that phase 2 installed. Returns the index.
function Complete-RepairSet {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)][string]$Closure, [string[]]$FirmwareFiles = @())
    $bad = Test-FirmwareFolder $Manifest.firmware (Join-Path $Closure 'firmware')
    if ($bad.Count) { [void](Save-ClosureFirmware -Firmware $Manifest.firmware -Closure $Closure -FromFiles $FirmwareFiles) }
    $dir = Get-PackagesDir
    $present = @(Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $plan = Get-RepairSetPlan (Read-RepairSetIndex) ([string]$Manifest.version) $present
    foreach ($v in @($plan.remove)) { Remove-PathOrSchedule (Join-Path $dir $v) }
    # Each set against its own manifest: a previous version keeps the firmware of its own release, which can differ
    # from this one's. A set whose manifest is missing, unreadable, of another version or without a firmware list is
    # not complete.
    $entry = {
        param([string]$Version)
        if (-not $Version) { return $null }
        $d = Join-Path $dir $Version
        $m = Join-Path $d 'manifest.json'
        $own = $null
        if (Test-Path -LiteralPath $m) { try { $own = Get-Content -LiteralPath $m -Raw | ConvertFrom-Json } catch { $own = $null } }
        $valid = ($own -is [pscustomobject]) -and ([string]$own.version -eq $Version) -and $own.firmware -and @($own.firmware.files).Count
        return [ordered]@{ version = $Version; dir = $d; manifest_sha256 = $(if (Test-Path -LiteralPath $m) { Get-Sha256 $m } else { $null }); manifest_valid = [bool]$valid
            firmware_complete = [bool]($valid -and (Test-FirmwareFolder $own.firmware (Join-Path $d 'firmware')).Count -eq 0); setup = (Test-Path -LiteralPath (Join-Path $d $script:SetupExeName)) }
    }
    $index = [ordered]@{ schema = $script:RepairIndexSchema; updated_utc = [DateTime]::UtcNow.ToString('o'); active = (& $entry $plan.active); previous = (& $entry $plan.previous) }
    [IO.File]::WriteAllText((Join-Path $dir 'index.json'), ($index | ConvertTo-Json -Depth 4))
    Write-Info "repair set: $($plan.active) active$(if ($plan.previous) { ", $($plan.previous) kept as the previous set" })$(if (@($plan.remove).Count) { "; removed $(@($plan.remove) -join ', ')" })"
    return $index
}

# ---------------------------------------------------------------------------------------------------------------
# What an install does to the settings (WU-006, WU-044): the registry-default plan of every group, as rows the setup
# window shows before any change. Installer-owned values (paths, counters, the Release record) are not settings and
# are left out. A tester's own profiles and per-user preferences are never in the table and never touched.
function Get-PreviousAppliedDefaults($RegistryDefaults, [string]$Record) {
    $applied = $null
    if ($Record) { try { $applied = $Record | ConvertFrom-Json } catch { $applied = $null } }
    if ($applied) { return [pscustomobject]@{ applied = $applied; source = 'Release\AppliedDefaults' } }
    return [pscustomobject]@{ applied = $RegistryDefaults.legacy_applied; source = 'the defaults of tester.1 to tester.7 (no record)' }
}
# Pure: rows from the plans. $Groups: a list of @{ group = <name>; plan = <Get-RegistryDefaultPlan result> }.
function Get-SettingsImpact($Groups) {
    $rows = New-Object System.Collections.ArrayList
    $sum = [ordered]@{ kept = 0; updated = 0; added = 0; unchanged = 0; command = 0; reopened = 0; driver_closed = 0; chosen = 0 }
    foreach ($g in @($Groups)) {
        foreach ($e in @($g.plan)) {
            if ($e.decision -eq 'installer' -or $e.decision -eq 'restored') { continue }
            switch ($e.decision) { 'kept' { $sum.kept++ } 'update' { $sum.updated++ } 'set' { $sum.added++ } 'same' { $sum.unchanged++ } 'command' { $sum.command++ }
                'reopened' { $sum.reopened++ } 'driver-closed' { $sum.driver_closed++ } 'chosen' { $sum.chosen++ } }
            # closure: the driver's own act in plain words, for the window and the support report. The reason code
            # stays in the installer's log (Format-RegistryPlan).
            $closure = $null
            if ($e.PSObject.Properties['closure']) { $closure = $e.closure }
            [void]$rows.Add([ordered]@{ group = $g.group; name = $e.name; decision = $e.decision; current = $e.current; value = $e.value; default = $e.default; present = [bool]$e.present; closure = $closure })
        }
    }
    return [pscustomobject]@{ rows = $rows.ToArray(); summary = $sum }
}
