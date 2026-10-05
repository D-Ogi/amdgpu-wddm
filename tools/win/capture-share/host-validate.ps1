# SPDX-License-Identifier: MIT
# host-validate.ps1 - runs the headless capshare cells on a development PC with a working graphics driver and compares
# every verdict with the expected one. It proves the client and its oracles before a lab run. No cell here creates a
# window: dda and wgc are run only as the interlock check (they must refuse to start without --interactive-ok).
# Output: <Out>\<stamp>\<run>.txt/.json and summary.txt.
#
#   pwsh -NoProfile -File tools\win\capture-share\host-validate.ps1 [-Exe <capshare.exe>] [-Out <dir>] [-Bound 30]
#        [-Only <name,...>] [-SetEnv NAME=VALUE,...]
#
# -Only runs the named rows alone, in the order of the table below. -SetEnv passes --env to every run, which the
# client applies before it creates any device and the peer inherits. Both sides of our own D3D12 driver read
# AMDGPU_WDDM_LOG, so a trace of a failing cell is:
#   -Only s12to12,s11to12 -SetEnv AMDGPU_WDDM_LOG=file:<dir>\ddi.log,AMDGPU_WDDM_DDI_TRACE=1
# AMDGPU_WDDM_DDI_TRACE alone writes nothing: the sink is off until AMDGPU_WDDM_LOG names one (stdio-log.h).
# A driver refusal needs neither switch, because the driver writes it to the debugger channel and this client
# records that channel of both of its processes in the cell log, with the A or B prefix of the side that wrote it.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Exe = (Join-Path $Root 'scratch\build\capture-share\capshare.exe'),
    [string]$Out = (Join-Path $Root 'scratch\build\capture-share\host'),
    [int]$Bound = 30,
    [string[]]$Only = @(),
    [string[]]$SetEnv = @()
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
New-Item -ItemType Directory -Force $Out | Out-Null
$dir = Join-Path (Resolve-Path $Out).Path $stamp
New-Item -ItemType Directory -Force $dir | Out-Null

# name, expected result, arguments (without --out/--json)
$runs = @(
    @('selftest', 'pass', '--self-test'),
    @('ipc', 'pass', '--cell ipc'),
    @('w11', 'pass', '--cell w11'),
    @('w12', 'pass', '--cell w12'),
    @('capdry', 'pass', '--cell capdry'),
    @('km11', 'pass', '--cell km11'),
    @('km11-kmt', 'pass', '--cell km11 --handle kmt'),
    @('km12to11', 'pass', '--cell km12to11'),
    @('km11to12', 'pass', '--cell km11to12'),
    @('s11to11', 'pass', '--cell s11to11'),
    @('s11to11-kmt', 'pass', '--cell s11to11 --handle kmt'),
    @('s12to11', 'pass', '--cell s12to11'),
    @('s11to12', 'pass', '--cell s11to12'),
    @('s12to12', 'pass', '--cell s12to12'),
    @('s11to11-fence', 'pass', '--cell s11to11 --sync fence'),
    @('s12to11-fence', 'pass', '--cell s12to11 --sync fence'),
    @('s11to12-fence', 'pass', '--cell s11to12 --sync fence'),
    @('s12to12-fence', 'pass', '--cell s12to12 --sync fence'),
    @('f11to11', 'pass', '--cell f11to11'),
    @('f12to11', 'pass', '--cell f12to11'),
    @('f11to12', 'pass', '--cell f11to12'),
    @('f12to12', 'pass', '--cell f12to12'),
    @('s12to11-rgba8', 'pass', '--cell s12to11 --format rgba8 --sync fence'),
    @('s12to12-simultaneous', 'pass', '--cell s12to12 --simultaneous --sync fence'),
    @('km12to11-stderr', 'pass', '--cell km12to11 --stderr {dir}\km12to11-stderr-err.txt'),
    @('ipc-stderr', 'pass', '--cell ipc --stderr {dir}\ipc-stderr-err.txt'),
    @('neg-f11to11', 'mismatch', '--cell f11to11 --inject skip-wait'),
    @('neg-f12to12', 'mismatch', '--cell f12to12 --inject skip-wait'),
    @('neg-s12to11-fence', 'mismatch', '--cell s12to11 --sync fence --inject skip-wait'),
    @('neg-s11to12-fence', 'mismatch', '--cell s11to12 --sync fence --inject skip-wait'),
    @('interlock-dda', 'fail', '--cell dda'),
    @('interlock-wgc', 'fail', '--cell wgc')
)

if ($Only.Count) {
    $wanted = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $runs = @($runs | Where-Object { $wanted -contains $_[0] })
    if (-not $runs.Count) { throw "no row of the table matches -Only $($wanted -join ',')" }
}
$envArgs = (@($SetEnv | ForEach-Object { $_ -split ',' } | Where-Object { $_ } |
    ForEach-Object { "--env $($_ -replace '\{dir\}', $dir)" }) -join ' ')

$lines = @("capshare host validation $stamp", "exe $Exe sha256 $((Get-FileHash $Exe -Algorithm SHA256).Hash)")
if ($envArgs) { $lines += "env $envArgs" }
$lines += ''
$ok = 0
foreach ($r in $runs) {
    $name, $expect, $cellArgs = $r
    $txt = Join-Path $dir "$name.txt"
    $json = Join-Path $dir "$name.json"
    $argLine = ($cellArgs -replace '\{dir\}', $dir) + " --bound $Bound --out $txt --json $json"
    if ($envArgs) { $argLine += " $envArgs" }
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -Wait -PassThru -WindowStyle Hidden
    $verdict = if (Test-Path $txt) { (Select-String -Path $txt -Pattern '^(A\s+\d+\s+)?VERDICT ' | Select-Object -Last 1).Line } else { '' }
    $result = if ($verdict -match ' result=(\S+)') { $Matches[1] } else { 'none' }
    $gate = if ($verdict -match ' gate=(\S+)') { $Matches[1] } else { '-' }
    $pass = $result -eq $expect
    if ($name -like 'neg-*') { $pass = $pass -and $gate -like 'violated*' }
    if ($name -like 'interlock-*') { $pass = $pass -and $p.ExitCode -eq 4 }
    if ($name -eq 'ipc-stderr') {
        # the peer's standard error is the parent's --stderr file from process creation on
        $err = Join-Path $dir 'ipc-stderr-err.txt'
        $pass = $pass -and $verdict -match 'peer_stderr=inherited' -and (Test-Path $err) -and
            (Select-String -Path $err -SimpleMatch 'capshare peer stderr probe' -Quiet)
    }
    if ($pass) { $ok++ }
    $lines += ('{0,-22} expect={1,-8} result={2,-8} exit={3} ok={4}' -f $name, $expect, $result, $p.ExitCode, [int]$pass)
    $lines += "    $verdict"
}
$lines += ''
$lines += "TOTAL ok=$ok/$($runs.Count)"
$summary = Join-Path $dir 'summary.txt'
$lines | Set-Content -Path $summary -Encoding utf8
$lines | Select-Object -Last 1
"summary: $summary"
