# Logon start-confirm of a tester PC: the scheduled task "amdgpu-wddm start confirm" (install.ps1 registers it: at
# logon of an administrator, highest privileges) runs this once per logon. The release variant of the lab's
# start-confirm.ps1; the decision rules are the lab's, unchanged, in start-confirm-core.ps1.
#
# Why it exists: every driver start counts against the KMD's boot-loop guard (guard.c, BC250_MAX_UNCONFIRMED_STARTS
# 2). Only a confirm escape from an administrator clears the count, and the KMD accepts it only when the start has
# been ready for 60 s with fresh GPU work. An idle desktop presents too rarely for that, so this script shows a
# small top-most window whose text changes every 250 ms, keeps DWM presenting, and confirms the start once the KMD's
# milestone is met. Then it exits: at health flags 15, or after -Seconds (120). Nothing stays resident.
# Without a confirmation, the third start falls back to Microsoft Basic Display Adapter, and an unconfirmed DPM or
# CU request falls back to the driver default.
#
# Health reads and the confirm go through bc250kmd_cli.exe next to this script. The only file it writes is a short
# status log, %ProgramData%\amdgpu-wddm\start-confirm.log, cut to its last 200 lines at each run.
param([int]$Seconds = 120, [switch]$Probe)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here 'start-confirm-core.ps1')
$clock = [Diagnostics.Stopwatch]::StartNew()
$cli = Join-Path $here 'bc250kmd_cli.exe'
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
function Read-UnconfirmedStarts {
    $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($parametersKey)
    if (!$k) { return 'no key' }
    try { $v = $k.GetValue('UnconfirmedStarts', $null); if ($null -eq $v) { return 'absent' }; return [string]$v } finally { $k.Dispose() }
}
function Read-Health {
    $text = & $cli health read 2>&1 | Out-String
    $r = ConvertFrom-StartConfirmCliHealth $text
    if (!$r) { throw "health read unreadable: $($text.Trim())" }
    return $r
}
function Confirm-Health([uint64]$Generation, [uint64]$Epoch) {
    $text = & $cli health confirm $Generation $Epoch 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "confirm exit $($LASTEXITCODE): $($text.Trim())" }
    return $text.Trim()
}
function Format-Reading($r) { if (!$r) { return 'none' }; return "flags $($r.flags) generation $($r.generation) epoch $($r.epoch) completed $($r.completed) age_ms $($r.age_ms) ready_ms $($r.ready_ms)" }

if ($Probe) {
    Write-StartConfirmLog "probe: UnconfirmedStarts $(Read-UnconfirmedStarts)"
    try { Write-StartConfirmLog ('probe: ' + (Format-Reading (Read-Health))); exit 0 }
    catch { Write-StartConfirmLog "probe: no reading: $($_.Exception.Message)"; exit 5 }
}

Write-StartConfirmLog "start: bound $Seconds s, UnconfirmedStarts $(Read-UnconfirmedStarts)"
$state = New-StartConfirmState $Seconds
$pollEvery = 5.0
$script:nextPoll = 0.0
$script:lastLogged = ''
$script:lastLoggedAt = -100.0

function Invoke-Poll {
    $now = $clock.Elapsed.TotalSeconds
    $reading = $null; $readError = $null
    try { $reading = Read-Health } catch { $readError = $_.Exception.Message }
    # A tester PC has no lab kit: no owner STOP flag, no transition tasks, no kit heartbeat.
    $step = Get-StartConfirmStep -State $state -Reading $reading -Now $now -Stop $false -Kit $null -ReadError $readError
    $summary = "$($step.action): $($step.reason)"
    if ($summary -ne $script:lastLogged -or ($now - $script:lastLoggedAt) -ge 10) {
        Write-StartConfirmLog ("$summary | " + (Format-Reading $reading))
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
    if ($step.action -eq 'exit') { return $step }
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
Write-StartConfirmLog "exit $($script:exitCode): $($script:lastReason); UnconfirmedStarts $(Read-UnconfirmedStarts); confirm calls $($state.confirms)"
exit $script:exitCode
