#Requires -Version 5
# The HIP runtime of the installed release: tools\hip on the machine PATH, vadd.exe from there, then a
# llama.cpp benchmark of the HIP backend through it (BD-110). Installs nothing and changes no setting.
# Generic: it names no train and no package.
#
#   path    the installed tools\hip directory is an entry of the machine PATH, and amdhip64.dll is
#           found there and nowhere earlier in the DLL search order of the benchmark
#   vadd    vadd.exe resolved through this session's PATH, --expect-compute, a kill bound
#   bench   llama-bench.exe of a staged HIP build (-Bin, which holds no amdhip64.dll of its own), TinyLlama
#           -p 512 -n 128, through the seg0 supervisor kit, which owns the bound, the thermal rule and the
#           cleanup. BC250_HIP_LOG=1 sends the runtime's log and every refusal to the standard error stream.
#
# BD-110 passes when the benchmark ends with exit 0, both the pp512 and the tg128 rows, no error or warning
# line of the runtime, no mock device, no new GPU fault, fence timeout or recovery, and a clean supervisor.
# The result lines that the train validation reads:
#   hip result vadd <ok|FAILED> verdict <PASS|FAILED> arm_exit <n> pp512 <t/s> t/s tg128 <t/s> t/s
#   BD-110 <PASS|FAIL> <the first error line of the runtime, or the reason>
param(
    [Parameter(Mandatory)][string]$Bin,
    [string]$Model = 'C:\BC250\tmp\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf',
    [string]$ModelSha = 'DA3087FB14AEDE55FDE6EB81A0E55E886810E43509EC82ECDC7AA5D62A03B556',
    [string]$Root = 'C:\BC250\bd114',
    [string]$Tag = 'train-hip',
    [ValidateRange(30, 170)][int]$Deadline = 110,
    [int]$WorkloadStop = 80,
    [ValidateRange(5, 40)][int]$VaddBoundS = 30
)
$ErrorActionPreference = 'Continue'
function Encode([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
function Norm([string]$p) { if ($p) { $p.Trim().Trim('"').TrimEnd('\').ToLowerInvariant() } else { '' } }
"utc $([DateTime]::UtcNow.ToString('o'))"
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$hip = Join-Path $inst 'tools\hip'
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$exe = Join-Path $Bin 'llama-bench.exe'
$kit = Join-Path $Root 'kit'
$supervisor = Join-Path $kit 'seg0-arm3.ps1'
foreach ($needed in @($hip, (Join-Path $hip 'amdhip64.dll'), (Join-Path $hip 'vadd.exe'), $exe, $Model, $supervisor)) {
    if (-not (Test-Path -LiteralPath $needed)) { "MISSING $needed"; 'BD-110 FAIL a staged or installed file is missing'; exit 2 }
}
foreach ($name in @(Get-ChildItem Env: | Where-Object Name -like 'BC250_HIP_*' | ForEach-Object Name)) {
    Remove-Item "Env:\$name"; "removed $name from this session"
}
'--- installed tools\hip'
Get-ChildItem -LiteralPath $hip -File | Sort-Object Name | ForEach-Object {
    '  {0,-22} {1,9}  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }

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
# Every file the benchmark loads is pinned, and the supervisor checks each pin before and after the run.
$pinMap = [ordered]@{}
$pinMap[$exe] = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
foreach ($f in @(Get-ChildItem -LiteralPath $Bin -Filter '*.dll' -File) + @(Get-ChildItem -LiteralPath $hip -Filter '*.dll' -File)) {
    $pinMap[$f.FullName] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
}
$icd = 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll'
if (Test-Path -LiteralPath $icd) { $pinMap[$icd] = (Get-FileHash -LiteralPath $icd -Algorithm SHA256).Hash }
$modelNow = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
"model $Model sha256 $modelNow pin $ModelSha"
if ($modelNow -ne $ModelSha) { 'model hash does not match the pin'; 'BD-110 FAIL the model is not the pinned file'; exit 2 }
$pinMap[$Model] = $ModelSha
foreach ($k in $pinMap.Keys) { "  pin $k $($pinMap[$k].Substring(0, 8))" }
$arguments = '-m "' + $Model + '" -p 512 -n 128 -ngl 99 -r 1 -t 4'
"arguments $arguments"
$before = @()
if (Test-Path -LiteralPath $cli) {
    $before = @(& $cli log 2>&1 | Select-String 'HARDWARE FENCE TIMEOUT|NOT dispatched|ResetEngine node|GPU FAULT|recovery' | ForEach-Object Line)
}
'--- ARM ---'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $supervisor `
    -Tag $Tag -ExePath $exe -ArgsB64 (Encode $arguments) -ProcPattern 'llama-*' `
    -ExpectedHashesB64 (Encode ($pinMap | ConvertTo-Json -Compress)) `
    -EnvB64 (Encode 'BC250_HIP_LOG=1;BC250_HIP_WAIT_TOTAL_MS=10000') -StdoutName 'stdout.txt' `
    -RunRoot $Root -Deadline $Deadline -WorkloadStop $WorkloadStop -OperationCapMs 15000
$armExit = $LASTEXITCODE
"arm_exit $armExit"
$dir = Get-ChildItem (Join-Path $Root 'runs') -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -like ($Tag + '-*') | Sort-Object LastWriteTime | Select-Object -Last 1
$verdict = 'FAILED'; $reason = ''; $pp = ''; $tg = ''
$errors = @(); $mock = 0
if ($dir) {
    "run_dir $($dir.FullName)"
    $sj = Join-Path $dir.FullName 'supervisor.json'
    if (Test-Path -LiteralPath $sj) {
        $report = Get-Content -LiteralPath $sj -Raw | ConvertFrom-Json
        $verdict = [string]$report.Verdict; $reason = [string]$report.Reason
        "supervisor verdict $verdict reason $reason child_exit $($report.ChildExit) elapsed $($report.ElapsedSeconds) s"
    }
    $so = Join-Path $dir.FullName 'stdout.txt'
    foreach ($line in @(Get-Content -LiteralPath $so -ErrorAction SilentlyContinue)) {
        '  ' + $line
        if ($line -match '\|\s*pp512\s*\|\s*([\d.]+)') { $pp = $Matches[1] }
        if ($line -match '\|\s*tg128\s*\|\s*([\d.]+)') { $tg = $Matches[1] }
    }
    $se = @(Get-Content -LiteralPath (Join-Path $dir.FullName 'stderr.txt') -ErrorAction SilentlyContinue)
    $errors = @($se | Where-Object { $_ -match '^amdhip64 \[\d+:\d+\] (error|warn)' })
    $mock = @($se | Where-Object { $_ -match '(?i)mock' }).Count
    "--- runtime log: $($se.Count) lines, $($errors.Count) error or warning lines, $mock naming a mock"
    $errors | Select-Object -First 40 | ForEach-Object { '  ' + $_ }
    '--- stderr tail'
    $se | Select-Object -Last 30 | ForEach-Object { '  ' + $_ }
} else { 'no run directory' }
'--- driver after the arm'
$after = @()
if (Test-Path -LiteralPath $cli) {
    $after = @(& $cli log 2>&1 | Select-String 'HARDWARE FENCE TIMEOUT|NOT dispatched|ResetEngine node|GPU FAULT|recovery' | ForEach-Object Line)
    $after | Select-Object -Last 20 | ForEach-Object { '  ' + $_ }
    & $cli dpm 2>&1 | Select-Object -First 2 | ForEach-Object { '  ' + $_ }
}
$newFaults = [Math]::Max(0, $after.Count - $before.Count)
"driver fault, timeout or recovery lines new in this arm: $newFaults"
"hip result vadd $vadd verdict $verdict arm_exit $armExit pp512 $pp t/s tg128 $tg t/s"
$why = if (-not $pathOk) { 'tools\hip is not the amdhip64.dll the benchmark loads' }
    elseif ($mock) { 'the runtime log names a mock device' }
    elseif ($errors.Count) { $errors[0] }
    elseif ($armExit -ne 0 -or $verdict -ne 'PASS') { "supervisor $verdict $reason, arm exit $armExit" }
    elseif (-not $pp -or -not $tg) { 'a pp512 or tg128 row is missing' }
    elseif ($newFaults) { $after[-1] }
    else { '' }
if ($why) { "BD-110 FAIL $why" } else { 'BD-110 PASS exit 0, pp512 and tg128 rows, no runtime error or warning line' }
if (-not $why -and $vadd -eq 'ok') { exit 0 } else { exit 1 }
