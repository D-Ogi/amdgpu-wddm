# Logon start-confirm of a tester PC: the scheduled task "amdgpu-wddm start confirm" (install.ps1 registers it: at
# logon of an administrator, highest privileges) runs this once per logon. The release variant of the lab's
# start-confirm.ps1; the decision rules are the lab's, unchanged, in start-confirm-core.ps1.
#
# Why it exists: every driver start counts against the KMD's boot-loop guard (guard.c GuardCheckAndCountStart,
# BC250_MAX_UNCONFIRMED_STARTS 2): at the third start nobody confirmed, the driver refuses to start and Windows uses
# Microsoft Basic Display Adapter. A DPM start has a guard of its own (dpm.c, DpmPending): the next start after an
# unconfirmed DPM start runs at the fixed floor clock and writes DpmMode 0. The KMD's start-health confirmation
# (start_health.c StartHealthRequest, BC250_ESCAPE_RUN_START_HEALTH) clears both, and the KMD accepts it only when
# the start has been ready and visible for 60 s with fresh presentation completions. An idle desktop presents too
# rarely for that, so this script shows a small top-most window whose text changes every 250 ms, keeps DWM
# presenting, and confirms the start once the KMD's milestone is met. Then it exits: at health flags 15, or after
# -Seconds (120). Nothing stays resident.
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
param([int]$Seconds = 120, [switch]$Probe, [string]$ExpectedVersion = '', [int]$FallbackHoldSeconds = 60)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# The driver version the fallback expects: manifest.json kmd_abi of the installed release (the install root is the
# parent of tools\).
if (-not $ExpectedVersion) {
    try { $ExpectedVersion = [string](Get-Content -LiteralPath (Join-Path (Split-Path $here) 'manifest.json') -Raw | ConvertFrom-Json).kmd_abi } catch { }
    if (-not $ExpectedVersion) { $ExpectedVersion = '0x000700D7' }   # this release's kmd_abi; test-dryrun.ps1 gates it
}
. (Join-Path $here 'start-confirm-core.ps1')
$clock = [Diagnostics.Stopwatch]::StartNew()
$cli = Join-Path $here 'bc250kmd_cli.exe'
$dll = [IO.Path]::GetFullPath((Join-Path $here 'bc250control.dll'))
$parametersKey = 'SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$log = Join-Path $env:ProgramData 'amdgpu-wddm\start-confirm.log'
$script:exitCode = 1
$script:lastReason = $null

if (-not $Probe) {
    try {
        [void][IO.Directory]::CreateDirectory((Split-Path $log))
        if (Test-Path -LiteralPath $log) { $keep = @(Get-Content -LiteralPath $log -Tail 200); [IO.File]::WriteAllLines($log, [string[]]$keep) }
    } catch { }
}
function Write-StartConfirmLog([string]$Text) {
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $line = [string]::Format($inv, '{0} +{1,6:F1}s {2}', [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', $inv), $clock.Elapsed.TotalSeconds, $Text)
    if ($Probe) { $line; return }
    try { [IO.File]::AppendAllText($log, $line + "`r`n") } catch { }
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
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($parametersKey)
    if (!$k) { return 'no key' }
    try { $v = $k.GetValue($Name, $null); if ($null -eq $v) { return 'absent' }; return [string]$v } finally { $k.Dispose() }
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

if ($Probe) {
    Write-StartConfirmLog "probe: UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts')"
    Write-StartConfirmLog "probe: $(Format-Dpm)"
    Write-StartConfirmLog ('probe: fallback view: ' + (Get-FallbackView).text)
    try { Write-StartConfirmLog ('probe: start health ' + (Format-Reading (Read-Health))); exit 0 }
    catch { Write-StartConfirmLog "probe: start health no reading: $($_.Exception.Message)"; exit 5 }
}

Write-StartConfirmLog "start: bound $Seconds s, UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts'), $(Format-Dpm)"
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

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$form = New-Object Windows.Forms.Form
$form.FormBorderStyle = 'None'; $form.TopMost = $true; $form.ShowInTaskbar = $false; $form.ShowIcon = $false
$form.StartPosition = 'Manual'; $form.Size = New-Object Drawing.Size(250, 22); $form.Location = New-Object Drawing.Point(8, 34)
$form.BackColor = [Drawing.Color]::Navy
$label = New-Object Windows.Forms.Label
$label.Dock = 'Fill'; $label.ForeColor = [Drawing.Color]::Cyan; $label.Font = New-Object Drawing.Font('Consolas', 9)
$form.Controls.Add($label)
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({
    $now = $clock.Elapsed.TotalSeconds
    $label.Text = 'amdgpu-wddm start check ' + [DateTime]::Now.ToString('HH:mm:ss.f')
    if ($now -lt $script:nextPoll) { return }
    $step = $null
    try { $step = Invoke-Poll }
    catch { Write-StartConfirmLog "poll error: $($_.Exception.Message)"; $script:nextPoll = $now + $pollEvery }
    if (!$step -and $now -ge $Seconds + 10) { $step = [ordered]@{code = 1; reason = 'hard bound: polls kept failing' } }
    if ($step) { $script:exitCode = [int]$step.code; $script:lastReason = $step.reason; $timer.Stop(); $form.Close() }
})
$timer.Start()
[void]$form.ShowDialog()
$timer.Dispose(); $form.Dispose()
Write-StartConfirmLog "exit $($script:exitCode): $($script:lastReason); UnconfirmedStarts $(Read-Parameter 'UnconfirmedStarts'); $(Format-Dpm); confirm calls $($state.confirms)"
# The running-release witness of this boot: the loaded KMD image and its reply against the installed manifest. Written
# whatever the confirmation's outcome (it names what runs, not how healthy it is); never fails the task. It takes the
# installer's engine.lock for the whole reading and publication (up to 3 s wait): while an installer runs, no witness.
try {
    . (Join-Path $here 'release-witness.ps1')
    $why = Write-RunningReleaseWitness -InstallRoot (Split-Path $here) -RecordedBy 'start-confirm'
    Write-StartConfirmLog $(if ($why) { "running-release witness: not written, $why" } else { "running-release witness: written, $($script:WitnessPath)" })
} catch { Write-StartConfirmLog "running-release witness: $($_.Exception.Message)" }
exit $script:exitCode
