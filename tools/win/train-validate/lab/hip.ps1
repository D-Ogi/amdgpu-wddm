#Requires -Version 5
# The HIP runtime of the installed release: tools\hip on the machine PATH, vadd.exe from there, then a
# llama.cpp benchmark and a text answer of the HIP backend through it (BD-110). Installs nothing and changes no
# setting. Generic: it names no train and no package.
#
#   consumer  -Bin is a staged HIP build of llama.cpp that holds no amdhip64.dll of its own, and its
#             consumer-pins.json names every file in it by SHA-256: a file that is absent, differs or is not
#             named stops the arm before any GPU work, and so does the ggml-hip.dll of the BD-110 fault build
#   path      the installed tools\hip directory is an entry of the machine PATH, and amdhip64.dll is
#             found there and nowhere earlier in the DLL search order of the benchmark
#   vadd      vadd.exe resolved through this session's PATH, --expect-compute, a kill bound
#   bench     llama-bench.exe of -Bin, TinyLlama -p 512 -n 128, through the seg0 supervisor kit, which owns the
#             bound, the thermal rule and the cleanup. BC250_HIP_LOG=1 sends the runtime's log and every
#             refusal to the standard error stream.
#   text      llama-completion.exe of -Bin through the same supervisor: one question in the model's own chat
#             template (--jinja -cnv -st), greedy. A raw prompt to an instruction model proves nothing: Gemma
#             12B gives the same 16 digits on HIP, Vulkan, a CUDA card and the CPU (scratch/bd117-4090).
#             It runs only when the bench left no new driver fault and its deadline fits in -Budget.
# Neither step passes -fa: the HIP build of llama.cpp has no Flash Attention yet.
#
# BD-110 passes when the benchmark ends with exit 0 and both the pp512 and the tg128 rows, the text step ends
# with exit 0 and its answer names Paris, neither step has an error or warning line of the runtime, no mock
# device, no new GPU fault, fence timeout or recovery, and both supervisors are clean.
# The result lines that the train validation reads:
#   hip result vadd <ok|FAILED> verdict <PASS|FAILED> arm_exit <n> pp512 <t/s> t/s tg128 <t/s> t/s
#   hip text <PASS|FAIL> <the first 80 characters of the answer>
#   BD-110 <PASS|FAIL> <the first error line of the runtime, or the reason>
param(
    [Parameter(Mandatory)][string]$Bin,
    [string]$Model = 'C:\BC250\tmp\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf',
    [string]$ModelSha = 'DA3087FB14AEDE55FDE6EB81A0E55E886810E43509EC82ECDC7AA5D62A03B556',
    [string]$Root = 'C:\BC250\bd114',
    [string]$Tag = 'train-hip',
    [ValidateRange(30, 170)][int]$Deadline = 70,
    [int]$WorkloadStop = 45,
    [ValidateRange(30, 170)][int]$TextDeadline = 55,
    [int]$TextWorkloadStop = 30,
    [ValidateRange(5, 40)][int]$VaddBoundS = 25,
    # The whole script inside the arm's 170 s: the text step starts only when its deadline still fits.
    [ValidateRange(60, 165)][int]$Budget = 160
)
$ErrorActionPreference = 'Continue'
$clock = [Diagnostics.Stopwatch]::StartNew()
function Encode([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
function Norm([string]$p) { if ($p) { $p.Trim().Trim('"').TrimEnd('\').ToLowerInvariant() } else { '' } }
"utc $([DateTime]::UtcNow.ToString('o'))"
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$hip = Join-Path $inst 'tools\hip'
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$exe = Join-Path $Bin 'llama-bench.exe'
$textExe = Join-Path $Bin 'llama-completion.exe'
$consumerPins = Join-Path $Bin 'consumer-pins.json'
$kit = Join-Path $Root 'kit'
$supervisor = Join-Path $kit 'seg0-arm3.ps1'
$faultPattern = 'HARDWARE FENCE TIMEOUT|NOT dispatched|ResetEngine node|GPU FAULT|recovery'
$question = 'What is the capital of France? Answer in one word.'
foreach ($needed in @($hip, (Join-Path $hip 'amdhip64.dll'), (Join-Path $hip 'vadd.exe'), $exe, $textExe, $consumerPins,
        $Model, $supervisor)) {
    if (-not (Test-Path -LiteralPath $needed)) { "MISSING $needed"; 'BD-110 FAIL a staged or installed file is missing'; exit 2 }
}
foreach ($name in @(Get-ChildItem Env: | Where-Object Name -like 'BC250_HIP_*' | ForEach-Object Name)) {
    Remove-Item "Env:\$name"; "removed $name from this session"
}
'--- installed tools\hip'
Get-ChildItem -LiteralPath $hip -File | Sort-Object Name | ForEach-Object {
    '  {0,-22} {1,9}  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }

'--- staged consumer'
# The ggml HIP backend built with the gridDim header of the BD-110 fault: its soft_max kernel faults the GPU.
$refused = @{ '1102A1DDEAF3EBFF5DF394C6EC4C52C23E1E8616EECC401A2E6E103EA74564D0' = 'the ggml-hip.dll of the BD-110 fault build' }
$want = Get-Content -LiteralPath $consumerPins -Raw | ConvertFrom-Json
$binHash = @{}
$wrong = @()
foreach ($f in @(Get-ChildItem -LiteralPath $Bin -File | Sort-Object Name)) {
    if ($f.Name -eq 'consumer-pins.json') { continue }
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    $binHash[$f.Name] = $h
    $pin = if ($want.PSObject.Properties[$f.Name]) { [string]$want.($f.Name) } else { '' }
    $state = if (-not $pin) { 'NOT PINNED' } elseif ($pin -ne $h) { "DIFFERS (pin $($pin.Substring(0, 8)))" } else { 'ok' }
    if ($refused.ContainsKey($h)) { $state = 'REFUSED, ' + $refused[$h] }
    '  {0,-30} {1,9}  {2}  {3}' -f $f.Name, $f.Length, $h.Substring(0, 8), $state
    if ($state -ne 'ok') { $wrong += "$($f.Name) $state" }
}
foreach ($p in $want.PSObject.Properties) { if (-not $binHash.ContainsKey($p.Name)) { $wrong += "$($p.Name) ABSENT" } }
if ($binHash.ContainsKey('amdhip64.dll')) { $wrong += 'amdhip64.dll is in the staged build' }
"consumer: $($binHash.Count) files, $(if ($wrong) { 'FAILED ' + ($wrong -join '; ') } else { 'every file equals its pin' })"
if ($wrong) { 'BD-110 FAIL the staged build is not the pinned consumer: ' + $wrong[0]; exit 2 }

'--- path'
$machine = @(([Environment]::GetEnvironmentVariable('Path', 'Machine')) -split ';' | Where-Object { $_ })
$at = [Array]::FindIndex([string[]]$machine, [Predicate[string]] { param($e) (Norm $e) -eq (Norm $hip) })
"machine PATH entry of tools\hip: $(if ($at -ge 0) { "$($at + 1) of $($machine.Count)" } else { 'ABSENT' })"
$session = @($env:Path -split ';' | Where-Object { $_ })
"this session's PATH holds tools\hip: $(@($session | Where-Object { (Norm $_) -eq (Norm $hip) }).Count -gt 0)"
# The search order of the benchmark: its own directory, the system directories, then PATH. The interactive
# user's PATH is the machine PATH and the user's own entries after it.
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$userPath = @()
try {
    $sid = (New-Object Security.Principal.NTAccount($user)).Translate([Security.Principal.SecurityIdentifier]).Value
    $userPath = @(([string](Get-ItemProperty "Registry::HKEY_USERS\$sid\Environment" -Name Path -ErrorAction Stop).Path) -split ';' | Where-Object { $_ })
} catch { }
$order = @($Bin, (Join-Path $env:SystemRoot 'System32'), (Join-Path $env:SystemRoot 'System'), $env:SystemRoot) + $machine + $userPath
$first = $null
foreach ($dir in $order) {
    $candidate = Join-Path ([Environment]::ExpandEnvironmentVariables($dir)) 'amdhip64.dll'
    if (Test-Path -LiteralPath $candidate) { $first = $candidate; break }
}
$pathOk = $at -ge 0 -and $first -and (Norm (Split-Path -Parent $first)) -eq (Norm $hip)
"amdhip64.dll for the benchmark resolves to: $first"
"path: $(if ($pathOk) { 'ok' } else { 'FAILED' })"

'--- vadd'
$vaddCmd = Get-Command vadd.exe -ErrorAction SilentlyContinue
"vadd.exe from this session's PATH: $(if ($vaddCmd) { $vaddCmd.Source } else { 'NOT FOUND' })"
$vadd = 'FAILED'
if ($vaddCmd) {
    $vout = Join-Path $env:TEMP "train-hip-vadd-$PID.txt"
    $p = Start-Process -FilePath $vaddCmd.Source -ArgumentList @('--expect-compute', '--wait-total', '20000') -PassThru -NoNewWindow `
        -RedirectStandardOutput $vout -RedirectStandardError "$vout.err"
    $null = $p.Handle
    $code = 'killed'
    if ($p.WaitForExit($VaddBoundS * 1000)) { $code = $p.ExitCode } else { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $vlines = @(Get-Content -LiteralPath $vout -ErrorAction SilentlyContinue) + @(Get-Content -LiteralPath "$vout.err" -ErrorAction SilentlyContinue)
    $vlines | ForEach-Object { '  ' + $_ }
    "vadd exit $code"
    if ("$code" -eq '0' -and ($vlines -match '^vadd: ok')) { $vadd = 'ok' }
}

'--- bench'
# Every file the two steps load is pinned, and the supervisor checks each pin before and after each run.
$pinMap = [ordered]@{}
foreach ($name in @($binHash.Keys | Where-Object { $_ -like '*.dll' -or $_ -eq 'llama-bench.exe' -or $_ -eq 'llama-completion.exe' } | Sort-Object)) {
    $pinMap[(Join-Path $Bin $name)] = $binHash[$name]
}
foreach ($f in @(Get-ChildItem -LiteralPath $hip -Filter '*.dll' -File)) {
    $pinMap[$f.FullName] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
}
$icd = 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll'
if (Test-Path -LiteralPath $icd) { $pinMap[$icd] = (Get-FileHash -LiteralPath $icd -Algorithm SHA256).Hash }
$modelNow = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model $Model sha256 $modelNow pin $ModelSha"
if ($modelNow -ne $ModelSha) { 'model hash does not match the pin'; 'BD-110 FAIL the model is not the pinned file'; exit 2 }
$pinMap[$Model] = $ModelSha
foreach ($k in $pinMap.Keys) { "  pin $k $($pinMap[$k].Substring(0, 8))" }
$pinsB64 = Encode ($pinMap | ConvertTo-Json -Compress)
$envB64 = Encode 'BC250_HIP_LOG=1;BC250_HIP_WAIT_TOTAL_MS=10000'

function Get-FaultLines {
    if (Test-Path -LiteralPath $cli) { @(& $cli log 2>&1 | Select-String $faultPattern | ForEach-Object Line) } else { @() }
}
# The newest run directory of one supervisor tag (<tag>-<run id>), its report, its output and its runtime log.
function Read-Run([string]$RunTag, [string]$StdoutName) {
    $run = @{ Dir = $null; Verdict = 'FAILED'; Reason = ''; Report = $null; Out = @(); Err = @(); Errors = @(); Mock = 0 }
    $run.Dir = Get-ChildItem (Join-Path $Root 'runs') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match ('^' + [regex]::Escape($RunTag) + '-[0-9a-f]{32}$') } |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $run.Dir) { return $run }
    $sj = Join-Path $run.Dir.FullName 'supervisor.json'
    if (Test-Path -LiteralPath $sj) {
        $run.Report = Get-Content -LiteralPath $sj -Raw | ConvertFrom-Json
        $run.Verdict = [string]$run.Report.Verdict; $run.Reason = [string]$run.Report.Reason
    }
    $run.Out = @(Get-Content -LiteralPath (Join-Path $run.Dir.FullName $StdoutName) -ErrorAction SilentlyContinue)
    $run.Err = @(Get-Content -LiteralPath (Join-Path $run.Dir.FullName 'stderr.txt') -ErrorAction SilentlyContinue)
    $run.Errors = @($run.Err | Where-Object { $_ -match '^amdhip64 \[\d+:\d+\] (error|warn)' })
    $run.Mock = @($run.Err | Where-Object { $_ -match '(?i)mock' }).Count
    return $run
}
function Show-Run($Run) {
    if (-not $Run.Dir) { 'no run directory'; return }
    "run_dir $($Run.Dir.FullName)"
    if ($Run.Report) {
        "supervisor verdict $($Run.Verdict) reason $($Run.Reason) child_exit $($Run.Report.ChildExit) elapsed $($Run.Report.ElapsedSeconds) s"
    }
    $Run.Out | ForEach-Object { '  ' + $_ }
    "--- runtime log: $($Run.Err.Count) lines, $($Run.Errors.Count) error or warning lines, $($Run.Mock) naming a mock"
    $Run.Errors | Select-Object -First 40 | ForEach-Object { '  ' + $_ }
    '--- stderr tail'
    $Run.Err | Select-Object -Last 30 | ForEach-Object { '  ' + $_ }
}
# The answer is what follows the question in the transcript that llama-completion prints (-cnv echoes the
# prompt in the model's template), without the template's role marks and the end-of-text mark.
function Get-Answer([string[]]$Lines) {
    $text = $Lines -join "`n"
    $at = $text.LastIndexOf($question)
    if ($at -ge 0) { $text = $text.Substring($at + $question.Length) }
    $text = $text -replace '<\|?[A-Za-z_/]+\|?>', ' ' -replace '\[end of text\]', ' '
    $kept = @($text -split "`n" | Where-Object { $_.Trim() -notin @('', 'model', 'assistant') })
    return (($kept -join ' ') -replace '\s+', ' ').Trim()
}

$arguments = '-m "' + $Model + '" -p 512 -n 128 -ngl 99 -r 1 -t 4'
"arguments $arguments"
$before = @(Get-FaultLines)
'--- ARM ---'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $supervisor `
    -Tag $Tag -ExePath $exe -ArgsB64 (Encode $arguments) -ProcPattern 'llama-*' -ExpectedHashesB64 $pinsB64 `
    -EnvB64 $envB64 -StdoutName 'stdout.txt' `
    -RunRoot $Root -Deadline $Deadline -WorkloadStop $WorkloadStop -OperationCapMs 15000
$armExit = $LASTEXITCODE
"arm_exit $armExit"
$bench = Read-Run $Tag 'stdout.txt'
Show-Run $bench
$pp = ''; $tg = ''
foreach ($line in $bench.Out) {
    if ($line -match '\|\s*pp512\s*\|\s*([\d.]+)') { $pp = $Matches[1] }
    if ($line -match '\|\s*tg128\s*\|\s*([\d.]+)') { $tg = $Matches[1] }
}
'--- driver after the bench'
$mid = @(Get-FaultLines)
$mid | Select-Object -Last 20 | ForEach-Object { '  ' + $_ }
$newFaults = [Math]::Max(0, $mid.Count - $before.Count)
"driver fault, timeout or recovery lines new in the bench: $newFaults"

'--- text'
$textTag = $Tag + '-text'
$textArgs = '-m "' + $Model + '" -p "' + $question + '" -n 64 --temp 0 --seed 1234 -ngl 99 -c 2048 --jinja -cnv -st --simple-io'
"arguments $textArgs"
$left = $Budget - [int]$clock.Elapsed.TotalSeconds
$chat = $null; $textExit = 'not run'; $textFaults = 0; $answer = ''
$textWhy = if ($newFaults) { 'not run: the bench left a new driver fault' }
    elseif ($left -lt $TextDeadline + 10) { "not run: $left s of the $Budget s budget left, the step needs $($TextDeadline + 10) s" }
    else { '' }
if (-not $textWhy) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $supervisor `
        -Tag $textTag -ExePath $textExe -ArgsB64 (Encode $textArgs) -ProcPattern 'llama-*' -ExpectedHashesB64 $pinsB64 `
        -EnvB64 $envB64 -StdoutName 'stdout.txt' `
        -RunRoot $Root -Deadline $TextDeadline -WorkloadStop $TextWorkloadStop -OperationCapMs 15000
    $textExit = $LASTEXITCODE
    "text_exit $textExit"
    $chat = Read-Run $textTag 'stdout.txt'
    Show-Run $chat
    $answer = Get-Answer $chat.Out
    $after = @(Get-FaultLines)
    $textFaults = [Math]::Max(0, $after.Count - $mid.Count)
    '--- driver after the text step'
    $after | Select-Object -Last 20 | ForEach-Object { '  ' + $_ }
    "driver fault, timeout or recovery lines new in the text step: $textFaults"
    $textWhy = if ($chat.Mock) { 'the runtime log names a mock device' }
        elseif ($chat.Errors.Count) { $chat.Errors[0] }
        elseif ("$textExit" -ne '0' -or $chat.Verdict -ne 'PASS') { "supervisor $($chat.Verdict) $($chat.Reason), exit $textExit" }
        elseif ($textFaults) { $after[-1] }
        elseif ($answer -notmatch 'paris') { 'the answer does not name Paris' }
        else { '' }
}
if (Test-Path -LiteralPath $cli) { & $cli dpm 2>&1 | Select-Object -First 2 | ForEach-Object { '  ' + $_ } }
"hip result vadd $vadd verdict $($bench.Verdict) arm_exit $armExit pp512 $pp t/s tg128 $tg t/s"
$shown = if ($answer) { $answer.Substring(0, [Math]::Min(80, $answer.Length)) } else { "($textWhy)" }
"hip text $(if ($textWhy) { 'FAIL' } else { 'PASS' }) $shown"
$why = if (-not $pathOk) { 'tools\hip is not the amdhip64.dll the benchmark loads' }
    elseif ($bench.Mock) { 'the runtime log names a mock device' }
    elseif ($bench.Errors.Count) { $bench.Errors[0] }
    elseif ($armExit -ne 0 -or $bench.Verdict -ne 'PASS') { "supervisor $($bench.Verdict) $($bench.Reason), arm exit $armExit" }
    elseif (-not $pp -or -not $tg) { 'a pp512 or tg128 row is missing' }
    elseif ($newFaults) { $mid[-1] }
    elseif ($textWhy) { "the text step: $textWhy" }
    else { '' }
if ($why) { "BD-110 FAIL $why" } else { 'BD-110 PASS exit 0, pp512 and tg128 rows, the templated answer names Paris, no runtime error or warning line' }
if (-not $why -and $vadd -eq 'ok') { exit 0 } else { exit 1 }
