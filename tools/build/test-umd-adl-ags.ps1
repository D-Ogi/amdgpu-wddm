# Host tests of the open ADL subset with the real AMD AGS 6.3 library (driver/umd/adl, docs/design/adl-subset.md).
# Needs no AMD GPU: every case runs ags-client.exe of build-umd-adl.ps1 next to amd_ags_<arch>.dll and one ADL
# library, in its own directory, so that AGS loads that library by name from the application directory.
#
#   pwsh tools\build\test-umd-adl-ags.ps1 [-Arch x64|x86] [-BuildDir <dir>] [-AgsSdk <dir>] [-EvidenceDir <dir>]
#
# Cases (the ADL library of each case is copied under the name that the Arch needs):
#   shipped   the shipped library: agsInitialize returns 0 with and without GPU information, no AMD device on a host
#             without our driver, 100 rounds without a leak, and a DX12 device through AGS on any adapter
#   inject    test\atiadl-test.dll with the snapshot of tests\test-snapshot.h (two AMD GPUs, three displays): the
#             AGS parsers over a nonempty adapter list
#   host      test\atiadl-test-host.dll (only when the build has it): the discovery on the GPU of this host
#   none      no ADL library: agsInitialize returns 6 (AGS_NO_AMD_DRIVER_INSTALLED)
#   no-modes  test\atiadl-no-modes.dll, without ADL2_Display_Modes_Get: agsInitialize returns 5 (AGS_LEGACY_DRIVER)
# Outputs in -EvidenceDir (default <BuildDir>\ags-tests): one text file per run, results.json, SHA256SUMS.
# Exit 0 only when every run gives its expected exit code.
param([ValidateSet('x64', 'x86')][string]$Arch = 'x64', [string]$BuildDir, [string]$AgsSdk, [string]$EvidenceDir,
      [int]$TimeoutSeconds = 120)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $BuildDir) { $BuildDir = Join-Path $root "scratch\build\umd-adl-$Arch" }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not $AgsSdk) { $AgsSdk = Join-Path $root 'ref\AGS_SDK-v6.3.0' }
if (-not $EvidenceDir) { $EvidenceDir = Join-Path $BuildDir 'ags-tests' }
$EvidenceDir = [IO.Path]::GetFullPath($EvidenceDir)
$adlName = if ($Arch -eq 'x86') { 'atiadlxy.dll' } else { 'atiadlxx.dll' }
$agsName = if ($Arch -eq 'x86') { 'amd_ags_x86.dll' } else { 'amd_ags_x64.dll' }
$client = Join-Path $BuildDir 'ags-client.exe'
$ags = Join-Path $AgsSdk $agsName
foreach ($f in @($client, $ags, (Join-Path $BuildDir $adlName))) {
    if (-not (Test-Path -LiteralPath $f)) { throw "missing input: $f" }
}
# The negative control "none" is valid only when no ADL library is on the system search path of the process.
$sysDir = if ($Arch -eq 'x86' -and [Environment]::Is64BitOperatingSystem) { "$env:SystemRoot\SysWOW64" } else { "$env:SystemRoot\System32" }
$systemAdl = @('atiadlxx.dll', 'atiadlxy.dll' | Where-Object { Test-Path -LiteralPath (Join-Path $sysDir $_) })

$libraries = [ordered]@{
    'shipped' = Join-Path $BuildDir $adlName
    'inject' = Join-Path $BuildDir 'test\atiadl-test.dll'
    'host' = Join-Path $BuildDir 'test\atiadl-test-host.dll'
    'none' = $null
    'no-modes' = Join-Path $BuildDir 'test\atiadl-no-modes.dll'
}
# Case, arguments, expected exit code.
$runs = @(
    @('shipped', 'info', 0), @('shipped', 'info --no-gpu-info', 0), @('shipped', 'loop 100', 0),
    @('shipped', 'dx12 --any-adapter', 0),
    @('inject', 'info --inject', 0), @('inject', 'info --no-gpu-info --inject', 0), @('inject', 'loop 100 --inject', 0),
    @('inject', 'dx12 --any-adapter --inject', 0),
    @('inject', 'info --kmt-fixture --guard-allocations', 0),
    @('inject', 'loop 20 --kmt-fixture --guard-allocations', 0),
    @('host', 'info', 0), @('host', 'loop 20', 0), @('host', 'dx12 --any-adapter', 0),
    @('none', 'expect 6', 0), @('no-modes', 'expect 5', 0)
)
if ($systemAdl.Count) {
    Write-Warning "an ADL library is in ${sysDir}: $($systemAdl -join ', '); the case 'none' is skipped"
    $runs = @($runs | Where-Object { $_[0] -ne 'none' })
}
if (-not (Test-Path -LiteralPath $libraries['host'])) { $runs = @($runs | Where-Object { $_[0] -ne 'host' }) }

if (Test-Path -LiteralPath $EvidenceDir) { Remove-Item -LiteralPath $EvidenceDir -Recurse -Force }
New-Item -ItemType Directory -Force $EvidenceDir | Out-Null
$stage = Join-Path $EvidenceDir 'stage'
foreach ($case in $libraries.Keys) {
    $dir = Join-Path $stage $case
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath $client, $ags -Destination $dir
    if ($libraries[$case] -and (Test-Path -LiteralPath $libraries[$case])) {
        Copy-Item -LiteralPath $libraries[$case] -Destination (Join-Path $dir $adlName)
    }
}

$results = @()
$failed = 0
foreach ($r in $runs) {
    $case, $arguments, $want = $r
    $dir = Join-Path $stage $case
    $tag = "$case-" + (($arguments -replace '--', '') -replace '[^A-Za-z0-9]+', '-').Trim('-')
    $out = Join-Path $EvidenceDir "$tag.txt"
    $p = Start-Process -FilePath (Join-Path $dir 'ags-client.exe') -ArgumentList "$arguments --out `"$out`"" `
        -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $EvidenceDir "$tag.stdout")
    $handle = $p.Handle
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        $p.Kill()
        $code = 'timeout'
    } else {
        $p.WaitForExit()
        $code = $p.ExitCode
    }
    Remove-Item -LiteralPath (Join-Path $EvidenceDir "$tag.stdout") -ErrorAction SilentlyContinue
    $ok = $code -eq $want
    if (-not $ok) { $failed++ }
    Write-Host ('{0} {1,-40} exit {2} (expected {3})' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), "$Arch $case $arguments", $code, $want)
    $results += [ordered]@{ case = $case; arguments = $arguments; exit = $code; expected = $want; pass = $ok; output = "$tag.txt" }
}
$null = $handle

$inputs = @($client, $ags) + @($libraries.Values | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
[ordered]@{
    arch = $Arch
    utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    adl_name = $adlName
    system_adl_libraries = $systemAdl
    inputs = @($inputs | ForEach-Object { [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash } })
    runs = $results
    failed = $failed
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDir 'results.json') -Encoding utf8
Remove-Item -LiteralPath $stage -Recurse -Force
@(Get-ChildItem -LiteralPath $EvidenceDir -File | Where-Object Name -ne 'SHA256SUMS' | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant(), $_.Name
}) | Set-Content -LiteralPath (Join-Path $EvidenceDir 'SHA256SUMS') -Encoding ascii
Write-Host "$($results.Count - $failed) of $($results.Count) runs as expected; evidence in $EvidenceDir"
exit $(if ($failed) { 1 } else { 0 })
