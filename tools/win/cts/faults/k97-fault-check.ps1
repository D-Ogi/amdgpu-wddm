# Runs one CTS case list through run-batch.ps1 and reads the kernel driver's log ring right afterwards, to see
# whether the cases' GPU work raised VM faults. The ring holds only the last ~1024 lines (seconds of activity),
# so the read follows the runner at once; the count of gfx job lines from the deqp-vk pid shows whether the ring
# still covers that process's submissions (0 = the ring rotated past them, and the check says nothing).
#   target.py ps faults\k97-fault-check.ps1 -CaseList C:\BC250\cts\batches\rt-k97-di1.txt -Perftest rtwave64
# Each script in this directory resolves the CLI by itself, because target.py ps copies one file to the target.
param(
    [string]$CaseList = 'C:\BC250\cts\batches\rt-k97-di1.txt',
    [string]$RunId = 'rt-k97-fault',
    [string]$Perftest = '',
    [string]$Root = 'C:\BC250\cts',
    [string]$Runner = '',
    [string]$Cli = ''
)
# The CLI ships with the kernel-driver kit, whose directory changes with every kit (STATE.md names the current
# one): give -Cli, or set BC250_KMD_CLI, or let this take the newest bc250kmd_cli.exe under C:\BC250.
function Resolve-Cli([string]$Path) {
    if ($Path) { return $Path }
    if ($env:BC250_KMD_CLI) { return $env:BC250_KMD_CLI }
    $found = @(Get-ChildItem -LiteralPath 'C:\BC250' -Recurse -Depth 1 -Filter 'bc250kmd_cli.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($found.Count) { return $found[0].FullName }
    throw 'no bc250kmd_cli.exe under C:\BC250; pass -Cli <path>'
}
$Cli = Resolve-Cli $Cli
if (-not $Runner) { $Runner = Join-Path $Root 'run-batch.ps1' }
$args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Runner, '-Root', $Root, '-CaseList', $CaseList,
    '-RunId', $RunId)
if ($Perftest) { $args2 += @('-Perftest', $Perftest) }
& "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" @args2
"runner exit $LASTEXITCODE"
"cli $Cli"
$lines = @(& $Cli log 2>&1 | ForEach-Object { "$_" })
$ringStart = $null
for ($i = 1; $i -lt $lines.Count; $i++) {
    $a = $lines[$i - 1].Trim() -split '\s+'; $b = $lines[$i].Trim() -split '\s+'
    if ($a.Count -gt 0 -and $b.Count -gt 0 -and ($a[0] -as [long]) -and ($b[0] -as [long]) -and ([long]$b[0] - [long]$a[0]) -gt 1000) { $ringStart = $lines[$i]; break }
}
"ring from: $ringStart"
"ring to:   $($lines[$lines.Count - 2])"
$json = Get-ChildItem -LiteralPath (Join-Path (Join-Path $Root 'results') $RunId) -Recurse -Filter 'attempt-*.json' | Sort-Object LastWriteTime | Select-Object -Last 1
$deqpPid = $null
if ($json) { $deqpPid = (Get-Content -LiteralPath $json.FullName -Raw | ConvertFrom-Json).pid }
"deqp-vk pid: $deqpPid"
if ($deqpPid) { "gfx job lines of that pid in the ring: " + @($lines | Where-Object { $_ -match " pid $deqpPid " }).Count }
$hits = @($lines | Where-Object { $_ -match '(?i)vm fault|vm_l2|utcl2|protection fault|memviol|no-retry|retry fault' })
"fault lines: $($hits.Count)"
$hits | Select-Object -First 20
