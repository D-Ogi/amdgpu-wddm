# Shared functions of the tester installer (install.ps1, uninstall.ps1). Windows PowerShell 5.1 syntax only: the
# tester runs the copy of powershell.exe that ships with Windows 11, so no ternary, no '??', no '&&'.
#
# Every change to the system goes through Invoke-Change. In a dry run it prints what it would do and does nothing,
# so the dry run executes the same code path as the real run, checks included.

$script:ReleaseName      = 'amdgpu-wddm'
$script:ServiceName      = 'bc250kmd'
$script:HardwareIdPrefix = 'PCI\VEN_1002&DEV_13FE'          # bc250kmd.inf [Models.NTamd64]
$script:DisplayClassGuid = '{4d36e968-e325-11ce-bfc1-08002be10318}'
$script:SoftwareKey      = 'HKLM:\SOFTWARE\amdgpu-wddm'
$script:ParametersKey    = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$script:KhronosKey       = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
$script:RunOnceKey       = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
$script:RunOnceName      = 'amdgpu-wddm-installer'
$script:TaskName         = 'amdgpu-wddm start confirm'
$script:FirmwareInstallDir      = 'C:\BC250\firmware'                 # compiled into the KMD (psp.c BC250_PSP_FIRMWARE_DIR)
$script:StateDir         = Join-Path $env:ProgramData 'amdgpu-wddm\installer'
$script:StatePath        = Join-Path $script:StateDir 'state.json'
$script:DryRunMode       = $false
$script:LogPath          = $null
$script:CurrentStep      = '(before the first change)'
$script:ScheduleOldCopies = $true                              # host tests set $false: no MoveFileEx on the test PC

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
    if ($script:DryRunMode) { Write-Host "   [dry run] would: $Description" -ForegroundColor DarkYellow; return $null }
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

# Asks the tester. -Answer (unattended) wins; a dry run never asks and answers 'no'.
function Read-Confirmation {
    param([Parameter(Mandatory)][string]$Question, [string]$Expect = 'YES', [string]$Answer)
    if ($Answer) { Write-Info "$Question -> $Answer (from the command line)"; return ($Answer -ceq $Expect) }
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

# The inputs of an install that phase 2 needs: the firmware folder (-FirmwareDir), the driver settings given on the
# command line (-DpmMaxMHz, -CuMode) and the switches that select what is installed and how (-NoControlApp, -NoReboot,
# -Force). The run after a restart that the installer asked for (test signing, or a driver package that waits for the
# restart) starts from RunOnce without arguments: it takes them from the state, if the state is of the same package
# version. A value given on its own command line wins. Every run saves its effective inputs; phase 2 clears them when
# it completes.
$script:ResumePhases = @('testsigning-pending', 'driver-pending-restart')
$script:InstallSwitches = @('NoControlApp', 'NoReboot', 'Force')
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
# Pure: the defaults, what the previous installer wrote, what is in the key now (name -> value; absent = no entry),
# the command-line values and the installer-owned values in; one entry per value out (name, value, decision, write).
function Get-RegistryDefaultPlan {
    param($Defaults, $Previous, [hashtable]$Current = @{}, [hashtable]$Explicit = @{}, $Owned = $null, [hashtable]$After = $null, [string[]]$Restore = @())
    $prev = @{}
    foreach ($p in ConvertTo-PairList $Previous) { $prev[$p.Name] = $p.Value }
    $plan = New-Object System.Collections.ArrayList
    $seen = @{}
    foreach ($d in ConvertTo-PairList $Defaults) {
        $seen[$d.Name] = $true
        $has = $Current.ContainsKey($d.Name)
        $cur = $null; if ($has) { $cur = $Current[$d.Name] }
        if ($Explicit.ContainsKey($d.Name)) { $decision = 'command'; $value = $Explicit[$d.Name] }
        elseif (-not $has) { $decision = 'set'; $value = $d.Value }
        elseif (Test-RegistryValueSame $cur $d.Value) { $decision = 'same'; $value = $d.Value }
        elseif ($prev.ContainsKey($d.Name) -and (Test-RegistryValueSame $cur $prev[$d.Name])) { $decision = 'update'; $value = $d.Value }
        else { $decision = 'kept'; $value = $cur }
        [void]$plan.Add([pscustomobject]@{ name = $d.Name; value = $value; default = $d.Value; current = $cur; present = $has; decision = $decision; write = ($decision -in @('command', 'set', 'update')) })
    }
    foreach ($e in $Explicit.GetEnumerator()) {
        if ($seen.ContainsKey($e.Key)) { continue }
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
    foreach ($n in @($Restore)) {
        if ($seen.ContainsKey($n) -or -not $Current.ContainsKey($n)) { continue }
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
            if ($differs) { $e.write = $true }
        }
    }
    return , $plan.ToArray()
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
# path, "Reopen the GPU desktop path" in the control application opens it again (INSTALL.md).

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
        if (-not $e.write) { continue }
        $v = $e.value
        if ($v -is [array]) { New-ItemProperty -LiteralPath $Key -Name $e.name -Value ([string[]]@($v | ForEach-Object { [string]$_ })) -PropertyType MultiString -Force | Out-Null }
        elseif ($v -is [string]) { New-ItemProperty -LiteralPath $Key -Name $e.name -Value $v -PropertyType String -Force | Out-Null }
        else { New-ItemProperty -LiteralPath $Key -Name $e.name -Value ([int]$v) -PropertyType DWord -Force | Out-Null }
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
            try {
                $r = Invoke-WebRequest -Uri $u -Method Head -UseBasicParsing -TimeoutSec $TimeoutSec -UserAgent $script:DownloadUserAgent
                if ([int]$r.StatusCode -eq 200) { $ok += $name } else { $bad += "$name HTTP $([int]$r.StatusCode)" }
            } catch { $bad += "$name $($_.Exception.Message)" }
        }
        if ($ok.Count -or $round -eq $Rounds) { break }
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
function Get-TestSigningActive {
    # The options the running boot was started with; readable without elevation.
    $o = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
    return ([string]$o -match 'TESTSIGNING')
}
function Get-TestSigningConfigured {
    # What the next boot will use; needs elevation. $null when unknown.
    if (-not (Test-IsAdmin)) { return $null }
    $n = Invoke-Native bcdedit.exe @('/enum', '{current}')
    if ($n.code -ne 0) { return $null }
    return ($n.text -match '(?im)^\s*testsigning\s+Yes\s*$')
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
    if (-not (Test-IsAdmin)) { return 'unknown' }
    try {
        $v = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop
        if ([string]$v.ProtectionStatus -eq 'On') { return 'on' }
        if ([string]$v.VolumeStatus -eq 'FullyDecrypted') { return 'off' }
        return 'suspended'
    } catch {
        $out = (Invoke-Native manage-bde.exe @('-status', $env:SystemDrive)).text
        if ($out -match '(?im)Protection Status:\s+Protection On') { return 'on' }
        if ($out -match '(?im)Protection Status:\s+Protection Off') { return 'off' }
        return 'unknown'
    }
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
# A lab installation (the project's development unit) carries files under C:\BC250\m1x. The release installer does
# not merge into one.
function Test-LabInstallPresent {
    foreach ($d in 'C:\BC250\m15', 'C:\BC250\m14', 'C:\BC250\m10') { if (Test-Path -LiteralPath $d) { return $true } }
    return $false
}

# Our driver packages in the driver store (pnputil /enum-drivers), by original name.
function Get-OurDriverPackages {
    $out = (Invoke-Native pnputil.exe @('/enum-drivers')).text
    $blocks = $out -split "(\r?\n){2,}"
    $r = @()
    foreach ($b in $blocks) {
        if ($b -match '(?im)^\s*Original Name:\s*bc250kmd\.inf\s*$' -and $b -match '(?im)^\s*Published Name:\s*(oem\d+\.inf)\s*$') { $r += $Matches[1] }
    }
    return $r
}

# MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT): delete a file that a running process (DWM) still has mapped.
function Remove-FileAtReboot([string]$Path) {
    if (-not ('AmdgpuWddmInstaller.Native' -as [type])) {
        Add-Type -Namespace AmdgpuWddmInstaller -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@
    }
    return [AmdgpuWddmInstaller.Native]::MoveFileEx($Path, $null, 4)
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
