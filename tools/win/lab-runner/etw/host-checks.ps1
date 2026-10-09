# Host checks for the ETW capture instrument, on the development PC, with no lab involved.
#
#   pwsh -NoProfile -File tools\win\lab-runner\host-checks.ps1
#
# The three scripts run on the lab as SYSTEM, inside a game trial that already holds the GPU. A mistake in them
# costs a trial and can leave an ETW session running on the lab, so the shape of each one is checked here:
#
#   - all three parse (a syntax error would only show up as a failed task on the lab);
#   - the capture takes its deadline from the caller (-NotAfterQpc) and refuses to run without one outside
#     -Smoke, so operator delay cannot push a capture past the trial's own cutoff;
#   - the capture cleans up in finally and exits 1 when a cleanup failed, because a session that outlives the
#     task keeps writing to the lab's disk;
#   - a failed session query is never read as absence (the lesson of the closure receipt);
#   - the start script and the capture agree on the switches the start script passes (-GpuOnly, -SkipA,
#     -SecondsB, -WorldLog, -ReserveSeconds, -Process, -PresentMode, -SchedulerStacks): a switch the capture does
#     not know would be accepted by the task and silently ignored;
#   - the scheduler-stack mode (C49) keeps the one thing it must not break: it changes only PerfView's own
#     collection, leaves the logman -gpu session as the packet-level source, and the start script puts
#     -ReserveSeconds on the task's line exactly once, because two of them would fail the task with the game
#     already running;
#   - the closure receipt keeps its documented exit codes (1 sessions remain, 2 query failed, 3 producer
#     running, 4 a helper could not be joined) and removes the task only after closure holds.
#
# Exit 0 and a PASS line, or a FAIL line per failed check and exit 1.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$checks = 0
$failures = 0
function Check([string]$what, [scriptblock]$body) {
    $script:checks++
    try {
        & $body
        "ok    $what"
    } catch {
        $script:failures++
        "FAIL  $what : $($_.Exception.Message)"
    }
}
function Text([string]$name) {
    $path = Join-Path $here $name
    if (-not (Test-Path -LiteralPath $path)) { throw "missing $name" }
    return (Get-Content -LiteralPath $path -Raw)
}
# Plain substring tests: -like would read the brackets of a type literal as a wildcard class.
function Needs([string]$name, [string[]]$needles) {
    $text = Text $name
    foreach ($needle in $needles) {
        if (-not $text.Contains($needle)) { throw "$name no longer has: $needle" }
    }
}

$scripts = @('etw-capture.ps1', 'etw-start.ps1', 'etw-closure.ps1', 'world-rule.ps1')

Check 'the three scripts parse' {
    foreach ($name in $scripts) {
        $errors = $null
        $null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $here $name), [ref]$null, [ref]$errors)
        if ($errors) { throw "$name : $($errors[0].Message) at line $($errors[0].Extent.StartLineNumber)" }
    }
}
Check 'the capture takes its deadline from the caller and refuses to run without one' {
    Needs 'etw-capture.ps1' @('[long]$NotAfterQpc = 0',
        "if (!`$Smoke) { throw 'NotAfterQpc is required outside -Smoke' }")
}
Check 'the capture cleans up in finally and fails on a failed cleanup' {
    $text = Text 'etw-capture.ps1'
    if ($text -notmatch '(?m)^\s*\}\s*finally\s*\{') { throw 'no finally block' }
    Needs 'etw-capture.ps1' @('$script:cleanupFailed = $true', 'exit ([int]$script:cleanupFailed)')
}
Check 'a failed session query is not read as absence' {
    Needs 'etw-capture.ps1' @('ok = $false')
    Needs 'etw-closure.ps1' @('query failed', 'final query failed')
}
Check 'the start script only passes switches the capture knows' {
    # The capture's own parameter names, from its syntax tree: a name that only appears somewhere in the body
    # would not be a parameter the task can pass.
    $ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'etw-capture.ps1'), [ref]$null, [ref]$null)
    $declared = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
    foreach ($switch in @('-GpuOnly', '-SkipA', '-SecondsB', '-WorldLog', '-ReserveSeconds', '-Process',
                          '-PresentMode', '-SchedulerStacks', '-WorldTelemetry', '-WorldSettleSeconds')) {
        if ($declared -notcontains $switch.Substring(1)) { throw "etw-capture.ps1 has no $switch" }
    }
    # The task's argument line is built on the lines that assign $fps and on the action line, after its -File.
    # Every -Switch spelled in that text must be a parameter of the capture.
    $argText = ''
    foreach ($line in (Text 'etw-start.ps1') -split "`n") {
        if ($line -match '\$(fps|worldArgs)\s*\+?=') { $argText += "`n" + $line.Substring($line.IndexOf('=') + 1) }
        elseif ($line -match 'New-ScheduledTaskAction') {
            $at = $line.IndexOf('-File')
            if ($at -lt 0) { throw 'the action line no longer starts the capture with -File' }
            $argText += "`n" + $line.Substring($at + 5)
        }
    }
    if ($argText -notmatch 'NotAfterQpc') { throw 'the task argument line is no longer built the way this check reads it' }
    foreach ($m in [regex]::Matches($argText, '(?<=[\s"])-(?<n>[A-Z][A-Za-z]+)')) {
        $name = $m.Groups['n'].Value
        if ($declared -notcontains $name) {
            throw "etw-start.ps1 passes -$name, which the capture does not declare"
        }
    }
}
Check 'the start script refuses a trial that cannot hold a capture' {
    Needs 'etw-start.ps1' @('not a game trial stage', 'trial not started (no start.json)',
        's left before the trial deadline', 'capture exists in', 'PerfView hash mismatch')
}
Check 'the closure receipt keeps its exit codes and removes the task last' {
    $text = Text 'etw-closure.ps1'
    Needs 'etw-closure.ps1' @('exit 1', 'exit 2', 'exit 3', 'exit 4', 'Unregister-ScheduledTask', 'producer running')
    $closure = $text.LastIndexOf("'closure established")
    $remove = $text.LastIndexOf('Unregister-ScheduledTask')
    if ($remove -lt 0 -or $closure -lt 0 -or $remove -gt $closure) { throw 'the task is not removed before the closure line' }
}
Check 'the scheduler-stack mode changes only the PerfView collection' {
    $text = Text 'etw-capture.ps1'
    Needs 'etw-capture.ps1' @('[switch]$SchedulerStacks',
        'if ($SchedulerStacks -and !$GpuOnly) {',
        '/StackCompression',
        '/KernelEvents:Process,Thread,ImageLoad,ContextSwitch,Dispatcher,DeferedProcedureCalls,Interrupt',
        '/Providers:`"Microsoft-Windows-DxgKrnl:0x88008001:5:@EventIDsToEnable=$ids;@EventIDStacksToEnable=$stackIds`"',
        "`$ids = '20 238 436 175 176 177 181 17 18 19 22'",
        "`$stackIds = '20 238 436'")
    # PerfView 3.2.8's help: /Providers is "a comma separated list of specifications for providers" and
    # "@EventIDsToEnable - a space separated list of decimal event ID numbers". A comma inside either list would
    # split the spec into providers named "238", "436" and so on; PerfView would then enable one id and no stack
    # filter, which is the one thing this mode exists for, and nothing at run time would say so.
    foreach ($line in @("`$ids = ", "`$stackIds = ")) {
        $value = $text.Substring($text.IndexOf($line) + $line.Length)
        $value = $value.Substring(0, $value.IndexOf("`n"))
        if ($value.Contains(',')) { throw "the event-id list $line is comma separated; PerfView wants spaces" }
        if ($value -notmatch "^'[0-9 ]+'") { throw "the event-id list $line is not a quoted space-separated list" }
    }
    # The spec must reach PerfView as ONE argv element, so it carries its own quotes (neither shell adds them).
    if ($text -notmatch '\$spec\s*=\s*"/Providers:`"') { throw 'the provider spec is not one quoted element' }
    # The expensive kernel groups must not come back with the stacks: /Profile cost 1.6 ms a frame on the lab.
    $at = $text.IndexOf('if ($SchedulerStacks -and !$GpuOnly) {')
    $block = $text.Substring($at)
    $end = $block.IndexOf("`n    }")
    if ($end -lt 0) { throw 'the scheduler-stack branch is not one indented block any more' }
    $block = $block.Substring(0, $end)
    foreach ($group in @('Profile', 'DiskIO', 'MemoryHardFaults', '/ThreadTime')) {
        if ($block.Contains($group)) { throw "the scheduler-stack collection asks for $group again" }
    }
    if ($block.Contains('logman')) { throw 'the scheduler-stack branch touches the logman session' }
    # The packet-level source keeps its own unfiltered provider line, whatever the mode.
    Needs 'etw-capture.ps1' @("@('-p', 'Microsoft-Windows-DxgKrnl', '0xffffffffffffffff', '5')")
}
Check 'the start script passes -ReserveSeconds once and refuses a window the mode cannot use' {
    $text = Text 'etw-start.ps1'
    Needs 'etw-start.ps1' @('[switch]$SchedulerStacks',
        'the staged etw-capture.ps1 has no -SchedulerStacks',
        '-SchedulerStacks needs the CPU window',
        '-SchedulerStacks needs -WorldSeconds N')
    # Exactly one place may put -ReserveSeconds into the task's argument line. Only the lines that build that
    # line are read, so a comment or a refusal message that spells the switch does not count; two of them on the
    # line would fail the task as a duplicate parameter, with the game already running.
    $argLines = @($text -split "`n" | Where-Object { $_ -match '\$fps\s*\+?=' -or $_ -match 'New-ScheduledTaskAction' })
    $inArgs = ([regex]::Matches(($argLines -join "`n"), '-ReserveSeconds')).Count
    if ($inArgs -ne 1) { throw "the task line carries -ReserveSeconds $inArgs times, not once" }
}
# The world rule of BD-107. Its numbers are the lab's: the Witcher 3 Remaster main menu sits on the 1000 MHz DPM
# floor at 0-40 % busy (it presents at the 240 fps cap), and the world holds 1100-1500 MHz at 93-100 %. The three
# sampler lines below are recorded ones, from trials 520 (menu) and 519 (world) of the b26 validation, plus the
# legacy fixed-clock form of the same sampler. The whole four-arm replay is scratch\bd107\replay-b26.ps1.
. (Join-Path $here 'world-rule.ps1')
function Band([datetime]$start, [int]$count, [int]$mhz, [double]$busy) {
    # $count one-second sampler lines, oldest first, ending at $start.
    # Invariant formatting: this PC may run a culture whose decimal separator is a comma, and the sampler the
    # lab writes is invariant ("busy 13.3%").
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $lines = @()
    for ($i = $count - 1; $i -ge 0; $i--) {
        $lines += , ($start.AddSeconds(-$i).ToString('HH:mm:ss.fff', $inv) + ' dpm ' + $mhz + ' MHz  919 mV (SMU ' +
            $mhz + ' MHz VID 100)  78.1 C busy ' + $busy.ToString('0.0', $inv) + '% avg  99.2% want 1900 cap 1500 max 1500')
    }
    return ($lines -join "`n")
}
Check 'the world rule reads the lab sampler lines' {
    $now = [datetime]'2026-10-09T16:31:09'
    $text = @(
        '16:57:58.123 dpm 1000 MHz  820 mV (SMU 1000 MHz VID 116)  68.8 C busy  26.7% avg  16.1% want 1000 cap 1500 max 1500',
        '16:31:09.049 dpm 1500 MHz  919 mV (SMU 1500 MHz VID 100)  79.9 C busy 100.0% avg  99.9% want 1900 cap 1500 max 1500',
        '16:31:10.055 fixed 1000 MHz  820 mV (SMU 1000 MHz VID 116)  70.1 C busy  13.3% avg  17.2% want 1000 cap 1500 max 1500',
        'smu metrics: ok, 4498 tables, 1 failures, last 560 ms ago: gfx 818 mV 1000 MHz 62.25 C') -join "`n"
    $s = Get-WorldSamples -Text $text -Reference $now
    if ($s.Count -ne 3) { throw "read $($s.Count) samples of 3" }
    # Ascending by time: the world line, the legacy fixed line, then the menu line of 16:57:58.
    if ($s[0].mhz -ne 1500 -or $s[0].busy -ne 100.0) { throw "the world line read $($s[0].mhz) MHz busy $($s[0].busy)" }
    if ($s[1].mhz -ne 1000 -or $s[1].busy -ne 13.3) { throw 'the legacy fixed line is no longer read' }
    if ($s[2].busy -ne 26.7) { throw "the menu line read busy $($s[2].busy)" }
    # The lines carry no date. A session that runs across midnight reads a late time of day against an early
    # reference as the day before, so the samples stay in order.
    $after = [datetime]'2026-10-10T02:00:00'
    $late = Get-WorldSamples -Text '23:50:00.000 dpm 1500 MHz  919 mV (SMU 1500 MHz VID 100)  78.1 C busy 100.0% avg' -Reference $after
    if ($late[0].time -ne [datetime]'2026-10-09T23:50:00') { throw "a late time of day read as $($late[0].time) against a 02:00 reference" }
}
Check 'the world rule admits the world band and refuses the W3 main menu' {
    $now = [datetime]'2026-10-09T16:31:09'
    $world = Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 40 1500 100.0) -Reference $now) -Now $now
    if (!$world.world) { throw "the world band was refused: $($world.why)" }
    $menu = Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 40 1000 26.7) -Reference $now) -Now $now
    if ($menu.world) { throw 'the main menu band passed as the world' }
    if ($menu.why -notlike 'menu band*') { throw "the menu band gave the wrong reason: $($menu.why)" }
    # A menu that reaches 40 % busy on the floor is still the menu, and the world at 93 % is still the world.
    if ((Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 40 1000 40.0) -Reference $now) -Now $now).world) { throw '1000 MHz at 40 % busy passed as the world' }
    if (!(Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 40 1100 93.1) -Reference $now) -Now $now).world) { throw '1100 MHz at 93 % busy was refused' }
}
Check 'the world rule waits out the settle time and refuses stale or absent telemetry' {
    $now = [datetime]'2026-10-09T16:31:09'
    $short = Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 10 1500 100.0) -Reference $now) -Now $now
    if ($short.world) { throw 'a band of 10 s passed the 25 s settle' }
    if ($short.why -notlike 'band held*') { throw "the settle refusal gave the wrong reason: $($short.why)" }
    $few = Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now 4 1500 100.0) -Reference $now) -Now $now -SettleSeconds 0
    if ($few.world) { throw '4 samples passed the 8-sample run' }
    $stale = Test-WorldSignal -Samples (Get-WorldSamples -Text (Band $now.AddSeconds(-60) 40 1500 100.0) -Reference $now) -Now $now
    if ($stale.world -or $stale.why -notlike 'telemetry stale*') { throw "a sampler that stopped a minute ago gave: $($stale.why)" }
    $none = Test-WorldSignal -Samples @() -Now $now
    if ($none.world -or $none.telemetry -or $none.why -ne 'no telemetry') { throw "no samples gave: $($none.why)" }
    $missing = Test-WorldTelemetry -Path (Join-Path $env:TEMP ('bc250-no-such-sampler-' + [guid]::NewGuid().ToString('N') + '.txt')) -Now $now
    if ($missing.world -or $missing.telemetry -or $missing.why -ne 'no telemetry file') { throw "a missing sampler file gave: $($missing.why)" }
}
Check 'the held band resets across a sampler gap' {
    # H2 of the 2026-10-10 audit, with the fixture that found it: seven world samples from five minutes ago and
    # one fresh world sample returned world=true with band_seconds=300, because the loop counted samples and
    # never looked at the time between them. Nothing was observed in those five minutes.
    $now = [datetime]'2026-10-09T16:31:09'
    $gapped = (Band $now.AddSeconds(-294) 7 1500 99.0) + "`n" + (Band $now 1 1500 99.0)
    $r = Test-WorldSignal -Samples (Get-WorldSamples -Text $gapped -Reference $now) -Now $now
    if ($r.world) { throw "a band with a 294 s hole passed as the world: $($r.why)" }
    if ($r.run -ne 1 -or $r.gap_seconds -ne 294.0) { throw "the gap was not read: run $($r.run), gap $($r.gap_seconds) s" }
    if ($r.why -notlike '*after a 294.0 s sampler gap') { throw "the refusal does not name the gap: $($r.why)" }
    # A sampler that stopped and came back: the band after the gap is judged on its own, and a long enough one
    # is still the world.
    $resumed = (Band $now.AddSeconds(-120) 20 1500 99.0) + "`n" + (Band $now 40 1500 99.0)
    $r2 = Test-WorldSignal -Samples (Get-WorldSamples -Text $resumed -Reference $now) -Now $now
    if (!$r2.world) { throw "a 40-sample band after the gap was refused: $($r2.why)" }
    if ($r2.run -ne 40 -or $r2.gap_seconds -ne 81.0) { throw "the resumed band read run $($r2.run), gap $($r2.gap_seconds) s" }
    # And a single missed sample is not a gap: the sampler's own interval may slip.
    $lines = @((Band $now 40 1500 99.0) -split "`n")
    $hole = (($lines[0..19] + $lines[21..39]) -join "`n")
    $r3 = Test-WorldSignal -Samples (Get-WorldSamples -Text $hole -Reference $now) -Now $now
    if (!$r3.world) { throw "one missed sample ended the band: $($r3.why)" }
}
Check 'the walk marker says whether anything verified the world' {
    # H1 of the 2026-10-10 audit: the automatic walk of a session without telemetry wrote the same marker as a
    # measured world, and the capture read it as a world. Producer and consumer are both here.
    $good = New-WorldMarker -Seconds 42 -Why 'world band 30.0 s, 1500 MHz busy 100.0%' -Verified
    if ($good -ne 'walk 42s: start (world verified: world band 30.0 s, 1500 MHz busy 100.0%)') { throw "the verified marker reads: $good" }
    $bad = New-WorldMarker -Seconds 50 -Why 'no telemetry (the 25 s rule)'
    if ($bad -ne 'walk 50s: start (world unverified: no telemetry the 25 s rule)') { throw "the unverified marker reads: $bad" }
    $log = "12:00:00 window 1s`n" + $bad + "`n12:00:51 walk 51s: W 3 s -> 2"
    $m = Find-WorldMarker -Text $log
    if ($m.verified) { throw "an unverified marker was read as the world: $($m.verified)" }
    if ($m.unverified -ne $bad) { throw "the unverified marker was not found: $($m.unverified)" }
    # The operator marks the world later in the same session: the verified marker is then found.
    $mark = New-WorldMarker -Seconds 70 -Why 'operator mark' -Verified
    $m2 = Find-WorldMarker -Text ($log + "`n" + $mark)
    if ($m2.verified -ne $mark) { throw "the operator mark was not found: $($m2.verified)" }
    # A runtime older than this grammar writes a bare marker; it carries no world evidence either.
    $m3 = Find-WorldMarker -Text '12:00:50 walk 50s: start'
    if ($m3.verified -or $m3.unverified -notlike 'walk 50s: start (no world state*') { throw "a bare marker gave: $($m3.unverified)" }
    if ((Find-WorldMarker -Text '').verified -or (Find-WorldMarker -Text 'nothing here').unverified) { throw 'a log without a marker is not empty' }
}
Check 'the capture opens window B only on a verified walk marker' {
    Needs 'etw-capture.ps1' @('$mark = if ($script:worldRule) { Find-WorldMarker -Text $text }',
        'if ($mark.verified) { Start-B (''world: '' + $mark.verified); break }',
        'walk marker not verified, window B holds')
    $text = Text 'etw-capture.ps1'
    if ($text -match "regex\]::Match\(\`$text, 'walk \[0-9\]\+s: start'\)") { throw 'the capture still reads any walk marker as the world' }
}
Check 'the runner writes the marker in the grammar of the rule' {
    # The game runtime is the producer. The copy in this repository has no telemetry rule, so its automatic walk
    # and its input-command cue may only write the unverified form; the operator's note:world is the one world it
    # can verify. The template kit keeps its own copy of the runner and the same grammar.
    $runner = Join-Path (Split-Path -Parent $here) 'game-runtime.ps1'
    if (!(Test-Path -LiteralPath $runner)) { throw 'game-runtime.ps1 is no longer next to the capture' }
    $text = Get-Content -LiteralPath $runner -Raw
    foreach ($m in [regex]::Matches($text, "'walk '\+\`$t\+'s: start[^']*'")) {
        $line = $m.Value
        if ($line -eq "'walk '+`$t+'s: start ('") { continue }   # the marker is built from a reason variable
        if ($line -notmatch 'world (un)?verified: ') { throw "a marker without a world state: $line" }
    }
    if (-not $text.Contains('world verified: operator mark')) { throw 'the operator mark is no longer a verified world' }
    if (-not $text.Contains('s: start (world unverified: no telemetry rule in this runner')) {
        throw 'the automatic walk no longer writes an unverified marker'
    }
}
Check 'the time bound no longer opens window B by itself' {
    Needs 'etw-capture.ps1' @('[string]$WorldTelemetry', '[int]$WorldSettleSeconds = 25',
        'if ($w.world) { Start-B (''time, world: '' + $w.why); break }',
        'if (!$w.telemetry) { Start-B (''time, world unverified: '' + $w.why); break }',
        'window B held at the time bound')
    # The only Start-B on the time path is inside the world decision: a bare one would be the old defect back.
    $text = Text 'etw-capture.ps1'
    $at = $text.IndexOf('if ($el -ge $LatestB)')
    if ($at -lt 0) { throw 'the time bound is no longer read the way this check reads it' }
    $block = $text.Substring($at)
    $block = $block.Substring(0, $block.IndexOf('Start-Sleep -Seconds 2'))
    if ($block -match "Start-B\s+'time'") { throw 'the time bound still opens window B unconditionally' }
}
Check 'the capture loads the world rule at script scope, so every poll can ask it' {
    # The trap this check exists for. A dot-source inside a function runs in that FUNCTION's scope, so the
    # functions it defines are gone when the function returns. The world rule was first loaded that way, lazily,
    # from inside World-Now: the first poll answered, every later one threw CommandNotFoundException, the catch
    # turned that into telemetry=$false, and the time bound opened window B unverified two seconds after it had
    # correctly held it - BD-107 again. The trap is demonstrated here, so that nobody has to take it on trust.
    $probe = Join-Path ([IO.Path]::GetTempPath()) ('bc250-scope-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $probe
    try {
        Set-Content -LiteralPath (Join-Path $probe 'r.ps1') -Value 'function Probe-Thing { return 1 }'
        $lazy = {
            param($dir)
            function Load { . (Join-Path $dir 'r.ps1'); return (Probe-Thing) }
            $first = Load
            $second = try { Probe-Thing } catch { 'gone' }
            return @($first, $second)
        }
        $r = & $lazy $probe
        if ($r[1] -ne 'gone') { throw 'a dot-source inside a function now survives it; this check needs rewriting' }
    } finally { Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue }

    # And the capture must not be written that way: no dot-source of the rule inside any of its functions, and
    # exactly one at the script's own level.
    $ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'etw-capture.ps1'), [ref]$null, [ref]$null)
    # Every dot-source in the capture, whatever it names its path variable: the capture has exactly one, the rule.
    $dots = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and
        $n.InvocationOperator -eq 'Dot' }, $true))
    if ($dots.Count -ne 1) { throw "the capture has $($dots.Count) dot-sources, not the one that loads the rule" }
    foreach ($fn in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true))) {
        if ($fn.Extent.StartOffset -le $dots[0].Extent.StartOffset -and $fn.Extent.EndOffset -ge $dots[0].Extent.EndOffset) {
            throw "the world rule is dot-sourced inside $($fn.Name): its functions would be gone after the first poll"
        }
    }
    # The rule is reached through the script-scope flag, and the "not staged" note is still written only once.
    Needs 'etw-capture.ps1' @('$script:worldRule = [bool](Test-Path -LiteralPath $script:worldRulePath)',
        'if (!$script:worldRule) {', '$script:worldRuleNoted')
}
Check 'the start script stages the world rule with the trial sampler' {
    Needs 'etw-start.ps1' @('world-rule.ps1 not staged', '-NoWorldRule',
        "Copy-Item -LiteralPath `$rule -Destination (Join-Path `$root 'world-rule.ps1')",
        'the staged etw-capture.ps1 has no -WorldTelemetry')
    if ((Text 'etw-start.ps1') -notmatch '-WorldTelemetry\s+`"C:\\BC250\\tmp\\dpm-\$Trial\.txt`"') {
        throw 'the task line no longer names the trial DPM sampler'
    }
}
Check 'the capture writes its own notes file next to the trial' {
    Needs 'etw-capture.ps1' @("Join-Path `$Root 'etw-notes.txt'")
    Needs 'etw-start.ps1' @("'etw-notes.txt'")
}

if ($failures) { "HOST-CHECKS FAIL $failures of $checks"; exit 1 }
"HOST-CHECKS PASS $checks checks"
