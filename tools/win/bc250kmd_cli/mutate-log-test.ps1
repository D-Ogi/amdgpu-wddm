# Mutation controls for log_test.c (BD-054). Each mutant of bc250kmd_cli.c must make the test fail, or the test
# proves nothing. Builds under -Out, never in the tree it reads.
#
#   pwsh tools\win\bc250kmd_cli\mutate-log-test.ps1 -Out <BC250_ROOT>\scratch\build\log-test-mutants
#
# Recorded run: evidence/windows/2026-10-03-BD-054-log-poll-controls/mutate-log-test.txt.
param(
    [string]$Tree = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [Parameter(Mandatory)][string]$Out,
    [string]$Kits = $(if ($env:BC250_ROOT) { Join-Path $env:BC250_ROOT 'toolchain\nuget' } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\toolchain\nuget')).Path }),
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$cliDir = Join-Path $Tree 'tools\win\bc250kmd_cli'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = ''; $env:LIB = ''

$source = Get-Content -LiteralPath (Join-Path $cliDir 'bc250kmd_cli.c') -Raw
# The CLI of KMD 196, the one the lab ran while BD-054 was open: every log page a HardwareAccess escape.
$original196 = (& git -C $Tree show 28b89a4c:tools/win/bc250kmd_cli/bc250kmd_cli.c) -join "`n"
$mutants = [ordered]@{
    'unchanged' = $source
    'cli196-original' = $original196
    'pages-hardware-access' = $source.Replace('SendReadEscape(BC250_DEFAULT_HWID, log.Command, &log, sizeof(log), &status)',
                                              'SendEscape(BC250_DEFAULT_HWID, &log, sizeof(log), &status)')
    'no-sentinel-fixup' = $source.Replace('if (from == BC250_LOG_FROM_SUMMARY) from = log.From;', '')
    'no-count-line' = $source.Replace('escapes: %lu without adapter synchronization, %lu with HardwareAccess\n', 'escapes: %lu, %lu\n')
}
$results = @()
foreach ($name in $mutants.Keys) {
    $dir = Join-Path $Out $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    if ($name -ne 'unchanged' -and $mutants[$name] -eq $source) { throw "mutant $name did not change the source" }
    # Same relative include path as the tree: <dir>\tools\win\bc250kmd_cli with driver\kmd beside it.
    $mcli = Join-Path $dir 'tools\win\bc250kmd_cli'
    $mkmd = Join-Path $dir 'driver\kmd'
    New-Item -ItemType Directory -Force $mcli, $mkmd | Out-Null
    Copy-Item -LiteralPath (Join-Path $Tree 'driver\kmd\bc250kmd_escape.h') -Destination $mkmd -Force
    Copy-Item -LiteralPath (Join-Path $cliDir 'log_test.c') -Destination $mcli -Force
    [IO.File]::WriteAllText((Join-Path $mcli 'bc250kmd_cli.c'), $mutants[$name])
    & $cl @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/Fo$dir\log_test.obj", "/Fe$dir\log_test.exe",
        (Join-Path $mcli 'log_test.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
        "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", 'gdi32.lib', 'setupapi.lib', 'advapi32.lib') | Out-Null
    if ($LASTEXITCODE -ne 0) { $results += "$name : does not compile"; continue }
    $text = & "$dir\log_test.exe" 2>&1 | Out-String
    $code = $LASTEXITCODE
    $summary = ($text -split "`r?`n" | Where-Object { $_ -match '^log_test:' }) -join ''
    $fails = @($text -split "`r?`n" | Where-Object { $_ -match '^FAIL ' }).Count
    $results += ('{0} : exit {1}, {2}, {3} FAIL lines' -f $name, $code, $summary, $fails)
}
$results
if (@($results | Where-Object { $_ -notmatch '^unchanged ' -and $_ -match ', 0 failures, 0 FAIL lines$' })) {
    throw 'a mutant passed log_test: the test does not see the defect it is there to catch'
}
