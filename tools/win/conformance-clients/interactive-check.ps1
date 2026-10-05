param(
    # Host check of the lab protocol: drives the client through create-device, create-queue, copy, status, exit with
    # the d3d12queue controller, as the lab runner does. WARP by default. -Mode Bc250 is for the lab only.
    [string]$Exe = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\amdgpu_wddm_conformance\amdgpu_wddm_conformance.exe",
    [Parameter(Mandatory)][string]$Directory,
    [ValidateSet('Warp', 'Bc250')][string]$Mode = 'Warp',
    [ValidateRange(1, 150)][int]$Deadline = 70
)
$ErrorActionPreference = 'Stop'
$controller = (Resolve-Path (Join-Path $PSScriptRoot '..\d3d12queue\controller.ps1')).Path
if (Test-Path -LiteralPath $Directory) { throw "$Directory exists; use a new directory" }
New-Item -ItemType Directory -Force $Directory | Out-Null
$Directory = (Resolve-Path $Directory).Path
$flag = if ($Mode -eq 'Warp') { '--interactive-warp' } else { '--interactive' }
$process = Start-Process -FilePath $Exe -ArgumentList $flag, "`"$Directory`"", '--deadline', $Deadline -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $Directory 'runtime.out') -RedirectStandardError (Join-Path $Directory 'runtime.err')
$null = $process.Handle  # keeps the exit code readable after the process ends
$end = (Get-Date).AddSeconds($Deadline + 5)
$sequence = 0
foreach ($command in 'create-device', 'create-queue', 'copy', 'status', 'exit') {
    $sequence++
    & $controller -Directory $Directory -Sequence $sequence -Command $command | Out-Null
    $result = Join-Path $Directory ('result-{0:d6}.json' -f $sequence)
    while (-not (Test-Path -LiteralPath $result) -and (Get-Date) -lt $end -and -not $process.HasExited) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path -LiteralPath $result)) { throw "no receipt for $command" }
    $receipt = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    '{0} {1} success={2} hr={3} copy_success={4} gpu_pending={5}' -f $sequence, $command, $receipt.success, $receipt.hr,
        $receipt.copy_success, $receipt.gpu_pending
}
if (-not $process.WaitForExit(($Deadline + 5) * 1000)) { $process.Kill(); throw 'client did not exit' }
"exit=$($process.ExitCode)"
Get-Content -LiteralPath (Join-Path $Directory 'runtime.out') | Where-Object { $_ -match '^(runtime|FEATURES|CONFORMANCE)' }
