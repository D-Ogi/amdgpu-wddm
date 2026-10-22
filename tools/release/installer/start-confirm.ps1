# Logon start-confirm of a tester PC: the scheduled task "amdgpu-wddm start confirm" (install.ps1 registers it from
# the plan of common.ps1: at the logon of any user, as Local System, highest run level, -NoWindow) runs this once per
# logon. The release variant of the lab's start-confirm.ps1; the decision rules are the lab's, unchanged, in
# start-confirm-core.ps1.
#
# Why it exists: every driver start counts against the KMD's boot-loop guard (guard.c GuardCheckAndCountStart,
# BC250_MAX_UNCONFIRMED_STARTS 2): at the third start nobody confirmed, the driver refuses to start and Windows uses
# Microsoft Basic Display Adapter. A DPM start has a guard of its own (dpm.c, DpmPending): the next start after an
# unconfirmed DPM start runs at the fixed floor clock and writes DpmMode 0. The KMD's start-health confirmation
# (start_health.c StartHealthRequest, BC250_ESCAPE_RUN_START_HEALTH) clears both, and the KMD accepts it only when
# the start has been ready and visible for 60 s with a completed flip, and either a fresh completion or nothing owed
# (QUIET, 0.7.216.104: every flip programmed in the interval has completed). This script reads the driver and
# confirms the start once the KMD's milestone is met. Then it exits: at health flags 15, or after -Seconds (120).
# Nothing stays resident.
#
# Up to tester.31 the milestone asked for a completion within 15 s at the 60 s mark, which an idle desktop rarely
# gives, and a small blue window with a countdown kept DWM presenting for it. That window came from the time of the
# CPU desktop and is gone (owner, 2026-10-22: "Niebieskie okienko jest z czasu pulpitu na CPU przeciez. Nie mozemy
# go usunac w calosci?"). An idle GPU desktop flips about once a minute, for the taskbar clock, and QUIET accepts it.
#
# BD-080: this script only reads and confirms, and shows nothing: the Local System account that confirms at the logon
# of any user has no desktop in that session. -NoWindow is still taken, because the registered task passes it; it
# changes nothing. The second task of the logon, start-window.ps1, runs in the session of the user who logged on and
# only starts the sign-in notice of the control application. If the KMD never reaches its milestone (a KMD older
# than QUIET on an idle desktop, for example), the fallback below resets the boot-loop guard alone: the machine stays
# off Basic Display and the next start runs at the floor clock.
#
# Health reads and the confirmation go through bc250control.dll next to this script (Bc250StartHealth, the same
# export the control application uses). If the KMD never reaches its milestone, a fallback resets only the
# boot-loop guard (bc250kmd_cli confirm) after 60 s of a healthy-looking start: device OK, the driver answers with
# the expected version, LastStage 50 (first VidPN commit). That keeps the PC out of Basic Display; the DPM request
# stays unconfirmed and the next start runs at the floor clock (the log says so).
#
# It writes a short status log, %ProgramData%\amdgpu-wddm\start-confirm.log, cut to its last 200 lines at each run, the
# DWM baseline of the logon (dwm-session.ps1) and, at its end, the running-release witness of this boot when the loaded
# KMD is the installed release's (release-witness.ps1, read by the control application). -Probe prints one reading of
# everything and exits (read-only; verify uses it).
param([int]$Seconds = 120, [switch]$Probe, [switch]$NoWindow, [string]$ExpectedVersion = '', [int]$FallbackHoldSeconds = 60)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# The driver version the fallback expects: manifest.json kmd_abi of the installed release (the install root is the
# parent of tools\).
if (-not $ExpectedVersion) {
    try { $ExpectedVersion = [string](Get-Content -LiteralPath (Join-Path (Split-Path $here) 'manifest.json') -Raw | ConvertFrom-Json).kmd_abi } catch { }
    if (-not $ExpectedVersion) { $ExpectedVersion = '0x000700D8' }   # this release's kmd_abi; test-dryrun.ps1 gates it
}
. (Join-Path $here 'start-confirm-core.ps1')
. (Join-Path $here 'state-reader.ps1')
# The boot identity (Get-BootIdentity), loaded here and not only before the witness at the end: the start record's
# reading needs this boot's start time from the one place that defines it (start-confirm-core.ps1
# Get-StartRecordBootUtc), and -Probe exits before the witness block.
# Guarded, because this script runs under $ErrorActionPreference = 'Stop' and release-witness.ps1 runs Add-Type at
# its top level, which needs a writable TEMP and the CodeDom compiler. Unguarded, a failure there stopped this
# script before it read anything: the task that keeps the PC out of Basic Display never ran its confirmation loop,
# and -Probe printed no line at all, which is the text verify parses for the device, the driver version, LastStage
# and the start health. A failure is a line in the log instead; the boot time then reads as unknown and the witness
# block at the end says why.
$script:witnessLoadError = $null
try { . (Join-Path $here 'release-witness.ps1') } catch { $script:witnessLoadError = [string]$_.Exception.Message }
$clock = [Diagnostics.Stopwatch]::StartNew()
$cli = Join-Path $here 'bc250kmd_cli.exe'
$dll = [IO.Path]::GetFullPath((Join-Path $here 'bc250control.dll'))
$log = Join-Path $env:ProgramData 'amdgpu-wddm\start-confirm.log'
$script:exitCode = 1
$script:lastReason = $null

if (-not $Probe) {
    try {
        [void][IO.Directory]::CreateDirectory((Split-Path $log))
        if (Test-Path -LiteralPath $log) { $keep = @(Get-Content -LiteralPath $log -Tail 400); [IO.File]::WriteAllLines($log, [string[]]$keep) }
    } catch { }
}
function Write-StartConfirmLog([string]$Text) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $line = [string]::Format($inv, '{0} +{1,6:F1}s {2}', [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', $inv), $clock.Elapsed.TotalSeconds, $Text)
    if ($Probe) { $line; return }
    try { [IO.File]::AppendAllText($log, $line + "`r`n") } catch { }
}
if ($script:witnessLoadError) {
    Write-StartConfirmLog "release-witness.ps1 did not load: $($script:witnessLoadError); this boot's start time reads as unknown and no witness is written"
}
# BD-060: the DWM instance of this logon's session, recorded once per logon as the baseline that verify and the control
# application compare against (dwm-session.ps1). Never fails the task; -Probe writes nothing.
if (-not $Probe) {
    try {
        . (Join-Path $here 'dwm-session.ps1')
        $epoch = Get-DwmEpoch
        $saved = Save-DwmBaseline $epoch (Get-SessionDwm $epoch.session)
        if ($saved.written) { Write-StartConfirmLog "DWM baseline: session $($epoch.session), DWM $($saved.record.dwm_pid) created $($saved.record.dwm_created_utc), recorded" }
        elseif ($saved.record) { Write-StartConfirmLog "DWM baseline: session $($epoch.session) already recorded at $($saved.record.recorded_utc) (DWM $($saved.record.dwm_pid))" }
        else { Write-StartConfirmLog "DWM baseline: not recorded (session $($epoch.session), logon $($epoch.logon_utc), $(@(Get-SessionDwm $epoch.session).Count) DWM)" }
    } catch { Write-StartConfirmLog "DWM baseline: $($_.Exception.Message)" }
}
function Read-Parameter([string]$Name) {
    try {
        $values=Read-DriverStateValues -Cli $cli -MaxAgeMilliseconds 1000
        if(-not $values.ContainsKey($Name)){return 'absent'}
        return [string]$values[$Name]
    } catch { return "unreadable: $($_.Exception.Message)" }
}
# BD-122: the KMD's record of the last start that did not reach a started device (Parameters\StartRecord,
# guard.c). Read with no gate and without the driver, which is the point: a device with Code 43 answers no escape.
# The record's own values only; the rule that makes them current is in start-confirm-core.ps1.
function Read-StartRecord {
    $k = Open-DriverStateRecordView -Cli $cli -Path $script:StartRecordKey
    if (!$k) { return $null }
    try { return Read-StartRecordFromKey $k } finally { $k.Dispose() }
}
# One line for the log, and the kept ring lines under it when the record is a reason for this start. -Probe prints
# every kept line; the task's own log keeps the last $Lines of them, so one logon cannot push the file out.
# This boot's start time goes into both calls: a record older than this boot says so and keeps its lines to itself.
$script:bootUtc = Get-StartRecordBootUtc
function Write-StartRecordLog([int]$Lines = 24) {
    $record = Read-StartRecord
    Write-StartConfirmLog ('start record: ' + (Format-StartRecord -Record $record -BootUtc $script:bootUtc))
    if (-not (Test-StartRecordIsReason -Record $record -BootUtc $script:bootUtc)) { return }
    foreach ($line in (Get-StartRecordLogLines -Record $record -Max $Lines)) { Write-StartConfirmLog "start record log: $line" }
}
function Format-Dpm { return "DpmMode $(Read-Parameter 'DpmMode') DpmMaxMHz $(Read-Parameter 'DpmMaxMHz') DpmPending $(Read-Parameter 'DpmPending') DpmConfirmed $(Read-Parameter 'DpmConfirmed') DpmLastMode $(Read-Parameter 'DpmLastMode') DpmLastReason $(Read-Parameter 'DpmLastReason')" }
# A native program's stdout and stderr as one text, and its exit code (stderr stays text under Stop in 5.1).
function Invoke-Cli([string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& $cli @Arguments 2>&1 | ForEach-Object { [string]$_ }) -join "`n" } finally { $ErrorActionPreference = $old }
    return @{ text = $text; code = $LASTEXITCODE }
}

# ---- the KMD's start health through bc250control.dll --------------------------------------------------------------
function Initialize-Interop {
    if ('Bc250StartConfirm.Kmd' -as [type]) { return }
    if (-not (Test-Path -LiteralPath $dll)) { throw "missing $dll" }
    Add-Type -TypeDefinition (Get-StartConfirmInteropSource -DllPath $dll)
}
function Invoke-StartHealth([uint32]$Op, [uint64]$Generation, [uint64]$Epoch) {
    Initialize-Interop
    $d = New-Object Bc250StartConfirm.Health
    $status = [Bc250StartConfirm.Kmd]::Bc250StartHealth($Op, $Generation, $Epoch, [ref]$d, 96)
    return @{ status = $status; data = $d }
}
function Read-Health {
    $r = Invoke-StartHealth 0 0 0
    if ($r.status -ne 0) { throw ('start health read refused, status 0x{0:X8}' -f $r.status) }
    $d = $r.data
    return [ordered]@{ version = ('0x{0:X8}' -f $d.Version); flags = [int]$d.Flags; generation = [uint64]$d.Generation; epoch = [uint64]$d.Epoch
        completed = [uint64]$d.Completed; age_ms = [uint64]$d.LastCompletionAgeMs; ready_ms = [uint64]$d.ReadyAgeMs; source = 'dll' }
}
function Confirm-Health([uint64]$Generation, [uint64]$Epoch) {
    $r = Invoke-StartHealth 1 $Generation $Epoch
    if ($r.status -ne 0) { throw ('confirm refused, status 0x{0:X8} flags {1}' -f $r.status, $r.data.Flags) }
    return "flags $($r.data.Flags)"
}
function Format-Reading($r) { if (!$r) { return 'none' }; return "version $($r.version) flags $($r.flags) generation $($r.generation) epoch $($r.epoch) completed $($r.completed) age_ms $($r.age_ms) ready_ms $($r.ready_ms)" }

# ---- the fallback's view: device, driver version, last stage ------------------------------------------------------
function Get-FallbackView {
    $dev = @(Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'PCI\\VEN_1002&DEV_13FE%'" -ErrorAction SilentlyContinue)
    $problem = $(if ($dev.Count -eq 1) { [int]$dev[0].ConfigManagerErrorCode } else { -1 })
    $info = Invoke-Cli @('info')
    $version = $null
    if ($info.code -eq 0 -and $info.text -match '(?m)^version\s+(0x[0-9A-Fa-f]{8})') { $version = $Matches[1] }
    $stages = Invoke-Cli @('stages')
    $stage = $null
    if ($stages.text -match '(?m)^LastStage\s+(\d+)') { $stage = [int]$Matches[1] }
    $ok = ($problem -eq 0) -and ($version -eq $ExpectedVersion) -and ($stage -eq 50)
    return [ordered]@{ ok = $ok; text = "device problem $problem, driver version $version (expected $ExpectedVersion), LastStage $stage" }
}

# ---- BD-080: the other half of this logon, the task in the user's session ----------------------------------------
# That task runs as the user who logged on (start-window.ps1, the task below), where this script cannot see it: a
# different session, and under Local System no desktop at all. What it can read is the task's own record, so the log
# of this run says whether that half ran and what it reported. start-window.ps1's exit codes: 0 the sign-in notice
# was started (or there is no control application), 2 Windows refused to start it. Nagi Control App, which starts
# that script for the task (--start-window), passes those through and adds 4 (it stopped the script at its bound) and
# 5 (it could not start the script). 267009 is "running", and the task is absent on a machine where an older release
# registered the single task only. The task keeps its old name. This never throws and never changes the run's
# outcome; it is a log line.
$windowTaskName = 'amdgpu-wddm start window'   # common.ps1 $script:WindowTaskName; test-session-checks.ps1 holds the two together
function Format-WindowTask {
    try {
        $t = Get-ScheduledTask -TaskName $windowTaskName -ErrorAction SilentlyContinue
        if (-not $t) { return "window task: '$windowTaskName' is not registered on this computer" }
        $i = Get-ScheduledTaskInfo -TaskName $windowTaskName -ErrorAction Stop
        $last = $(if ($i.LastRunTime) { ([DateTime]$i.LastRunTime).ToUniversalTime().ToString('o') } else { 'never' })
        return "window task: state $($t.State), last run $last, last result $($i.LastTaskResult)"
    } catch { return "window task: not read ($($_.Exception.Message))" }
}

if ($Probe) {
    Write-StartConfirmLog "probe: $(Format-WindowTask)"
    Write-StartConfirmLog "probe: UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts')"
    Write-StartConfirmLog "probe: $(Format-Dpm)"
    Write-StartConfirmLog ('probe: fallback view: ' + (Get-FallbackView).text)
    Write-StartRecordLog -Lines $script:StartRecordMaxLogLines
    try { Write-StartConfirmLog ('probe: start health ' + (Format-Reading (Read-Health))); exit 0 }
    catch { Write-StartConfirmLog "probe: start health no reading: $($_.Exception.Message)"; exit 5 }
}

Write-StartConfirmLog "start: bound $Seconds s, UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts'), $(Format-Dpm)"
Write-StartRecordLog
$state = New-StartConfirmState $Seconds
$pollEvery = 5.0
$script:nextPoll = 0.0
$script:lastLogged = ''
$script:lastLoggedAt = -100.0
$script:fallbackSince = $null
$script:fallbackText = ''

# The fallback for an exit the KMD milestone did not reach (bound reached, or no reading at all): reset the boot-loop
# guard if the start has looked healthy for FallbackHoldSeconds. Returns the final step.
function Complete-WithFallback($Step) {
    if ($null -eq $script:fallbackSince -or ($clock.Elapsed.TotalSeconds - $script:fallbackSince) -lt $FallbackHoldSeconds) {
        $Step.reason = "$($Step.reason); fallback not taken: $($script:fallbackText)"
        return $Step
    }
    $c = Invoke-Cli @('confirm')
    $s = Invoke-Cli @('stages')
    $after = $null
    if ($s.text -match '(?m)^starts\s+(\d+) unconfirmed') { $after = [int]$Matches[1] }
    if ($c.code -eq 0 -and $after -eq 0) {
        return [ordered]@{ action = 'exit'; code = 6; reason = "$($Step.reason); fallback: boot-loop guard reset by bc250kmd_cli confirm after $FallbackHoldSeconds s of ($($script:fallbackText)), stages reports 0 unconfirmed; the DPM request is not confirmed: the next start runs at the floor clock" }
    }
    return [ordered]@{ action = 'exit'; code = 1; reason = "$($Step.reason); fallback confirm failed: exit $($c.code) $($c.text.Trim()); stages: $after unconfirmed" }
}

function Invoke-Poll {
    $now = $clock.Elapsed.TotalSeconds
    $reading = $null; $readError = $null
    try { $reading = Read-Health } catch { $readError = $_.Exception.Message }
    $fb = Get-FallbackView
    $script:fallbackText = $fb.text
    if ($fb.ok) { if ($null -eq $script:fallbackSince) { $script:fallbackSince = $now } } else { $script:fallbackSince = $null }
    # A tester PC has no lab kit: no owner STOP flag, no transition tasks, no kit heartbeat.
    $step = Get-StartConfirmStep -State $state -Reading $reading -Now $now -Stop $false -Kit $null -ReadError $readError
    $summary = "$($step.action): $($step.reason)"
    if ($summary -ne $script:lastLogged -or ($now - $script:lastLoggedAt) -ge 10) {
        Write-StartConfirmLog ("$summary | " + (Format-Reading $reading) + " | $($fb.text)")
        $script:lastLogged = $summary; $script:lastLoggedAt = $now
    }
    if ($step.action -eq 'confirm') {
        $state.last_confirm = $now; $state.confirms = $state.confirms + 1
        try { $answer = Confirm-Health $step.generation $step.epoch; $state.confirmed_here = $true; Write-StartConfirmLog "confirm generation $($step.generation) epoch $($step.epoch): done, $answer" }
        catch { Write-StartConfirmLog "confirm generation $($step.generation) epoch $($step.epoch): refused, $($_.Exception.Message)" }
        $script:nextPoll = $now + 0.5
        return $null
    }
    $script:nextPoll = $now + $pollEvery
    if ($step.action -eq 'exit') {
        if ($step.code -eq 1 -or ($step.code -eq 5 -and $null -eq $reading)) { return (Complete-WithFallback $step) }
        return $step
    }
    return $null
}

# One step of the run: poll when the next poll is due, and answer with the step that ends the run, or $null.
function Invoke-Tick {
    $now = $clock.Elapsed.TotalSeconds
    if ($now -lt $script:nextPoll) { return $null }
    $step = $null
    try { $step = Invoke-Poll }
    catch { Write-StartConfirmLog "poll error: $($_.Exception.Message)"; $script:nextPoll = $now + $pollEvery }
    if (!$step -and $now -ge $Seconds + 10) { $step = [ordered]@{code = 1; reason = 'hard bound: polls kept failing' } }
    return $step
}

# The run of the Local System task: no window, no form, no assembly to load. The sign-in notice of this logon is
# start-window.ps1's, in the user's own session; this loop keeps the time bound of the task.
Write-StartConfirmLog "window: none; the task $windowTaskName of this logon starts the sign-in notice only, and the exit line below reports what that task did"
while ($true) {
    $step = Invoke-Tick
    if ($step) { $script:exitCode = [int]$step.code; $script:lastReason = $step.reason; break }
    Start-Sleep -Milliseconds 250
}
Write-StartConfirmLog "exit $($script:exitCode): $($script:lastReason); $(Format-WindowTask); UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts'); $(Format-Dpm); confirm calls $($state.confirms); start record: $(Format-StartRecord -Record (Read-StartRecord) -BootUtc $script:bootUtc)"
# The running-release witness of this boot: the loaded KMD image and its reply against the installed manifest. Written
# whatever the confirmation's outcome (it names what runs, not how healthy it is); never fails the task. It takes the
# installer's engine.lock for the whole reading and publication (up to 3 s wait): while an installer runs, no witness.
try {
    $why = Write-RunningReleaseWitness -InstallRoot (Split-Path $here) -RecordedBy 'start-confirm'
    Write-StartConfirmLog $(if ($why) { "running-release witness: not written, $why" } else { "running-release witness: written, $($script:WitnessPath)" })
} catch { Write-StartConfirmLog "running-release witness: $($_.Exception.Message)" }
exit $script:exitCode
