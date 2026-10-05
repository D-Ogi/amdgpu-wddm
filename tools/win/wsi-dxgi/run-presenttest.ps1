param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    # The application-local DLLs that stand in for a game's own. Each hash below is the copy E56 used.
    # A scenario is skipped when its DLLs are not given, so one run can cover a subset.
    [string]$Dxvk311Dxgi,
    [string]$Vkd3dD3d12,
    [string]$Vkd3dCore,
    [string]$DxvkD3d11,
    [string]$DxvkDxgi,
    [hashtable]$ExpectSha256 = @{
        Dxvk311Dxgi = '2E674A56A48B2739B2B636105F7CDFA14A4C1964C278BCE6F9EE4C31ED50C05F'
        Vkd3dD3d12  = '7B77ED5C107033DCB33D339D5AE54122D94A37A78DD817590FC3D4142C431887'
        Vkd3dCore   = '90B1DAD6441BB2A6569681011D75770D5BCFE79882E4AA3DBBA21FF419EC9ECC'
    },
    [string[]]$Modes = @('control-plain', 'control-seal', 'dxvk11-plain', 'dxvk11-seal', 'vkd3d-plain', 'vkd3d-seal'),
    [int]$TimeoutSeconds = 60
)
# Runs the in-process System32 D3D11 presenter route in simulated game directories, with and without the
# proposed seal, on the development PC. Headless: console subsystem, CreateNoWindow, a composition swap
# chain without an HWND target, a hard per-mode deadline, nothing resident. The lab is not involved.
#
#   pwsh -NoProfile -File tools\win\wsi-dxgi\run-presenttest.ps1 -Exe <build>\presenttest.exe `
#        -Out <BC250_ROOT>\scratch\m16\wsi-dxgi\logs2 -Dxvk311Dxgi <package>\dxgi.dll `
#        -Vkd3dD3d12 <package>\d3d12.dll -Vkd3dCore <package>\d3d12core.dll `
#        -DxvkD3d11 <per-app>\d3d11.dll -DxvkDxgi <per-app>\dxgi.dll
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$logs = (New-Item -ItemType Directory -Force $Out).FullName
$run = (New-Item -ItemType Directory -Force (Join-Path $logs 'appdirs')).FullName

# Every given DLL is pinned by hash, so a changed copy stops the run instead of producing a result that
# claims a build it did not use.
foreach ($name in 'Dxvk311Dxgi', 'Vkd3dD3d12', 'Vkd3dCore', 'DxvkD3d11', 'DxvkDxgi') {
    $path = (Get-Variable -Name $name -ValueOnly)
    if (-not $path) { continue }
    $hash = (Get-FileHash -LiteralPath $path).Hash
    if ($ExpectSha256.ContainsKey($name) -and $ExpectSha256[$name] -and $hash -ne $ExpectSha256[$name]) {
        throw "unexpected $name : $path has sha256 $hash, expected $($ExpectSha256[$name])"
    }
}

$dirs = [ordered]@{
    control = @()
    dxvk11 = @(@($DxvkD3d11, 'd3d11.dll'), @($DxvkDxgi, 'dxgi.dll'))
    vkd3d = @(@($Dxvk311Dxgi, 'dxgi.dll'), @($Vkd3dD3d12, 'd3d12.dll'), @($Vkd3dCore, 'd3d12core.dll'))
}
$manifest = [ordered]@{
    utc = (Get-Date).ToUniversalTime().ToString('o')
    host = 'development PC, headless console process, composition swap chain without HWND target'
    presenttest_sha256 = (Get-FileHash -LiteralPath $exe).Hash
    app_local = [ordered]@{}
    system_modules = [ordered]@{}
    runs = [ordered]@{}
}
$ready = @()
foreach ($scenario in $dirs.Keys) {
    if (@($dirs[$scenario] | Where-Object { -not $_[0] }).Count) {
        Write-Host "skip  $scenario (its application-local DLLs were not given)"
        continue
    }
    $ready += $scenario
    $d = (New-Item -ItemType Directory -Force (Join-Path $run $scenario)).FullName
    Get-ChildItem -LiteralPath $d -File | Remove-Item -Force
    Copy-Item -LiteralPath $exe -Destination $d -Force
    $manifest.app_local[$scenario] = @()
    foreach ($pair in $dirs[$scenario]) {
        Copy-Item -LiteralPath $pair[0] -Destination (Join-Path $d $pair[1]) -Force
        $manifest.app_local[$scenario] += [ordered]@{ name = $pair[1]; sha256 = (Get-FileHash -LiteralPath $pair[0]).Hash }
    }
}
foreach ($m in 'dxgi.dll', 'd3d11.dll', 'dcomp.dll', 'd3d12.dll', 'dxcore.dll') {
    $p = Join-Path $env:SystemRoot "System32\$m"
    $manifest.system_modules[$m] = [ordered]@{ version = (Get-Item -LiteralPath $p).VersionInfo.FileVersion; sha256 = (Get-FileHash -LiteralPath $p).Hash }
}

foreach ($mode in $Modes) {
    $scenario = $mode.Split('-')[0]
    if ($ready -notcontains $scenario) { continue }
    $dir = Join-Path $run $scenario
    $psi = [Diagnostics.ProcessStartInfo]::new((Join-Path $dir 'presenttest.exe'))
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
    $result = ($out -split "`n" | Where-Object { $_ -like 'RESULT*' } | Select-Object -Last 1)
    $manifest.runs[$mode] = [ordered]@{
        exit = $(if ($finished) { $p.ExitCode } else { 'timeout-killed' })
        seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        result = $(if ($result) { $result.Trim() } else { $null })
        dxvk_logs = @(Get-ChildItem -LiteralPath $modeLogs -File | ForEach-Object { $_.Name })
    }
    Write-Host ("{0,-14} exit={1} {2:N2}s dxvk_logs={3} {4}" -f $mode, $manifest.runs[$mode].exit,
        $watch.Elapsed.TotalSeconds, $manifest.runs[$mode].dxvk_logs.Count, $manifest.runs[$mode].result)
}
($manifest | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath (Join-Path $logs 'manifest.json') -Encoding utf8
