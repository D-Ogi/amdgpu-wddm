# Offline dry-run of the task's argument line, on the development PC, with no ETW session started and no lab.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File dryrun-args.ps1
#
# etw-start.ps1 builds one long argument string and hands it to a scheduled task. A mistake in that string costs
# a game trial: the task starts, powershell refuses the line, and the trial runs with nothing captured. The host
# checks read the two scripts as text; this one goes one step further and has PowerShell itself bind the line,
# against a stub that carries the capture's real param block and nothing else. It proves that the switches exist,
# that none of them is passed twice, and that the values have the types the capture declares.
#
# It also round-trips the provider spec of -SchedulerStacks through Start-Process and Windows argv splitting, the
# two steps that decide whether PerfView sees one argument or five. That part needs no ETW session either.
#
# What it cannot prove: that PerfView accepts the provider spelling, and what the mode costs in bytes. Both need
# the lab: etw-capture.ps1 -Smoke -SchedulerStacks (one window, no game), then read etw-notes.txt for the
# collector exit code and the file sizes.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$capture = Join-Path $here 'etw-capture.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($capture, [ref]$null, [ref]$null)
$paramBlock = $ast.ParamBlock.Extent.Text
$stub = Join-Path $env:TEMP ("etw-capture-stub-{0}.ps1" -f [guid]::NewGuid().ToString('N'))
Set-Content -LiteralPath $stub -Encoding ascii -Value @(
    $paramBlock,
    '$bound = $PSBoundParameters.GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key)=$($_.Value)" }',
    '"BOUND " + ($bound -join " ")')

# The argument lines etw-start.ps1 builds, with the lab paths stood in for. Keep these in step with that script:
# a line here that it no longer produces proves nothing.
$stage = 'C:\BC250\m15\native-caps999'
$root = 'C:\BC250\m15\etw\999'
$log = "$stage\game\game-log.txt"
$cases = [ordered]@{
    'FpsSeconds 105 (rate window, GPU only)' =
        "-Root `"$root`" -Tag N999 -NotAfterQpc 123456789 -Seconds 30 -StartA 5 -DwmPct 15 -LatestB 110 -GpuOnly -SkipA -SecondsB 105 -WorldLog `"$log`""
    'WorldSeconds 40 (CPU window, as before)' =
        "-Root `"$root`" -Tag N999 -NotAfterQpc 123456789 -Seconds 30 -StartA 5 -DwmPct 15 -LatestB 110 -SkipA -SecondsB 40 -WorldLog `"$log`" -ReserveSeconds 25"
    'WorldSeconds 40 -SchedulerStacks (C49 mechanism window)' =
        "-Root `"$root`" -Tag N999 -NotAfterQpc 123456789 -Seconds 30 -StartA 5 -DwmPct 15 -LatestB 110 -SkipA -SecondsB 40 -WorldLog `"$log`" -ReserveSeconds 35 -SchedulerStacks"
    'WorldSeconds 40 -SchedulerStacks -PresentMode -Process' =
        "-Root `"$root`" -Tag N999 -NotAfterQpc 123456789 -Seconds 30 -StartA 5 -DwmPct 15 -LatestB 110 -SkipA -SecondsB 40 -WorldLog `"$log`" -ReserveSeconds 35 -Process `"ROTTR,witcher3`" -PresentMode -SchedulerStacks"
}
# One line that must be refused: the duplicate the 35 s reserve nearly introduced.
$refused = [ordered]@{
    'two -ReserveSeconds (the duplicate this mode nearly added)' =
        "-Root `"$root`" -Tag N999 -NotAfterQpc 1 -SkipA -SecondsB 40 -ReserveSeconds 25 -SchedulerStacks -ReserveSeconds 35"
}
# And one line that is NOT refused, which is the whole reason host-checks.ps1 compares the two scripts as text:
# measured on this Windows (PowerShell 5.1 and 7), powershell.exe -File takes an unknown -Switch, binds nothing
# for it and exits 0. A capture staged without -SchedulerStacks would therefore run the window in the old mode
# and say nothing. Nothing in the binder can catch that; only the static agreement check can.
$dropped = [ordered]@{
    'an unknown switch is accepted and dropped (the static check is the only guard)' =
        @{ line = "-Root `"$root`" -Tag N999 -NotAfterQpc 1 -SchedulerStacksPlease"; absent = 'SchedulerStacksPlease' }
}
$failures = 0
# A refused line makes powershell.exe write to stderr, and with ErrorActionPreference Stop a native command's
# stderr is a terminating error here. The binder's verdict is the exit code and the output, so stderr is only
# collected inside Bind, never acted on.
function Bind([string]$line) {
    $argv = @($line -split ' (?=(?:[^"]*"[^"]*")*[^"]*$)' | ForEach-Object { $_.Trim('"') })
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $stub @argv 2>&1 | Out-String
        return @{ ok = ($LASTEXITCODE -eq 0 -and $out -like '*BOUND *'); out = $out.Trim() }
    } finally { $ErrorActionPreference = $old }
}
try {
    foreach ($name in $cases.Keys) {
        $r = Bind $cases[$name]
        if (!$r.ok) { $failures++; "FAIL  binds: $name : $($r.out)" } else { "ok    binds: $name" }
    }
    foreach ($name in $refused.Keys) {
        $r = Bind $refused[$name]
        if ($r.ok) { $failures++; "FAIL  refused: $name was accepted" } else { "ok    refused: $name" }
    }
    foreach ($name in $dropped.Keys) {
        $case = $dropped[$name]
        $r = Bind $case.line
        if (!$r.ok) { $failures++; "FAIL  dropped: $name was refused instead: $($r.out)" }
        elseif ($r.out -like "*$($case.absent)*") { $failures++; "FAIL  dropped: $name actually bound it" }
        else { "ok    dropped: $name" }
    }
} finally { Remove-Item -LiteralPath $stub -Force -ErrorAction SilentlyContinue }

# ---- the provider spec, through the same two steps the capture uses ------------------------------------------
# PerfView's /Providers value has to arrive as ONE argv element with spaces inside it. Start-Process -ArgumentList
# joins the elements with spaces and adds no quotes of its own, so the quotes have to be in the string; Windows
# argv splitting then removes them and keeps the spaces. The spec is taken out of etw-capture.ps1's own text, so a
# drift there fails here instead of on the lab.
$captureText = Get-Content -LiteralPath $capture -Raw
$assign = [regex]::Matches($captureText, '(?m)^\s*\$(ids|stackIds|spec)\s*=\s*.+$')
if ($assign.Count -ne 3) { "FAIL  spec: etw-capture.ps1 no longer has the three assignments"; $failures++ }
else {
    foreach ($m in $assign) { Invoke-Expression $m.Value.Trim() }
    $argvStub = Join-Path $env:TEMP ("etw-argv-stub-{0}.ps1" -f [guid]::NewGuid().ToString('N'))
    $outFile = "$argvStub.out"
    Set-Content -LiteralPath $argvStub -Encoding ascii -Value '$args | ForEach-Object { "ARG[$_]" }'
    function ArgvOf([string[]]$list) {
        Start-Process -FilePath 'powershell.exe' -Wait -NoNewWindow -RedirectStandardOutput $outFile `
            -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $argvStub) + $list)
        return @(Get-Content -LiteralPath $outFile | Where-Object { $_ -like 'ARG[[]*' })
    }
    try {
        $expected = '/Providers:Microsoft-Windows-DxgKrnl:0x88008001:5:@EventIDsToEnable=' +
                    '20 238 436 175 176 177 181 17 18 19 22;@EventIDStacksToEnable=20 238 436'
        $got = @(ArgvOf @($spec))      # @() again: return unrolls a one-element array into a string
        if ($got.Count -eq 1 -and $got[0] -eq "ARG[$expected]") { "ok    spec: one argv element, spaces kept, quotes removed" }
        else { $failures++; "FAIL  spec: $($got.Count) element(s): $($got -join ' | ')" }
        # The same spec without its quotes: several elements, which is what PerfView would be handed if the quotes
        # were treated as decoration. This is why they are written into the string.
        $got = @(ArgvOf @($spec.Replace('"', '')))
        if ($got.Count -gt 1) { "ok    spec: unquoted, the same text splits into $($got.Count) elements" }
        else { $failures++; 'FAIL  spec: the unquoted spec did not split, so this check proves nothing' }
        # The comma form the first draft used cannot be caught here: commas do not split argv, only PerfView's own
        # parser splits on them ("/Providers ... a comma separated list of specifications for providers"). The
        # static check in host-checks.ps1 is what refuses it.
        if (($ids -split ' ').Count -lt 5) { $failures++; 'FAIL  spec: the id list is not space separated' }
        else { "ok    spec: $(($ids -split ' ').Count) event ids, space separated, and $(($stackIds -split ' ').Count) with stacks" }
    } finally {
        Remove-Item -LiteralPath $argvStub, $outFile -Force -ErrorAction SilentlyContinue
    }
}
if ($failures) { "DRYRUN FAIL $failures"; exit 1 }
'DRYRUN PASS: every argument line the start script builds binds to the capture, the duplicate reserve is refused, an unknown switch is silently dropped (which is what the static check in host-checks.ps1 is for), and the provider spec survives Start-Process and argv splitting as one element'
