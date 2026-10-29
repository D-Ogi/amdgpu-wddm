# BD-187 (GitHub #43): Task Manager with "Always on top" on, then the screen flashes and goes black. This trial turns
# that option on and off on the lab, first over the plain desktop (arm A) and then over a borderless D3D12 flip client
# (arm B), and reads the kernel driver's flip counters around each step. Windows PowerShell 5.1, over ssh (elevated,
# session 0), bounded to 170 s:
#
#   python tools\win\target.py push tools\win\topmost-flip\topmost-flip.ps1 tools\win\topmost-flip\topmost-helper.ps1 <flip client> --to C:\BC250\tmp\topmost-kit
#   python tools\win\target.py ps tools\win\topmost-flip\topmost-flip.ps1 -Kit C:\BC250\tmp\topmost-kit -FlipClient C:\BC250\tmp\topmost-kit\amdgpu_wddm_d3d12_queue.exe
#
#   -Kit         the folder with topmost-helper.ps1 (and the flip client, when -FlipClient names one there)
#   -Cli         bc250kmd_cli.exe (default: the install's tools folder); `log summary` is read after every step
#   -FlipClient  amdgpu_wddm_d3d12_queue.exe built with `build.ps1 -Flip` (tools\win\d3d12queue). Without it arm B
#                is skipped
#   -Format      the client's chain format as its DXGI number: 87 B8G8R8A8 (default, the case in which DWM draws a
#                covering window into the client's buffer, BD-197 x1), 28 R8G8B8A8 (the case DWM composes)
#   -Arms        A,B by default
#   -OnSeconds   how long the option stays on in each arm (default 25); -OffSeconds after it is off again (10)
#   -SettingsName, -ToggleName  the UI Automation names of Task Manager's Settings page and of its check box, in
#                the console user's display language (default en-US)
#   -Plan        print the schedule and exit: no task, no process, no file, on any computer
#   -SelfTest    parse the sample summary below and check the deltas, then exit (host gate; touches nothing)
#
# Steps, with the summary read after each one (s0..s7):
#   s0 baseline; A1 Task Manager started (s1); A2 option on, OnSeconds (s2); A3 option off, OffSeconds (s3);
#   B0 flip client started and presenting, Task Manager behind it, 8 s (s4); B1 option on, OnSeconds (s5);
#   B2 option off, OffSeconds (s6); end: client ends, Task Manager closed with the option off (s7).
# Over the whole run, once a second: the dwm.exe process ids (a new id is a compositor restart, the BD-168 class).
# At the end: Application error events of dwm.exe and Display/LiveKernelEvent events in the System log since s0.
#
# What tells the causes of BD-187 apart (the step deltas in the report):
#   dwm restarts > 0 or dwm.exe error events          -> the compositor crash class (BD-168), not a flip refusal
#   shared-transition delta > 0 with a refused (a, e-refused), writer-busy or e5 delta > 0 in the same step
#                                                    -> a refused transition flip (d3dkmddi.md:12793, a black output)
#   plane-format changes delta > 0 together with immediate delta > 0 in the same step -> the swizzle-at-immediate case
#   visibility false delta > 0                        -> the OS blanked the source (the 10-bit iflip pattern)
#   none of these, picture right on the screen        -> not reproduced on this stack
#
# The script refuses to run when Task Manager or the flip client already runs, or nobody is logged on at the console.
# It stops only what it started, registers two tasks and removes them on every exit path, pauses the overlay's summary
# poll while it runs (the pause file, as scanout-trial.ps1 does), and writes only under -Work. The overlay window stays:
# it covers part of the client, so in arm B the step before the option is already composed or drawn into by DWM (BD-197
# 06.11 and x1); the B0 deltas say which. The results stay in C:\BC250\tmp\topmost\<utc>\ and on standard output.
#
# Exit codes: 0 every step ran, 1 a step failed or did not fit in the bound, 2 refused before any step.
param(
    [string]$Kit = $PSScriptRoot,
    [string]$Helper = (Join-Path $Kit 'topmost-helper.ps1'),
    [string]$Cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe',
    [string]$FlipClient = '',
    [ValidateSet('87', '28')][string]$Format = '87',
    [string]$Arms = 'A,B',
    [ValidateRange(5, 40)][int]$OnSeconds = 25,
    [ValidateRange(3, 30)][int]$OffSeconds = 10,
    [string]$SettingsName = 'Settings',
    [string]$ToggleName = 'Always on top',
    [int]$BoundSeconds = 170,
    [string]$Work = 'C:\BC250\tmp\topmost',
    [string]$OverlayPause = 'C:\BC250\mon\graphics-summary.pause',
    [switch]$Plan,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Continue'
$clock = [Diagnostics.Stopwatch]::StartNew()
$bad = @()

# The summary lines this trial reads, each with its counter names. The newest line of each kind counts: the ring holds
# earlier summaries too. A line the running driver does not print is absent, never zero.
$patterns = [ordered]@{
    flips      = @{ re = 'wddm summary: presents (\d+), flips (\d+) of (\d+) address calls'; names = @('presents', 'flips', 'address_calls') }
    scanout    = @{ re = 'wddm summary: scan-out flips (\d+) of (\d+) requested candidates; admission ok/no-alloc/not-requested (\d+)/(\d+)/(\d+)'
                    names = @('scanout_flips', 'scanout_requests', 'admit_ok', 'admit_no_alloc', 'admit_not_requested') }
    refusals   = @{ re = 'wddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated (\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)'
                    names = @('ref_format', 'ref_geometry', 'ref_pitch', 'ref_size', 'ref_segment', 'ref_align', 'ref_gated') }
    flags      = @{ re = 'wddm summary: flip flags mode/immediate/shared-transition/independent (\d+)/(\d+)/(\d+)/(\d+)'
                    names = @('flag_mode', 'flag_immediate', 'flag_shared_transition', 'flag_independent') }
    formats    = @{ re = 'wddm summary: plane formats (\d+), flips argb8888/abgr8888/abgr2101010 (\d+)/(\d+)/(\d+), changes (\d+) refused (\d+)'
                    names = @('plane_formats', 'fmt_argb8888', 'fmt_abgr8888', 'fmt_abgr2101010', 'fmt_changes', 'fmt_refused') }
    edges      = @{ re = 'wddm summary: flip edges f0 (\d+) f1 (\d+) a (\d+) wv (\d+) wi (\d+) wu (\d+) e (\d+)'
                    names = @('edge_f0', 'edge_f1', 'edge_a', 'edge_wv', 'edge_wi', 'edge_wu', 'edge_e') }
    results    = @{ re = 'wddm summary: flip results of e refused (\d+) ok (\d+) fault (\d+) u (\d+)'
                    names = @('e_refused', 'e_ok', 'e_fault', 'e_u') }
    busy       = @{ re = 'wddm summary: flip writer-busy (\d+) recovery-pending (\d+)'; names = @('writer_busy', 'recovery_pending') }
    e5         = @{ re = 'wddm summary: flip e5 (\d+) faulting (\d+) code 0x([0-9A-F]+)'; names = @('e5', 'faulting', 'fault_code') }
    exclusive  = @{ re = 'exclusive refused (\d+) failed (\d+)'; names = @('exclusive_refused', 'exclusive_failed') }
    visibility = @{ re = 'wddm visibility: true (\d+) false (\d+) failures (\d+)'; names = @('vis_true', 'vis_false', 'vis_failures') }
}
# The counters whose delta names a cause (see the header), printed in every step line.
$keyCounters = @('flips', 'scanout_flips', 'flag_immediate', 'flag_shared_transition', 'flag_independent', 'edge_a',
    'e_refused', 'e_u', 'writer_busy', 'e5', 'fmt_changes', 'fmt_refused', 'vis_false', 'vis_failures')

function ConvertFrom-Summary([string]$Text) {
    $values = [ordered]@{}
    foreach ($key in $patterns.Keys) {
        $all = [regex]::Matches($Text, $patterns[$key].re)
        if ($all.Count -eq 0) { continue }
        $m = $all[$all.Count - 1]
        for ($i = 0; $i -lt $patterns[$key].names.Count; $i++) {
            $raw = $m.Groups[$i + 1].Value
            $values[$patterns[$key].names[$i]] = if ($raw -match '^\d+$') { [int64]$raw } else { $raw }
        }
    }
    return $values
}

# A delta only for a counter both reads carried as a number.
function Get-Delta($Before, $After) {
    $d = [ordered]@{}
    foreach ($name in $After.Keys) {
        if ($Before.Contains($name) -and $Before[$name] -is [int64] -and $After[$name] -is [int64]) { $d[$name] = $After[$name] - $Before[$name] }
    }
    return $d
}

function Format-Delta($Delta) {
    ($keyCounters | ForEach-Object { if ($Delta.Contains($_)) { '{0}={1}' -f $_, $Delta[$_] } else { '{0}=absent' -f $_ } }) -join ' '
}

if ($SelfTest) {
    $one = @'
wddm summary: presents 10, flips 100 of 101 address calls (0 arrived above DISPATCH_LEVEL)
wddm summary: scan-out flips 0 of 0 requested candidates; admission ok/no-alloc/not-requested 90/0/0
wddm summary: flip flags mode/immediate/shared-transition/independent 1/0/0/0, redirected presents 0
wddm summary: flip edges f0 0 f1 0 a 0 wv 0 wi 0 wu 0 e 100
wddm summary: flip results of e refused 0 ok 100 fault 0 u 0
wddm summary: flip e5 0 faulting 0 code 0x000000 node 0 blank 00000000
wddm visibility: true 2 false 0 failures 0 first-true 0x00000000 last-true 0x00000000 QPC 10000000
'@
    $two = $one + @'
wddm summary: presents 12, flips 180 of 183 address calls (0 arrived above DISPATCH_LEVEL)
wddm summary: scan-out flips 40 of 42 requested candidates; admission ok/no-alloc/not-requested 168/0/0
wddm summary: flip flags mode/immediate/shared-transition/independent 1/0/3/1, redirected presents 0
wddm summary: flip edges f0 0 f1 0 a 2 wv 0 wi 0 wu 0 e 178
wddm summary: flip results of e refused 1 ok 177 fault 0 u 0
wddm summary: flip e5 0 faulting 0 code 0x000000 node 0 blank 00000000
wddm visibility: true 3 false 1 failures 0 first-true 0x00000000 last-true 0x00000000 QPC 10000000
'@
    $a = ConvertFrom-Summary $one; $b = ConvertFrom-Summary $two; $d = Get-Delta $a $b
    $checks = @(
        ($d['flips'] -eq 80), ($d['scanout_flips'] -eq 40), ($d['flag_shared_transition'] -eq 3), ($d['edge_a'] -eq 2),
        ($d['e_refused'] -eq 1), ($d['vis_false'] -eq 1), (-not $d.Contains('writer_busy')), ($b['fault_code'] -eq '000000'),
        ((Format-Delta $d) -match 'writer_busy=absent'))
    $failed = @($checks | Where-Object { -not $_ }).Count
    "selftest: $($checks.Count - $failed) of $($checks.Count) checks pass"
    exit $(if ($failed) { 1 } else { 0 })
}

$armList = @($Arms.Split(',') | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ })
if (-not $FlipClient) { $armList = @($armList | Where-Object { $_ -ne 'B' }) }
if ($Plan) {
    $t = 3
    'plan: s0 summary at ~0 s'
    if ($armList -contains 'A') {
        "plan: A1 start Task Manager (task, highest run level, console user), s1 at ~$($t += 8; $t) s"
        "plan: A2 option on for $OnSeconds s, s2 at ~$($t += $OnSeconds + 2; $t) s"
        "plan: A3 option off for $OffSeconds s, s3 at ~$($t += $OffSeconds + 2; $t) s"
    }
    if ($armList -contains 'B') {
        "plan: B0 flip client $FlipClient format $Format (task, limited), 8 s, s4 at ~$($t += 12; $t) s"
        "plan: B1 option on for $OnSeconds s, s5 at ~$($t += $OnSeconds + 2; $t) s"
        "plan: B2 option off for $OffSeconds s, s6 at ~$($t += $OffSeconds + 2; $t) s"
    }
    "plan: end, Task Manager closed with the option off, s7 at ~$($t += 8; $t) s (bound $BoundSeconds s)"
    'plan: nothing was started, registered or written'
    exit 0
}

$helperTask = 'BC250 topmost helper'
$clientTask = 'BC250 topmost client'
$utc = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$dir = Join-Path $Work $utc
$snapshots = [ordered]@{}
$steps = @()
$dwmSeen = [ordered]@{}
$pausedByUs = $false
$started = [DateTime]::Now
$cmdNumber = 0

function Remove-Tasks {
    foreach ($t in $helperTask, $clientTask) {
        if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) {
            Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue
        }
    }
}

function Read-Summary([string]$Label) {
    $text = & $Cli log summary 2>&1 | Out-String
    Set-Content -LiteralPath (Join-Path $dir "summary-$Label.txt") -Value $text -Encoding ascii
    if ($LASTEXITCODE -ne 0) { $script:bad += "${Label}: bc250kmd_cli log summary exit $LASTEXITCODE" }
    $script:snapshots[$Label] = ConvertFrom-Summary $text
}

function Watch-Dwm([int]$Seconds) {
    $end = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        foreach ($p in @(Get-Process -Name 'dwm' -ErrorAction SilentlyContinue)) {
            if (-not $script:dwmSeen.Contains([string]$p.Id)) { $script:dwmSeen[[string]$p.Id] = [DateTime]::UtcNow.ToString('o') }
        }
        Start-Sleep -Milliseconds 1000
    } while ([DateTime]::UtcNow -lt $end)
}

function Send-Helper([string]$Word) {
    $script:cmdNumber++
    $n = $script:cmdNumber
    Set-Content -LiteralPath (Join-Path $dir "cmd-$n.txt") -Value $Word -Encoding ascii
    $result = Join-Path $dir "result-$n.json"
    $until = [DateTime]::UtcNow.AddSeconds(12)
    while (-not (Test-Path -LiteralPath $result) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 200 }
    if (-not (Test-Path -LiteralPath $result)) { $script:bad += "helper: no answer to '$Word' in 12 s"; return $null }
    return Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
}

function Step([string]$Name, [string]$From, [string]$To, $Answer) {
    $d = Get-Delta $script:snapshots[$From] $script:snapshots[$To]
    $how = if ($Answer) { " method=$($Answer.method) option=$($Answer.option) topmost=$($Answer.topmost)" } else { '' }
    $line = '{0,-3} {1,6:N1}s dwm={2}{3} {4}' -f $Name, $script:clock.Elapsed.TotalSeconds, $script:dwmSeen.Count, $how, (Format-Delta $d)
    $line
    $script:steps += [pscustomobject]@{ step = $Name; from = $From; to = $To; utc = [DateTime]::UtcNow.ToString('o'); answer = $Answer; delta = $d }
}

$code = 0
try {
    foreach ($f in $Helper, $Cli) { if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "refused: no file '$f'" } }
    if ($FlipClient -and -not (Test-Path -LiteralPath $FlipClient -PathType Leaf)) { throw "refused: no file '$FlipClient'" }
    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $explorer) { throw 'refused: no explorer.exe, so nobody is logged on at the console' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    $user = '{0}\{1}' -f $owner.Domain, $owner.User
    if (Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue) { throw 'refused: Task Manager runs; this script closes only a Task Manager it started' }
    if ($FlipClient -and (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FlipClient)) -ErrorAction SilentlyContinue)) {
        throw 'refused: the flip client already runs'
    }
    foreach ($t in $helperTask, $clientTask) { if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) { throw "refused: task '$t' exists" } }
    [void][IO.Directory]::CreateDirectory($dir)
    Copy-Item -LiteralPath $Helper -Destination (Join-Path $dir 'topmost-helper.ps1')
    "console user $user session $($explorer.SessionId), work $dir, arms $($armList -join ',')"
    if ($OverlayPause -and (Test-Path -LiteralPath (Split-Path -Parent $OverlayPause)) -and -not (Test-Path -LiteralPath $OverlayPause)) {
        Set-Content -LiteralPath $OverlayPause -Value "topmost-flip $utc" -Encoding ascii
        $pausedByUs = $true
    }
} catch {
    "$($_.Exception.Message)"
    exit 2
}

try {
    Read-Summary 's0'
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $helperArgs = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -Dir "{1}" -SettingsName "{2}" -ToggleName "{3}" -Seconds {4}' -f `
        (Join-Path $dir 'topmost-helper.ps1'), $dir, $SettingsName, $ToggleName, ($BoundSeconds - 10)
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds $BoundSeconds) -MultipleInstances IgnoreNew `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $helperTask -Settings $settings `
        -Action (New-ScheduledTaskAction -Execute $ps -Argument $helperArgs -WorkingDirectory $dir) `
        -Principal (New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest) | Out-Null
    Start-ScheduledTask -TaskName $helperTask
    $ready = Join-Path $dir 'ready.json'
    $until = [DateTime]::UtcNow.AddSeconds(25)
    while (-not (Test-Path -LiteralPath $ready) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 250 }
    $readyValue = if (Test-Path -LiteralPath $ready) { Get-Content -LiteralPath $ready -Raw | ConvertFrom-Json } else { $null }
    if (-not $readyValue -or -not $readyValue.ok) { throw "helper not ready: $(if ($readyValue) { $readyValue.reason } else { 'no answer in 25 s' })" }
    "taskmgr pid $($readyValue.pid) window $($readyValue.window) topmost $($readyValue.topmost)"
    Watch-Dwm 3
    Read-Summary 's1'
    Step 'A1' 's0' 's1' $null

    if ($armList -contains 'A') {
        $answer = Send-Helper 'on'; Watch-Dwm $OnSeconds; Read-Summary 's2'; Step 'A2' 's1' 's2' $answer
        $answer = Send-Helper 'off'; Watch-Dwm $OffSeconds; Read-Summary 's3'; Step 'A3' 's2' 's3' $answer
    } else { $snapshots['s3'] = $snapshots['s1'] }

    if ($armList -contains 'B') {
        $clientDir = Join-Path $dir 'client'
        [void][IO.Directory]::CreateDirectory($clientDir)
        # Frames for the rest of arm B at about 60 Hz, and the deadline after them; the client reads both from its
        # own environment (tools\win\d3d12queue\README.md).
        $frames = (8 + $OnSeconds + $OffSeconds + 6) * 60
        $deadline = 8 + $OnSeconds + $OffSeconds + 20
        $wrapper = Join-Path $clientDir 'run-client.cmd'
        Set-Content -LiteralPath $wrapper -Encoding ascii -Value @(
            '@echo off', 'set AMDGPU_WDDM_D3D12_EXPERIMENT=', "set AMDGPU_WDDM_D3D12_FLIP_FRAMES=$frames",
            'set AMDGPU_WDDM_D3D12_FLIP_BUFFERS=3', "set AMDGPU_WDDM_D3D12_FLIP_FORMAT=$Format",
            "`"$FlipClient`" --interactive `"$clientDir`" --deadline $deadline > `"$clientDir\client.out`" 2> `"$clientDir\client.err`"",
            "echo client_exit %ERRORLEVEL%>> `"$clientDir\client.out`"")
        Register-ScheduledTask -TaskName $clientTask -Settings $settings `
            -Action (New-ScheduledTaskAction -Execute $wrapper -WorkingDirectory $clientDir) `
            -Principal (New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited) | Out-Null
        Start-ScheduledTask -TaskName $clientTask
        $controller = Join-Path (Split-Path -Parent $FlipClient) 'controller.ps1'
        # The client's own command protocol (controller.ps1 next to it, as scanout-trial.ps1 drives it). 'copy' is
        # the flip run in a -Flip build and blocks the client until its frames are done, so its receipt is read at
        # the end of the arm, not here.
        $sequence = 0
        foreach ($command in 'create-device', 'create-queue', 'copy') {
            $sequence++
            & $controller -Directory $clientDir -Sequence $sequence -Command $command | Out-Null
            if ($command -eq 'copy') { break }
            $receipt = Join-Path $clientDir ('result-{0:d6}.json' -f $sequence)
            $until = [DateTime]::UtcNow.AddSeconds(15)
            while (-not (Test-Path -LiteralPath $receipt) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 200 }
            if (-not (Test-Path -LiteralPath $receipt)) { throw "flip client: no receipt for $command in 15 s" }
        }
        Watch-Dwm 8; Read-Summary 's4'; Step 'B0' 's3' 's4' $null
        $answer = Send-Helper 'on'; Watch-Dwm $OnSeconds; Read-Summary 's5'; Step 'B1' 's4' 's5' $answer
        $answer = Send-Helper 'off'; Watch-Dwm $OffSeconds; Read-Summary 's6'; Step 'B2' 's5' 's6' $answer
        $receipt = Join-Path $clientDir ('result-{0:d6}.json' -f $sequence)
        $until = [DateTime]::UtcNow.AddSeconds(12)
        while (-not (Test-Path -LiteralPath $receipt) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 250 }
        if (Test-Path -LiteralPath $receipt) {
            $flipReceipt = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
            "flip client: success $($flipReceipt.success) hr $($flipReceipt.hr)"
            & $controller -Directory $clientDir -Sequence ($sequence + 1) -Command 'exit' | Out-Null
        } else { $bad += 'flip client: the flip run gave no receipt in its bound' }
    }
    $answer = Send-Helper 'quit'
    Watch-Dwm 2
    Read-Summary 's7'
    $last = if ($armList -contains 'B') { 's6' } else { 's3' }
    Step 'end' $last 's7' $answer
} catch {
    $bad += "$($_.Exception.Message)"
} finally {
    $until = [DateTime]::UtcNow.AddSeconds(8)
    while ((Get-ScheduledTask -TaskName $helperTask -ErrorAction SilentlyContinue).State -eq 'Running' -and [DateTime]::UtcNow -lt $until) {
        Start-Sleep -Milliseconds 300
    }
    Remove-Tasks
    if ($FlipClient) {
        Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FlipClient)) -ErrorAction SilentlyContinue |
            Where-Object { $_.StartTime -ge $started } | ForEach-Object { Stop-Process -Id $_.Id -Force; $bad += "stopped the flip client pid $($_.Id)" }
    }
    if ($pausedByUs) { Remove-Item -LiteralPath $OverlayPause -ErrorAction SilentlyContinue }
}

# The compositor and display events since s0. Application Error 1000 names the faulting image in its first property.
$dwmErrors = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $started } -ErrorAction SilentlyContinue |
    Where-Object { [string]$_.Properties[0].Value -ieq 'dwm.exe' })
$display = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $started } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -in @('Display', 'Microsoft-Windows-Kernel-LiveDump', 'nvlddmkm', 'amdgpu-wddm', 'BC250') -or $_.Id -eq 4101 })
"dwm processes seen $($dwmSeen.Count) ($(($dwmSeen.Keys) -join ', ')); dwm.exe error events $($dwmErrors.Count); display events $($display.Count)"
foreach ($e in $dwmErrors) { "  dwm error $($e.TimeCreated.ToUniversalTime().ToString('o')) module $($e.Properties[3].Value) exception $($e.Properties[6].Value) offset $($e.Properties[7].Value)" }
foreach ($e in $display) { "  system $($e.TimeCreated.ToUniversalTime().ToString('o')) $($e.ProviderName) $($e.Id)" }
foreach ($b in $bad) { "problem: $b" }
if ($dir -and (Test-Path -LiteralPath $dir)) {
    [pscustomobject]@{ utc = $utc; arms = $armList; format = $Format; steps = $steps; dwm = $dwmSeen; dwm_errors = $dwmErrors.Count
        display_events = $display.Count; problems = $bad; seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1) } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'report.json') -Encoding ascii
    "report $(Join-Path $dir 'report.json')"
}
if ($bad.Count -or $clock.Elapsed.TotalSeconds -gt $BoundSeconds) { exit 1 }
exit 0
