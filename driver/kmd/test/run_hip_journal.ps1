param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [switch]$OmitProcessCheck,
    [switch]$ExpectFailure
)
$ErrorActionPreference = 'Stop'
if ($ExpectFailure -and -not $OmitProcessCheck) { throw 'Expected failure requires a source mutation' }
$kmd = Split-Path $PSScriptRoot
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$env:TEMP = $Out; $env:TMP = $Out
# Compile the complete production unit, substituting only the kernel include.
$source = [IO.File]::ReadAllText((Join-Path $kmd 'hip_journal.c'))
if (($source | Select-String -Pattern '#include "bc250kmd.h"' -AllMatches).Matches.Count -ne 1) { throw 'Production include changed' }
$source = $source.Replace('#include "bc250kmd.h"', '/* Kernel primitives supplied by host harness. */')
if ($OmitProcessCheck) {
    if (-not $source.Contains('owner->Process == Process')) { throw 'Process ownership predicate changed' }
    $source = $source.Replace('owner->Process == Process', 'Process != NULL')
}
[IO.File]::WriteAllText((Join-Path $Out 'hip_journal_production.inc'), $source)
$env:INCLUDE = ''; $env:LIB = ''
$args = @('/nologo','/TC','/std:c11','/W4','/WX','/wd4127','/Od','/Zi','/Zp8', '/DNTDDI_VERSION=0x0A00000C',
    "/I$Out", "/I$kmd", "/I$($msvc.FullName)\include", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/Fo$Out\hip_journal_test.obj", "/Fd$Out\hip_journal_test.pdb", "/Fe$Out\hip_journal_test.exe",
    (Join-Path $PSScriptRoot 'hip_journal_test.c'), '/link',
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64")
& $cl @args
if ($LASTEXITCODE) { throw "Host compile failed: $LASTEXITCODE" }
$testOutput = & (Join-Path $Out 'hip_journal_test.exe') 2>&1
$testExit = $LASTEXITCODE
$testOutput | Tee-Object -FilePath (Join-Path $Out 'result.txt')
if ($ExpectFailure) {
    if ($testExit -ne 1 -or -not ($testOutput -match 'FAIL')) {
        throw "Mutation must fail an executed assertion, exit=$testExit"
    }
    Write-Host 'Expected process-ownership negative control failed'
    exit 0
}
if ($testExit) { throw "Host test failed: $testExit" }
