# Shared functions of the tester installer (install.ps1, uninstall.ps1). Windows PowerShell 5.1 syntax only: the
# tester runs the copy of powershell.exe that ships with Windows 11, so no ternary, no '??', no '&&'.
#
# Every change to the system goes through Invoke-Change. In a dry run it prints what it would do and does nothing,
# so the dry run executes the same code path as the real run, checks included.

$script:ReleaseName      = 'amdgpu-wddm'
$script:ServiceName      = 'bc250kmd'
$script:HardwareIdPrefix = 'PCI\VEN_1002&DEV_13FE'          # bc250kmd.inf [Models.NTamd64]
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
$script:FirmwareInstallDir      = 'C:\BC250\firmware'                 # compiled into the KMD (psp.c BC250_PSP_FIRMWARE_DIR)
$script:StateDir         = Join-Path $env:ProgramData 'amdgpu-wddm\installer'
$script:StatePath        = Join-Path $script:StateDir 'state.json'
$script:ProfileListKey   = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
$script:ProfilePathsOverride = $null                           # host tests: fixture profile folders
$script:DryRunMode       = $false
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

# Asks the tester. -Answer (unattended) wins; a dry run never asks and answers as the expected answer; a run that the
# setup window drives never asks either (it has no console): what was not given on its command line is not consented.
function Read-Confirmation {
    param([Parameter(Mandatory)][string]$Question, [string]$Expect = 'YES', [string]$Answer)
    if ($Answer) { Write-Info "$Question -> $Answer (from the command line)"; return ($Answer -ceq $Expect) }
    if ($script:GuiMode) { Write-Info "$Question -> not given by the setup window: no"; return $false }
    # A dry run shows the path a YES takes, so it answers YES without asking.
    if ($script:DryRunMode) { Write-Host "   [dry run] would ask: $Question (type $Expect); continuing as if answered $Expect" -ForegroundColor DarkYellow; return $true }
    $reply = Read-Host "   $Question Type $Expect to continue"
    Write-Log "   asked: $Question -> '$reply'"
    return ($reply -ceq $Expect)
}

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
$script:InstallSwitches = @('NoControlApp', 'NoReboot', 'Force', 'Repair')
function Get-InstallInputs($Bound, $State, [string]$PackageVersion) {
    $resume = $State -and ([string]$State.phase -in $script:ResumePhases) -and ([string]$State.package_version -eq $PackageVersion)
    $in = [pscustomobject]@{ firmware_dir = $null; parameters = @{}; switches = @(); restored = @() }
    if ($Bound.ContainsKey('FirmwareDir') -and $Bound['FirmwareDir']) { $in.firmware_dir = [string]$Bound['FirmwareDir'] }
    elseif ($resume -and $State.firmware_source_dir) { $in.firmware_dir = [string]$State.firmware_source_dir; $in.restored += "-FirmwareDir $($in.firmware_dir)" }
    if ($Bound.ContainsKey('DpmMaxMHz')) { $in.parameters['DpmMaxMHz'] = [int]$Bound['DpmMaxMHz'] }
    if ($Bound.ContainsKey('CuMode') -and [int]$Bound['CuMode']) { $in.parameters['CuMode'] = [int]$Bound['CuMode'] }
    if (-not $in.parameters.Count -and $resume -and $State.command_line_parameters) {
        foreach ($p in $State.command_line_parameters.PSObject.Properties) { $in.parameters[$p.Name] = [int]$p.Value; $in.restored += "-$($p.Name) $([int]$p.Value)" }
    }
    foreach ($s in $script:InstallSwitches) {
        if ($Bound.ContainsKey($s)) { if ([bool]$Bound[$s]) { $in.switches += $s } }
        elseif ($resume -and (@($State.install_switches) -contains $s)) { $in.switches += $s; $in.restored += "-$s" }
    }
    return $in
}
function Save-InstallInputs($State, $Inputs) {
    Set-StateValue $State 'firmware_source_dir' $Inputs.firmware_dir
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
    foreach ($n in 'firmware_source_dir', 'command_line_parameters', 'install_switches') { Set-StateValue $State $n $null }
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
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($script:EnvironmentKey, $false)
    try {
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        if ($k.GetValueNames() -contains 'Path') { $kind = $k.GetValueKind('Path') }
        return [pscustomobject]@{ value = [string]$k.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames); kind = $kind }
    } finally { $k.Close() }
}
function Set-MachinePath([string]$Value, $Kind) {
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($script:EnvironmentKey, $true)
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

# The INF's Reboot directive in each install section that its models name: Windows 8 and later then install the
# package but do not restart a device that is already started, pnputil answers 3010, and the device changes driver
# at the next restart (INF Reboot directive, Remarks). build-release.ps1 adds it to the packaged INF.
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
# The INF text with a Reboot line as the first line of each install section that lacks one; nothing else changes.
function Add-InfRebootDirective([string]$Text) {
    $lines = $Text -split "`r?`n"
    $sections = Get-InfInstallSections $lines
    if (-not $sections.Count) { throw 'INF: no install section found through [Manufacturer] and its models' }
    if (Test-InfDefersDeviceRestart $lines) { return $Text }
    $nl = $(if ($Text -match "`r`n") { "`r`n" } else { "`n" })
    foreach ($s in $sections) {
        $hx = [regex]('(?m)^\[' + [regex]::Escape($s) + '\][ \t]*\r?\n')
        if ($hx.Matches($Text).Count -ne 1) { throw "INF: expected exactly one [$s] section" }
        $Text = $hx.Replace($Text, { param($m) $m.Value + 'Reboot                                          ; release package: the GPU changes driver at the next restart' + $nl })
    }
    if (-not (Test-InfDefersDeviceRestart ($Text -split "`r?`n"))) { throw 'INF: Reboot directive missing after the rewrite' }
    return $Text
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
    try { if (Confirm-SecureBootUEFI -ErrorAction Stop) { return 'on' } else { return 'off' } }
    catch [System.PlatformNotSupportedException] { return 'legacy-bios' }
    catch {
        $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue).UEFISecureBootEnabled
        if ($v -eq 1) { return 'on' }
        if ($v -eq 0) { return 'off' }
        return 'unknown'
    }
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
function Get-MemoryIntegrityState {
    $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -Name Enabled -ErrorAction SilentlyContinue).Enabled
    if ($v -eq 1) { return 'on' }
    return 'off'
}
function Get-VcRuntimeMissing {
    $missing = @()
    foreach ($f in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $env:windir "System32\$f"))) { $missing += $f }
    }
    return $missing
}
# The UserModeDriverName slots of the GPU's software key: D3D9, D3D10, D3D11, D3D12. The D3D9 slot is empty: a D3D9
# application then gets Microsoft's D3D9On12 layer (d3d9on12.dll), which runs on our D3D12 driver. Windows does not
# fall back to D3D9On12 when the slot names a UMD that cannot open the adapter: with the stub of the earlier releases
# (bc250umd.dll, OpenAdapter returns E_NOTIMPL) CreateDeviceEx failed with D3DERR_NOTAVAILABLE, with the empty slot it
# renders through d3d9on12.dll -> d3d12.dll -> amdgpu_wddm_d3d12.dll (tools/win/d3d9probe, unit A, 2026-10-06).
function Get-UmdNames([string]$InstallRoot) {
    $router = Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'
    return @('', $router, $router, (Join-Path $InstallRoot 'd3d12\amdgpu_wddm_d3d12.dll'))
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
# the empty D3D9 slot, the D3D10 and D3D11 routers, and the x86 D3D12 shell. As on x64 the D3D12 slot names the shell
# itself, not a router, and the shell loads its engine and ICD from its own directory (wow64\d3d12), so the D3D12 pair
# needs no Vulkan ICD registration. With the D3D12 slot a 32-bit D3D9 application gets D3D9On12 on our GPU, the same
# route as a 64-bit one. The x86 router and the x86 D3D12 shell read the 64-bit policy keys; the router also reads its
# own *Wow path values. The x86 builds link the C runtime statically: no x86 Visual C++ runtime is needed.
function Get-WowUmdNames([string]$InstallRoot) {
    $router = Join-Path $InstallRoot 'wow64\desktop\bc250d3d_router.dll'
    return @('', $router, $router, (Join-Path $InstallRoot 'wow64\d3d12\amdgpu_wddm_d3d12.dll'))
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
# Our scheduled tasks. The installer registers one ('amdgpu-wddm start confirm'); the sweep takes any task of ours,
# so a task that an older release named differently goes with it.
function Get-OurScheduledTasks {
    try { return @(Get-ScheduledTask -ErrorAction Stop | Where-Object { $_.TaskName -like "$($script:ReleaseName)*" } | ForEach-Object { $_.TaskName }) } catch { return @() }
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
    & $probe 'Start menu' {
        $lnks = @(@('amdgpu-wddm Control.lnk', 'amdgpu-wddm Control (recovery).lnk') | ForEach-Object { Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\$_" } | Where-Object { Test-Path -LiteralPath $_ })
        @{ present = ($lnks.Count -gt 0); detail = $(if ($lnks.Count) { @($lnks | ForEach-Object { Split-Path $_ -Leaf }) -join ', ' } else { 'no shortcut of ours' }) }
    }
    & $probe 'installer state' { $p = (Test-Path -LiteralPath $script:StateDir); @{ present = $p; detail = "$($script:StateDir)$(if ($p) { ' is there (state, kept repair sets, verify reports)' } else { ' is gone' })" } }
    & $probe 'per-user data' { $u = @(Get-OurUserDataDirs); @{ present = ($u.Count -gt 0); detail = $(if ($u.Count) { $u -join ', ' } else { 'no %LOCALAPPDATA%\amdgpu-wddm in any profile' }) } }
    & $probe 'certificates' { $c = @(Get-OurCertificates); @{ present = ($c.Count -gt 0); detail = $(if ($c.Count) { @($c | ForEach-Object { "$($_.store) $($_.thumbprint)" }) -join ', ' } else { 'no certificate of ours in Root or TrustedPublisher' }) } }
    # Kept on purpose: the firmware folder and C:\BC250 when they were there before the install, and the control
    # application's own files (the tester's setting backups and action log).
    $fwKept = [bool]($State -and $State.firmware_dir_existed)
    & $probe 'GPU firmware' { $p = (Test-Path -LiteralPath $script:FirmwareInstallDir); @{ present = $p; detail = "$($script:FirmwareInstallDir)$(if (-not $p) { ' is gone' } elseif ($fwKept) { ' is there, and it was there before the install' } else { ' is there' })" } } $fwKept
    & $probe 'control application data' { $d = Join-Path (Split-Path $script:StateDir) 'control'; $p = (Test-Path -LiteralPath $d); @{ present = $p; detail = "$d$(if ($p) { ' is there (the tester''s setting backups and action log)' } else { ' is gone' })" } } $true
    return $rows.ToArray()
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
# firmware into its own folder, and install.cmd, the documents and the setup window's own folder are read from the
# package and never copied.
function Get-InstalledPathOfPackageFile {
    param([Parameter(Mandatory)][string]$PackagePath, [Parameter(Mandatory)][string]$InstallRoot,
        [string]$SystemDir = (Join-Path $env:windir 'System32'), [string]$SysWowDir = (Join-Path $env:windir 'SysWOW64'))
    $p = ([string]$PackagePath) -replace '/', '\'
    $p = $p.TrimStart('\')
    if (-not $p) { return $null }
    if ($p -match '\.\.') { return $null }
    if ($p -match '^payload\\system32\\(.+)$') { return (Join-Path $SystemDir $Matches[1]) }
    if ($p -match '^payload\\syswow64\\(.+)$') { return (Join-Path $SysWowDir $Matches[1]) }
    if ($p -match '^payload\\(kmd|cert|firmware)\\') { return $null }
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
# older sets are removed. A prepared offline folder (prepare-offline.ps1) has the same layout.
$script:SetupExeRelative = 'setup\amdgpu_wddm_setup.exe'
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
    if ($Gui -and $Closure -and (Test-Path -LiteralPath (Join-Path $Closure $script:SetupExeRelative))) {
        return [pscustomobject]@{ exe = (Join-Path $Closure $script:SetupExeRelative); arguments = @('--continue') }
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
            firmware_complete = [bool]($valid -and (Test-FirmwareFolder $own.firmware (Join-Path $d 'firmware')).Count -eq 0); setup = (Test-Path -LiteralPath (Join-Path $d $script:SetupExeRelative)) }
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
    $sum = [ordered]@{ kept = 0; updated = 0; added = 0; unchanged = 0; command = 0; reopened = 0; driver_closed = 0 }
    foreach ($g in @($Groups)) {
        foreach ($e in @($g.plan)) {
            if ($e.decision -eq 'installer' -or $e.decision -eq 'restored') { continue }
            switch ($e.decision) { 'kept' { $sum.kept++ } 'update' { $sum.updated++ } 'set' { $sum.added++ } 'same' { $sum.unchanged++ } 'command' { $sum.command++ }
                'reopened' { $sum.reopened++ } 'driver-closed' { $sum.driver_closed++ } }
            # closure: the driver's own act in plain words, for the window and the support report. The reason code
            # stays in the installer's log (Format-RegistryPlan).
            $closure = $null
            if ($e.PSObject.Properties['closure']) { $closure = $e.closure }
            [void]$rows.Add([ordered]@{ group = $g.group; name = $e.name; decision = $e.decision; current = $e.current; value = $e.value; default = $e.default; present = [bool]$e.present; closure = $closure })
        }
    }
    return [pscustomobject]@{ rows = $rows.ToArray(); summary = $sum }
}
