param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64')
# Host gate of the per-application graphics settings reader (driver/umd/app-settings): precedence, ranges, ignored
# values and their log lines, the engine options, the frame rate limit and the registry reader on a private hive.
# The D3D11 shell, the D3D12 shell and the router builds run it before they produce a DLL. Everything the test writes
# stays below -OutputDir; HKEY_LOCAL_MACHINE is replaced by a private hive for the test process only.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-app-settings' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    Push-Location $OutputDir
    try {
        # The router's half with the router DLL's flags: C++14 (the compiler default), no /EHsc.
        & cl.exe /nologo /c /O2 /W4 /WX "/Fo:core-c++14-check.obj" "$repo\driver\umd\app-settings\core-c++14-check.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'app-settings-core.h does not build with the router flags' }
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Fe:app-settings-test.exe "$repo\driver\umd\app-settings\app-settings-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'App settings test build failed' }
        & .\app-settings-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'App settings tests failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
