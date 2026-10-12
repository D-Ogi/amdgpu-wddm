#Requires -Version 5
# The text check of a language model on the installed driver: one question in the model's own chat template
# (llama-completion --jinja -cnv -st), greedy, and the answer must name the known fact. A raw prompt to an
# instruction model proves nothing: Gemma 12B gives the same 16 digits on HIP, Vulkan, a CUDA card and the CPU
# (scratch/bd117-4090). It runs the installed driver, installs nothing and changes no setting.
#
# The run goes through the seg0 supervisor kit, which owns the bound, the thermal rule and the cleanup, as the
# dense model arm (llm-dense.ps1) does, with the same pins: the BD-114 pins file names every file of the
# upstream llama.cpp build, and the driver pins are refreshed from what the OS loads. That file does not name
# llama-completion.exe, so -ExeSha pins it here. The model is hashed against -ModelSha before and after the run.
#
# Pass: the supervisor is clean and the child ends with exit 0, the answer names -Expect, the standard error
# stream has no error line (an E line of the llama.cpp log, a lost device or a VK_ERROR code), and the driver
# log has no new GPU fault, fence timeout or recovery line. A model with a thought channel (Gemma 4) answers in
# it first ("<|channel>thought ... <channel|>"), so -Tokens leaves room for the final answer after it. Whether
# that final answer appears is reported, and it is not a condition of the pass. The two result lines that the
# train validation reads:
#   <Label> text <PASS|FAIL> <the first 80 characters of the answer, or the reason>
#   <Label> text final answer: <yes|no|no thought channel> <the first 80 characters of the final answer>
param(
    [string]$Model = 'C:\BC250\llmvram\models\gemma-4-12b-it-qat-q4_0.gguf',
    [ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ModelSha = '93567E57A8FE10B23569B9D9EC38CD005DEEDF71E29477C421A4B83F418A538B',
    [string]$Exe = 'C:\BC250\llmvram\bin-upstream\llama-completion.exe',
    [ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExeSha = 'FA3FBD925E91448E451BF06E1E67D37B26659D0A9968E43CE92B28717B1C421E',
    [string]$Root = 'C:\BC250\bd114',
    [string]$Pins = 'BD114-pins-gemma-nomodel.json',
    [string]$Question = 'What is the capital of France? Answer in one word.',
    [string]$Expect = 'Paris',
    [ValidateRange(16, 512)][int]$Tokens = 160,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label = 'vulkan',
    [string]$Tag = 'train-llm-text',
    [ValidateRange(30, 170)][int]$Deadline = 100,
    [int]$WorkloadStop = 70
)
$ErrorActionPreference = 'Continue'
function Encode([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
function Fail([string]$Why) { "$Label text FAIL ($Why)"; exit 2 }
"utc $([DateTime]::UtcNow.ToString('o'))"
$kit = Join-Path $Root 'kit'
$supervisor = Join-Path $kit 'seg0-arm3.ps1'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$faultPattern = 'HARDWARE FENCE TIMEOUT|NOT dispatched|ResetEngine node|GPU FAULT|recovery'
foreach ($needed in @($supervisor, (Join-Path $kit 'seg0-operations.ps1'), $Exe, $Model, (Join-Path $Root $Pins))) {
    if (-not (Test-Path -LiteralPath $needed)) { "MISSING $needed"; Fail "$needed is missing" }
}
"supervisor $supervisor sha256 $((Get-FileHash -LiteralPath $supervisor -Algorithm SHA256).Hash)"
$exeNow = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
"exe $Exe sha256 $exeNow pin $($ExeSha.ToUpperInvariant())"
if ($exeNow -ne $ExeSha.ToUpperInvariant()) { Fail "$Exe is not the pinned build" }
# Not `$pins`: PowerShell variable names are case-insensitive, so that name is the `[string]$Pins` parameter.
$pinMap = Get-Content -LiteralPath (Join-Path $Root $Pins) -Raw | ConvertFrom-Json
if ($pinMap -isnot [psobject]) { Fail "$Pins is not a pin map" }
# The same driver pin refresh as llm-dense.ps1: the pins file names the driver files of the BD-114 round, and
# a release validation runs the installed release.
$imagePath = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -ErrorAction SilentlyContinue).ImagePath
if ($imagePath) {
    $running = $imagePath -replace '^\\SystemRoot\\', ($env:SystemRoot + '\') -replace '^\\\?\?\\', ''
    if (-not [IO.Path]::IsPathRooted($running)) { $running = Join-Path $env:SystemRoot $running }
}
foreach ($key in @($pinMap.PSObject.Properties.Name)) {
    $isStore = $key -like '*\DriverStore\FileRepository\*bc250kmd.sys'
    $isInstall = $key -like 'C:\Program Files\amdgpu-wddm\*'
    if (-not ($isStore -or $isInstall)) { continue }
    if ($isStore) {
        $pinMap.PSObject.Properties.Remove($key)
        "pin dropped $key (the kernel image is pinned by the path the OS loads)"
        continue
    }
    $old = $pinMap.$key
    if (Test-Path -LiteralPath $key) {
        $now = (Get-FileHash -LiteralPath $key -Algorithm SHA256).Hash
        $pinMap.$key = $now
        if ($now -ne $old) { "pin refreshed $key $now (the pins file had $($old.Substring(0, 8)))" }
    } else {
        $pinMap.PSObject.Properties.Remove($key)
        "pin dropped $key (absent on this install)"
    }
}
if ($running -and (Test-Path -LiteralPath $running)) {
    $hash = (Get-FileHash -LiteralPath $running -Algorithm SHA256).Hash
    $pinMap | Add-Member -NotePropertyName $running -NotePropertyValue $hash -Force
    "pin running kernel image $running $hash"
} else {
    "MISSING the running kernel image ($imagePath)"; Fail 'the running kernel image is not found'
}
$pinMap | Add-Member -NotePropertyName $Exe -NotePropertyValue $exeNow -Force
$started = Get-Date
$before = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model_before $before in $([Math]::Round(((Get-Date) - $started).TotalSeconds, 2)) s pin $($ModelSha.ToUpperInvariant())"
if ($before -ne $ModelSha.ToUpperInvariant()) { Fail 'the model is not the pinned file' }
$arguments = '-m "' + $Model + '" -p "' + $Question + '" -n ' + $Tokens + ' --temp 0 --seed 1234 -ngl 99 -c 2048 --jinja -cnv -st --simple-io'
"arguments $arguments"
$faultsBefore = @(if (Test-Path -LiteralPath $cli) { & $cli log 2>&1 | Select-String $faultPattern | ForEach-Object Line })
'--- ARM ---'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $supervisor `
    -Tag $Tag -ExePath $Exe -ArgsB64 (Encode $arguments) -ProcPattern 'llama-*' `
    -ExpectedHashesB64 (Encode ($pinMap | ConvertTo-Json -Compress)) `
    -EnvB64 (Encode 'MESA_SHADER_CACHE_DISABLE=false') -StdoutName 'stdout.txt' `
    -RunRoot $Root -Deadline $Deadline -WorkloadStop $WorkloadStop -OperationCapMs 15000
$armExit = $LASTEXITCODE
"arm_exit $armExit"
$dir = Get-ChildItem (Join-Path $Root 'runs') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match ('^' + [regex]::Escape($Tag) + '-[0-9a-f]{32}$') } |
    Sort-Object LastWriteTime | Select-Object -Last 1
$verdict = 'FAILED'; $reason = ''; $out = @(); $err = @()
if ($dir) {
    "run_dir $($dir.FullName)"
    $sj = Join-Path $dir.FullName 'supervisor.json'
    if (Test-Path -LiteralPath $sj) {
        $report = Get-Content -LiteralPath $sj -Raw | ConvertFrom-Json
        $verdict = [string]$report.Verdict; $reason = [string]$report.Reason
        "supervisor verdict $verdict reason $reason child_exit $($report.ChildExit) elapsed $($report.ElapsedSeconds) s"
    }
    $out = @(Get-Content -LiteralPath (Join-Path $dir.FullName 'stdout.txt') -ErrorAction SilentlyContinue)
    $err = @(Get-Content -LiteralPath (Join-Path $dir.FullName 'stderr.txt') -ErrorAction SilentlyContinue)
    '--- stdout'
    $out | ForEach-Object { '  ' + $_ }
    '--- stderr tail'
    $err | Select-Object -Last 25 | ForEach-Object { '  ' + $_ }
} else { 'no run directory' }
$errors = @($err | Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+ E |(?i)device ?lost|VK_ERROR_' })
"--- error lines in the standard error stream: $($errors.Count)"
$errors | Select-Object -First 20 | ForEach-Object { '  ' + $_ }
'--- driver after the arm'
$faultsAfter = @(if (Test-Path -LiteralPath $cli) { & $cli log 2>&1 | Select-String $faultPattern | ForEach-Object Line })
$faultsAfter | Select-Object -Last 20 | ForEach-Object { '  ' + $_ }
if (Test-Path -LiteralPath $cli) { & $cli dpm 2>&1 | Select-Object -First 2 | ForEach-Object { '  ' + $_ } }
$newFaults = [Math]::Max(0, $faultsAfter.Count - $faultsBefore.Count)
"driver fault, timeout or recovery lines new in this arm: $newFaults"
$started = Get-Date
$after = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model_after $after in $([Math]::Round(((Get-Date) - $started).TotalSeconds, 2)) s match $($after -eq $before)"
# The answer is what follows the question in the transcript that llama-completion prints (-cnv echoes the
# prompt in the model's template), without the template's role marks and the end-of-text mark.
function Clean([string]$Text) {
    $Text = $Text -replace '<\|?[A-Za-z_/]+\|?>', ' ' -replace '\[end of text\]', ' '
    return ((@($Text -split "`n" | Where-Object { $_.Trim() -notin @('', 'model', 'assistant') }) -join ' ') -replace '\s+', ' ').Trim()
}
$generated = $out -join "`n"
$at = $generated.LastIndexOf($Question)
if ($at -ge 0) { $generated = $generated.Substring($at + $Question.Length) }
$answer = Clean $generated
# The final answer follows the last close of the thought channel. An open channel with no close after it
# means that the model was still thinking when -Tokens ran out.
$open = $generated.LastIndexOf('<|channel>')
$close = $generated.LastIndexOf('<channel|>')
$final = ''
$hasFinal = if ($close -ge 0 -and $close -gt $open) { $final = Clean $generated.Substring($close + '<channel|>'.Length); if ($final) { 'yes' } else { 'no' } }
    elseif ($open -ge 0) { 'no' }
    else { $final = $answer; 'no thought channel' }
"$Label text final answer: $hasFinal $(if ($final) { $final.Substring(0, [Math]::Min(80, $final.Length)) })".TrimEnd()
$why = if ("$armExit" -ne '0' -or $verdict -ne 'PASS') { "supervisor $verdict $reason, exit $armExit" }
    elseif ($errors.Count) { $errors[0] }
    elseif ($newFaults) { $faultsAfter[-1] }
    elseif ($after -ne $before) { 'the model file changed during the run' }
    elseif ($answer -notmatch [regex]::Escape($Expect)) { "the answer does not name $Expect" }
    else { '' }
$shown = if ($answer) { $answer.Substring(0, [Math]::Min(80, $answer.Length)) } else { "($why)" }
if ($why) { "$Label text FAIL $shown"; "reason $why"; exit 1 }
"$Label text PASS $shown"
exit 0
