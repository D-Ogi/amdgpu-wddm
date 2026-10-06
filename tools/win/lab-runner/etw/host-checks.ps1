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

$scripts = @('etw-capture.ps1', 'etw-start.ps1', 'etw-closure.ps1')

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
                          '-PresentMode', '-SchedulerStacks')) {
        if ($declared -notcontains $switch.Substring(1)) { throw "etw-capture.ps1 has no $switch" }
    }
    # The task's argument line is built on the lines that assign $fps and on the action line, after its -File.
    # Every -Switch spelled in that text must be a parameter of the capture.
    $argText = ''
    foreach ($line in (Text 'etw-start.ps1') -split "`n") {
        if ($line -match '\$fps\s*\+?=') { $argText += "`n" + $line.Substring($line.IndexOf('=') + 1) }
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
Check 'the capture writes its own notes file next to the trial' {
    Needs 'etw-capture.ps1' @("Join-Path `$Root 'etw-notes.txt'")
    Needs 'etw-start.ps1' @("'etw-notes.txt'")
}

if ($failures) { "HOST-CHECKS FAIL $failures of $checks"; exit 1 }
"HOST-CHECKS PASS $checks checks"
