#Requires -Version 5
# The dense language model regression of a release train: the exact llama.cpp batch that stopped Windows with
# stop code 0x116 on 0.7.216.100-tester.25 (BD-114, issue #36). It runs the installed driver, installs nothing
# and changes no setting.
#
# The run goes through the seg0 supervisor kit, which owns the bound, the thermal rule and the cleanup, exactly
# as the BD-114 lab round of 2026-10-10 ran it. The kit, the model and the llama.cpp build are lab staging:
# this script reads them where that round left them and refuses to start when one of them is absent.
#
#   -Deadline        the supervisor's own bound, inside the arm's bound
#   -WorkloadStop    when the supervisor stops the child, so that its cleanup is inside the bound
#   -Tag             the name of the run directory under <Root>\runs
#
# It prints one result line that the train validation reads:
#   llm-dense result verdict <PASS|FAILED> arm_exit <n> pp512 <t/s> tg128 <t/s>
# The model file is hashed against its approved pin before and after the run, as that round did: the kit's own
# operation cap cannot carry a 6.5 GiB hash inside its preflight.
param(
    [string]$Model = 'C:\BC250\llmvram\models\gemma-4-12b-it-qat-q4_0.gguf',
    [string]$Exe = 'C:\BC250\llmvram\bin-upstream\llama-bench.exe',
    [string]$BenchArgs = '-ub 512 -p 512 -n 128 -r 3',
    [string]$Root = 'C:\BC250\bd114',
    [string]$Pins = 'BD114-pins-gemma-nomodel.json',
    [string]$ModelPins = 'BD114-pins-all.json',
    [string]$Tag = 'train-llm-dense',
    [ValidateRange(30, 170)][int]$Deadline = 160,
    [int]$WorkloadStop = 120
)
$ErrorActionPreference = 'Continue'
function Encode([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
"utc $([DateTime]::UtcNow.ToString('o'))"
$kit = Join-Path $Root 'kit'
$supervisor = Join-Path $kit 'seg0-arm3.ps1'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
foreach ($needed in @($supervisor, (Join-Path $kit 'seg0-operations.ps1'), $Exe, $Model,
        (Join-Path $Root $Pins), (Join-Path $Root $ModelPins))) {
    if (-not (Test-Path -LiteralPath $needed)) { "MISSING $needed"; exit 2 }
}
"supervisor $supervisor sha256 $((Get-FileHash -LiteralPath $supervisor -Algorithm SHA256).Hash)"
$manifest = Join-Path $kit 'seg0-kit-manifest.json'
if (Test-Path -LiteralPath $manifest) {
    "kit manifest sha256 $((Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash)"
}
# Not `$pins`: PowerShell variable names are case-insensitive, so that name is the `[string]$Pins`
# parameter, and an object assigned to it is coerced to its string form. The supervisor then gets a string
# where it wants the pin map and refuses with 'workload hash pin missing'.
$pinMap = Get-Content -LiteralPath (Join-Path $Root $Pins) -Raw | ConvertFrom-Json
if ($pinMap -isnot [psobject] -or -not $pinMap.PSObject.Properties[$Exe]) { "MISSING pin for $Exe in $Pins"; exit 2 }
$modelPin = ((Get-Content -LiteralPath (Join-Path $Root $ModelPins) -Raw | ConvertFrom-Json).PSObject.Properties[$Model]).Value
if (-not $modelPin) { "MISSING pin for $Model in $ModelPins"; exit 2 }
$started = Get-Date
$before = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model_before $before in $([Math]::Round(((Get-Date) - $started).TotalSeconds, 2)) s pin $modelPin"
if ($before -ne $modelPin) { 'model hash does not match the pin'; exit 2 }
$arguments = '-m "' + $Model + '" -o json -t 6 -ngl 99 -fa on -v ' + $BenchArgs
"arguments $arguments"
'--- watchdog lines of the driver log before ---'
if (Test-Path -LiteralPath $cli) {
    & $cli log summary only 2>&1 | Select-String 'submit watchdog|node 0 hardware|node 1 paging' |
        ForEach-Object { '  ' + $_.Line }
}
'--- ARM ---'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $supervisor `
    -Tag $Tag -ExePath $Exe -ArgsB64 (Encode $arguments) -ProcPattern 'llama-*' `
    -ExpectedHashesB64 (Encode ($pinMap | ConvertTo-Json -Compress)) `
    -EnvB64 (Encode 'MESA_SHADER_CACHE_DISABLE=false') -StdoutName 'stdout.json' `
    -RunRoot $Root -Deadline $Deadline -WorkloadStop $WorkloadStop -OperationCapMs 15000
$armExit = $LASTEXITCODE
"arm_exit $armExit"
$dir = Get-ChildItem (Join-Path $Root 'runs') -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -like ($Tag + '-*') | Sort-Object LastWriteTime | Select-Object -Last 1
$verdict = 'FAILED'
$pp = ''
$tg = ''
if ($dir) {
    "run_dir $($dir.FullName)"
    $sj = Join-Path $dir.FullName 'supervisor.json'
    if (Test-Path -LiteralPath $sj) {
        $report = Get-Content -LiteralPath $sj -Raw | ConvertFrom-Json
        $verdict = [string]$report.Verdict
        "supervisor verdict $verdict reason $($report.Reason) child_exit $($report.ChildExit) elapsed $($report.ElapsedSeconds) s"
    }
    $so = Join-Path $dir.FullName 'stdout.json'
    if (Test-Path -LiteralPath $so) {
        $raw = Get-Content -LiteralPath $so -Raw
        try {
            $rows = $raw | ConvertFrom-Json
            foreach ($row in @($rows)) {
                if ($row.n_gen -eq 0) { $pp = [Math]::Round([double]$row.avg_ts, 2) }
                elseif ($row.n_prompt -eq 0) { $tg = [Math]::Round([double]$row.avg_ts, 2) }
                "  n_prompt $($row.n_prompt) n_gen $($row.n_gen) avg_ts $($row.avg_ts) stddev $($row.stddev_ts) samples $(@($row.samples_ts).Count)"
            }
        } catch { "stdout.json does not parse: $($_.Exception.Message)" }
    } else { 'stdout.json absent' }
    '--- stderr tail ---'
    foreach ($name in 'stderr.txt', 'child-stderr.txt', 'stderr.log') {
        $p = Join-Path $dir.FullName $name
        if (Test-Path -LiteralPath $p) { Get-Content -LiteralPath $p -Tail 25 | ForEach-Object { '  ' + $_ } }
    }
} else { 'no run directory' }
'--- driver after the arm ---'
if (Test-Path -LiteralPath $cli) {
    & $cli log 2>&1 | Select-String 'HARDWARE FENCE TIMEOUT|NOT dispatched|ResetEngine node|GPU FAULT|recovery' |
        Select-Object -Last 20 | ForEach-Object { '  ' + $_.Line }
    & $cli log summary only 2>&1 | Select-String 'submit watchdog|node 0 hardware|node 1 paging' |
        ForEach-Object { '  ' + $_.Line }
    & $cli dpm 2>&1 | Select-Object -First 2 | ForEach-Object { '  ' + $_ }
}
$hangKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters\HangRecovery'
if (Test-Path $hangKey) {
    $hr = Get-ItemProperty $hangKey
    foreach ($name in 'LastVerdict', 'LastSeq', 'LastNode', 'Count') {
        if ($hr.PSObject.Properties[$name]) { "  HangRecovery $name = $($hr.$name)" }
    }
} else { '  HangRecovery key absent' }
$par = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -ErrorAction SilentlyContinue
foreach ($name in 'DpmMode', 'DpmLastMode', 'DpmLastReason') {
    if ($par -and $par.PSObject.Properties[$name]) { "  $name = $($par.$name)" }
}
$started = Get-Date
$after = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model_after $after in $([Math]::Round(((Get-Date) - $started).TotalSeconds, 2)) s match $($after -eq $modelPin)"
"llm-dense result verdict $verdict arm_exit $armExit pp512 $pp t/s tg128 $tg t/s"
if ($verdict -eq 'PASS' -and $armExit -eq 0) { exit 0 } else { exit 1 }
