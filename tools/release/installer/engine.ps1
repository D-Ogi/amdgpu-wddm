# The engine's contract with the setup window (docs/gui/interfaces-setup.md): events, the terminal result, cancel at
# safe points, one mutating engine at a time, the boot identity and the record of the newest install action (the
# state.json fields that void an older running-release witness, docs/gui/interfaces.md section 2). Dot-sourced after
# common.ps1, together with release-witness.ps1 (the boot identity), by install.ps1 and prepare-offline.ps1; the
# start-confirm task dot-sources only release-witness.ps1. Windows PowerShell 5.1 syntax only.
#
# The command-line installer behaves as before: without -EventsFile and -ResultFile nothing here writes a file, and
# without -Gui nothing here changes how the installer asks. With -Gui the engine never prompts, never elevates itself
# and never restarts Windows: what it needs (a consent, administrator rights, a restart) ends the run with a terminal
# result that names it, and the window asks the user.

$script:EngineContract = 'amdgpu-wddm.engine/1'
$script:EventSchema    = 'amdgpu-wddm.engine-event/1'
$script:ResultSchema   = 'amdgpu-wddm.engine-result/1'
$script:GuiMode        = $false
$script:EngineInvocationId   = $null
$script:EngineEventsFile     = $null
$script:EngineResultFile     = $null
$script:EngineCancelFile     = $null
$script:EventSeq       = 0
$script:EngineMode     = 'run'
$script:EngineStarted  = [DateTime]::UtcNow.ToString('o')
$script:EngineAction   = $null
$script:EnginePackageVersion = $null
$script:EnginePhaseBefore = $null
$script:EngineRestart  = $null
$script:EngineConsents = @()
$script:EngineFailedChecks = @()
$script:EngineLock     = $null
$script:EngineBoot     = $null
$script:EngineMutationSeen = $false

function Initialize-Engine {
    param([bool]$Gui, [string]$InvocationId, [string]$EventsFile, [string]$ResultFile, [string]$Mode = 'run')
    $script:GuiMode = $Gui
    if (-not $InvocationId -and ($Gui -or $EventsFile -or $ResultFile)) { $InvocationId = [guid]::NewGuid().ToString() }
    $script:EngineInvocationId = $InvocationId
    $script:EngineEventsFile = $(if ($EventsFile) { [IO.Path]::GetFullPath($EventsFile) } else { $null })
    $script:EngineResultFile = $(if ($ResultFile) { [IO.Path]::GetFullPath($ResultFile) } else { $null })
    # The window asks for a cancel by creating this file; the engine looks for it only at a safe point.
    $script:EngineCancelFile = $(if ($script:EngineEventsFile) { $script:EngineEventsFile + '.cancel' } else { $null })
    $script:EngineMode = $Mode
    # A result file left by another invocation never stands for this one.
    if ($script:EngineResultFile -and (Test-Path -LiteralPath $script:EngineResultFile)) { Remove-Item -LiteralPath $script:EngineResultFile -Force -ErrorAction SilentlyContinue }
    # The first system change of this run is recorded in state.json before it is made: a running-release witness written
    # earlier in this boot no longer names what runs. Every later change moves the time on (saved with the state).
    $script:OnFirstChange = { Set-MutationRecord -Save }
    $script:OnMutation = { Set-MutationRecord }
}

function ConvertTo-EngineJson($Value) { return ($Value | ConvertTo-Json -Depth 10 -Compress) }

# One event: one JSON object on one line, appended. The reader takes complete lines only.
function Write-EngineEvent {
    param([Parameter(Mandatory)][string]$Type, [Collections.IDictionary]$Data = @{})
    if (-not $script:EngineEventsFile) { return }
    $script:EventSeq++
    $e = [ordered]@{ schema = $script:EventSchema; invocation = $script:EngineInvocationId; seq = $script:EventSeq; utc = [DateTime]::UtcNow.ToString('o'); type = $Type }
    foreach ($k in $Data.Keys) { $e[[string]$k] = $Data[$k] }
    $line = (ConvertTo-EngineJson $e) + "`n"
    try {
        [void][IO.Directory]::CreateDirectory((Split-Path $script:EngineEventsFile))
        $fs = New-Object IO.FileStream -ArgumentList $script:EngineEventsFile, ([IO.FileMode]::Append), ([IO.FileAccess]::Write), ([IO.FileShare]::ReadWrite)
        try { $b = (New-Object Text.UTF8Encoding $false).GetBytes($line); $fs.Write($b, 0, $b.Length); $fs.Flush() } finally { $fs.Dispose() }
    } catch { Write-Log "   event not written: $($_.Exception.Message)" }
}

# Writes a file through a temporary file and a rename, so that a reader sees the old file or the whole new one.
function Write-FileAtomic([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory((Split-Path $Path))
    $tmp = $Path + '.tmp-' + [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($tmp, $Text, (New-Object Text.UTF8Encoding $false))
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
}

# The boot (boot_id, boot_utc) comes from Get-BootIdentity in release-witness.ps1, the one place that defines it.

# ---- the record of the newest install action ----------------------------------------------------------------------
# state.json mutation_boot_id and mutation_utc (docs/gui/interfaces.md section 2): the boot and the start time of the
# newest step of a real run that changes Windows, the driver, files or settings. A dry run, a plan, verify and
# prepare-offline are not install actions. The first change of a run saves the state at once, before the change; later
# changes move the time on in memory and reach state.json with the next save of the state (every phase saves it). A
# running-release witness of the same boot that is older than mutation_utc no longer names what runs.
function Set-MutationRecord {
    param([switch]$Save)
    if ($script:DryRunMode -or -not $script:state) { return }
    $boot = Get-BootIdentity
    $first = -not $script:EngineMutationSeen
    Set-StateValue $script:state 'mutation_boot_id' $boot.boot_id
    Set-StateValue $script:state 'mutation_utc' ([DateTime]::UtcNow.ToString('o'))
    $script:EngineMutationSeen = $true
    if ($Save) {
        try { Save-InstallState $script:state } catch { Write-Warn2 "install action not recorded in $($script:StatePath): $($_.Exception.Message)" }
    }
    if ($first) { Write-EngineEvent 'install-action' ([ordered]@{ action = $script:EngineAction; boot_id = $boot.boot_id }) }
}

# ---- one mutating engine ----------------------------------------------------------------------------------------
# An exclusive handle on <state dir>\engine.lock for the life of the process; Windows closes it when the process ends,
# also after a crash. Dry runs and plans change nothing and take no lock.
function Enter-EngineLock([string]$Directory = $script:StateDir) {
    if ($script:DryRunMode) { return $true }
    try {
        [void][IO.Directory]::CreateDirectory($Directory)
        $script:EngineLock = New-Object IO.FileStream -ArgumentList (Join-Path $Directory 'engine.lock'), ([IO.FileMode]::OpenOrCreate), ([IO.FileAccess]::ReadWrite), ([IO.FileShare]::None)
        return $true
    } catch { return $false }
}

# ---- cancel -------------------------------------------------------------------------------------------------------
# Cancel is possible only at a safe point: before the first change of a phase, never in the middle of one. The window
# learns from 'cancel' events whether its Cancel button can work now.
function Set-CancelAvailable([bool]$Available, [string]$Where) {
    Write-EngineEvent 'cancel' ([ordered]@{ available = $Available; where = $Where })
}
function Test-CancelRequested {
    return [bool]($script:EngineCancelFile -and (Test-Path -LiteralPath $script:EngineCancelFile))
}
function Invoke-CancelPoint([string]$Where) {
    if (Test-CancelRequested) {
        Write-Host "Stopped at a safe point ($Where): cancelled from the setup window." -ForegroundColor Yellow
        Exit-Engine -Code 8 -Outcome 'cancelled' -MessageId $(if ($script:Mutated) { 'result.cancelled-after-changes' } else { 'result.cancelled' }) -Detail "cancel requested; stopped at the safe point '$Where'"
    }
}

# ---- the terminal result ----------------------------------------------------------------------------------------
# Written once, at the end of every run that got this far, also after a failed step (the trap). The window believes
# the result only when its invocation is the one it started; the exit code alone is never the outcome.
function Exit-Engine {
    param([Parameter(Mandatory)][int]$Code, [Parameter(Mandatory)][string]$Outcome, [string]$MessageId, [string]$Detail, [string]$Step)
    $phaseAfter = $null
    try { if ($script:state -and $script:state.PSObject.Properties['phase']) { $phaseAfter = [string]$script:state.phase } } catch { }
    $restart = $script:EngineRestart
    if (-not $restart) { $restart = [ordered]@{ required = $false } }
    $r = [ordered]@{
        schema = $script:ResultSchema
        invocation = $script:EngineInvocationId
        engine = [ordered]@{ contract = $script:EngineContract; package_version = $script:EnginePackageVersion }
        mode = $script:EngineMode
        dry_run = [bool]$script:DryRunMode
        started_utc = $script:EngineStarted
        ended_utc = [DateTime]::UtcNow.ToString('o')
        boot = (Get-BootIdentity)
        action = $script:EngineAction
        outcome = $Outcome
        exit_code = $Code
        mutated = [bool]$script:Mutated
        nothing_changed = -not [bool]$script:Mutated
        phase_before = $script:EnginePhaseBefore
        phase_after = $phaseAfter
        restart = $restart
        consents_needed = @($script:EngineConsents)
        failed_checks = @($script:EngineFailedChecks)
        message_id = $MessageId
        step = $(if ($Step) { $Step } else { $null })
        detail = $Detail
        log = $script:LogPath
    }
    Write-EngineEvent 'result' ([ordered]@{ outcome = $Outcome; exit_code = $Code; message_id = $MessageId; mutated = [bool]$script:Mutated })
    if ($script:EngineResultFile) {
        try { Write-FileAtomic $script:EngineResultFile ($r | ConvertTo-Json -Depth 10) } catch { Write-Log "   result not written: $($_.Exception.Message)" }
    }
    Write-Log "   engine result: $Outcome (exit $Code$(if ($MessageId) { ", $MessageId" }))"
    if ($script:EngineLock) { try { $script:EngineLock.Dispose() } catch { } }
    exit $Code
}
