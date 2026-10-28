param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# Host gate of the per-application graphics settings reader (driver/umd/app-settings): precedence, ranges, ignored
# values and their log lines, the engine options, the frame rate limit and the registry reader on a private hive.
# The D3D11 shell, the D3D12 shell and the router builds run it before they produce a DLL. Everything the test writes
# stays below -OutputDir; HKEY_LOCAL_MACHINE is replaced by a private hive for the test process only.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells, as the router and the shells build by default. -Compiler msvc builds with cl.exe and needs a concrete
# -MsvcReason (compiler policy). The script writes compiler.json into -OutputDir and prints the compiler path and
# version.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-app-settings' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $coreFlags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/c', '/O2', '/W4', '/WX') @() $Arch @($repo)
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @() $Arch @($repo)
    # /c /Od: the flavour string check is read as an object file, and /Od keeps every instantiated function.
    $stringFlags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/c', '/std:c++20', '/EHsc', '/W4', '/WX', '/Od', '/MT', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @() $Arch @($repo)
    Push-Location $OutputDir
    try {
        # The router's half with the router DLL's flags: C++14 (the compiler default), no /EHsc.
        Invoke-Bc250UmdCompiler $cc @coreFlags "/Fo:core-c++14-check.obj" "$repo\driver\umd\app-settings\core-c++14-check.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'app-settings-core.h does not build with the router flags' }
        # BD-167: the release flavour of log_once holds no lab line of dxvk-profile.h; the lab flavour holds them.
        $labLines=@('DXVK profile option %s=%s not applied: %s', 'no DXVK profile options (engine result %08lx)')
        foreach ($flavour in 'release', 'lab') {
            # An array even with one item: PowerShell unrolls a one-item array that an if statement returns.
            $define=@()
            if ($flavour -eq 'lab') { $define=@('/DAMDGPU_WDDM_LAB_FLAVOUR') }
            $obj="flavour-strings-$flavour.obj"
            Invoke-Bc250UmdCompiler $cc @stringFlags @define "/Fo:$obj" "$repo\driver\umd\app-settings\flavour-strings-check.cpp"
            if ($LASTEXITCODE -ne 0) { throw "The $flavour flavour string check does not build" }
            $bytes=[Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes((Join-Path $OutputDir $obj)))
            foreach ($line in $labLines) {
                $has=$bytes.Contains($line)
                if ($flavour -eq 'release' -and $has) { throw "The release flavour of log_once holds the lab line '$line'" }
                if ($flavour -eq 'lab' -and -not $has) { throw "The lab flavour of log_once lacks the lab line '$line'" }
            }
            "flavour strings: $flavour ok"
            # The test checks its own image name, so each flavour gets a directory of its own.
            New-Item -ItemType Directory -Force (Join-Path $OutputDir $flavour) | Out-Null
            $exe="$flavour\app-settings-test.exe"
            Invoke-Bc250UmdCompiler $cc @flags @define "/Fe:$exe" "$repo\driver\umd\app-settings\app-settings-test.cpp"
            if ($LASTEXITCODE -ne 0) { throw "App settings test build failed ($flavour flavour)" }
            & ".\$exe"
            if ($LASTEXITCODE -ne 0) { throw "App settings tests failed ($flavour flavour)" }
        }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
