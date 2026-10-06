param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    # The application-local DLL that stands in for the game's own: the DXVK 3.1.1 dxgi.dll of the
    # Witcher 3 DX12 package (E40). -ExpectAppLocalSha256 pins it, so a changed copy stops the run.
    [Parameter(Mandatory)][string]$AppLocalDxgi,
    [string]$ExpectAppLocalSha256 = '2E674A56A48B2739B2B636105F7CDFA14A4C1964C278BCE6F9EE4C31ED50C05F',
    [string[]]$Modes = @('control', 'shadow', 'shadow-rebind', 'shadow-actctx', 'shadow-search', 'sysfirst'),
    [int]$TimeoutSeconds = 60
)
# Runs each shadowtest mode in its own process on the development PC and writes one log per mode plus a
# manifest. Headless: console subsystem, CreateNoWindow, no swap chain, a hard per-mode deadline, nothing
# resident. The lab is not involved.
#
#   pwsh -NoProfile -File tools\win\wsi-dxgi\run-shadowtest.ps1 -Exe <build>\shadowtest.exe `
#        -Out <BC250_ROOT>\scratch\m16\wsi-dxgi\logs -AppLocalDxgi <package>\dxgi.dll
#
# The run creates two application directories under -Out: one without and one with the application-local
# dxgi.dll. The control mode uses the first, every other mode the second.
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$logs = (New-Item -ItemType Directory -Force $Out).FullName
$run = (New-Item -ItemType Directory -Force (Join-Path $logs 'appdirs')).FullName

$appLocalHash = (Get-FileHash -LiteralPath $AppLocalDxgi).Hash
if ($ExpectAppLocalSha256 -and $appLocalHash -ne $ExpectAppLocalSha256) {
    throw "unexpected application-local dxgi.dll: $AppLocalDxgi has sha256 $appLocalHash, expected $ExpectAppLocalSha256"
}

$plain = Join-Path $run 'plain'; $app = Join-Path $run 'appdir'
New-Item -ItemType Directory -Force $plain, $app | Out-Null
Copy-Item -LiteralPath $exe -Destination $plain -Force
Copy-Item -LiteralPath $exe -Destination $app -Force
Copy-Item -LiteralPath $AppLocalDxgi -Destination (Join-Path $app 'dxgi.dll') -Force

$manifest = [ordered]@{
    utc = (Get-Date).ToUniversalTime().ToString('o')
    host = 'development PC, headless console process'
    shadowtest_sha256 = (Get-FileHash -LiteralPath $exe).Hash
    app_local_dxgi = [ordered]@{ sha256 = $appLocalHash }
    system_modules = [ordered]@{}
    runs = [ordered]@{}
}
foreach ($m in 'dxgi.dll', 'd3d11.dll', 'dcomp.dll', 'd3d12.dll') {
    $p = Join-Path $env:SystemRoot "System32\$m"
    $manifest.system_modules[$m] = [ordered]@{ version = (Get-Item -LiteralPath $p).VersionInfo.FileVersion; sha256 = (Get-FileHash -LiteralPath $p).Hash }
}

foreach ($mode in $Modes) {
    $dir = if ($mode -eq 'control') { $plain } else { $app }
    $psi = [Diagnostics.ProcessStartInfo]::new((Join-Path $dir 'shadowtest.exe'))
    $psi.ArgumentList.Add($mode)
    $psi.WorkingDirectory = $dir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $modeLogs = (New-Item -ItemType Directory -Force (Join-Path $logs $mode)).FullName
    $psi.Environment['DXVK_LOG_PATH'] = $modeLogs
    $psi.Environment['DXVK_LOG_LEVEL'] = 'info'
    $psi.Environment['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { $p.Kill($true); $p.WaitForExit() }
    $watch.Stop()
    $out = $outTask.Result; $err = $errTask.Result
    [IO.File]::WriteAllText((Join-Path $logs "$mode.txt"), $out + $(if ($err) { "`n--- stderr`n$err" } else { '' }))
    $manifest.runs[$mode] = [ordered]@{
        exit = $(if ($finished) { $p.ExitCode } else { 'timeout-killed' })
        seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        dxvk_logs = @(Get-ChildItem -LiteralPath $modeLogs -File | ForEach-Object { $_.Name })
    }
    Write-Host ("{0,-14} exit={1} {2:N2}s" -f $mode, $manifest.runs[$mode].exit, $watch.Elapsed.TotalSeconds)
}
($manifest | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath (Join-Path $logs 'manifest.json') -Encoding utf8
