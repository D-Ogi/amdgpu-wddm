param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\bd215-collector",
    [string]$KitVersion = '10.0.26100.0',
    # LLVM directory whose bin\ holds clang-cl.exe and lld-link.exe.
    [string]$Llvm = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64",
    # Directory of the BD-201 scratch client after client-record.patch: it holds bd201scratch_modules.h.
    # Without it, the identity test that client-host-test.py runs is not built.
    [string]$ClientDir = ''
)

# Builds, all with clang-cl 23.1.4 and lld-link (README.md):
#   bd215collector.exe   x64 collector
#   bd215fault32.exe     x86 test process with intentional faults (selftest.py)
#   <arch>\identity-test.exe, <arch>\amdgpu_wddm_bd215_fixture.dll   only with -ClientDir
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$env:TEMP = $Out; $env:TMP = $Out; $env:INCLUDE = ''; $env:LIB = ''; $env:CL = ''; $env:_CL_ = ''
$cc = Join-Path $Llvm 'bin\clang-cl.exe'
$link = Join-Path $Llvm 'bin\lld-link.exe'
# The compiler is named by path and must exist; nothing falls back to cl or to a clang on PATH.
if (-not (Test-Path -LiteralPath $cc)) { throw "compiler not found: $cc" }
$version = (& $cc --version | Select-Object -First 1)
if ($version -notmatch '23\.1\.4') { throw "Unexpected compiler: $version" }
Write-Host "compiler: $version"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$vc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$includes = @("/imsvc$($vc.FullName)\include", "/imsvc$sdk\Include\$KitVersion\ucrt", "/imsvc$sdk\Include\$KitVersion\um", "/imsvc$sdk\Include\$KitVersion\shared")
$env:PATH = (Split-Path -Parent $cc) + ';' + $env:PATH
$common = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS', '/D_WIN32_WINNT=0x0A00', '-fuse-ld=lld') + $includes
$header = if ($ClientDir) { Join-Path $ClientDir 'bd201scratch_modules.h' } else { '' }
if ($ClientDir -and -not (Test-Path -LiteralPath $header)) { throw "bd201scratch_modules.h not found in $ClientDir" }
foreach ($arch in @('x64', 'x86')) {
    $libs = Join-Path $Kits "microsoft.windows.sdk.cpp.$arch\c"
    $tail = @('/link', '/Brepro', "/LIBPATH:$($vc.FullName)\lib\$arch", "/LIBPATH:$libs\ucrt\$arch", "/LIBPATH:$libs\um\$arch", 'kernel32.lib')
    if ($arch -eq 'x64') {
        & $cc @common /TP /std:c++17 /EHsc -march=x86-64-v3 -mtune=znver2 "/Fo$Out\collector.obj" "/Fe$Out\bd215collector.exe" (Join-Path $PSScriptRoot 'collector.cpp') @tail dbghelp.lib bcrypt.lib
    } else {
        & $cc @common --target=i686-pc-windows-msvc /TC /arch:AVX2 "/Fo$Out\fault-x86.obj" "/Fe$Out\bd215fault32.exe" (Join-Path $PSScriptRoot 'fault-x86.c') @tail
    }
    if ($LASTEXITCODE -ne 0) { throw "Build $arch failed ($LASTEXITCODE)" }
    if ($ClientDir) {
        $target = if ($arch -eq 'x86') { @('--target=i686-pc-windows-msvc', '/arch:AVX2') } else { @('-march=x86-64-v3', '-mtune=znver2') }
        $fixtureOut = Join-Path $Out $arch
        New-Item -ItemType Directory -Force -Path $fixtureOut | Out-Null
        & $cc @common @target /TC /LD "/Fo$fixtureOut\fixture.obj" "/Fe$fixtureOut\amdgpu_wddm_bd215_fixture.dll" (Join-Path $PSScriptRoot 'identity-fixture.c') @tail "/IMPLIB:$fixtureOut\fixture.lib"
        if ($LASTEXITCODE -ne 0) { throw "Identity fixture $arch failed" }
        & $cc @common @target /TC "/I$ClientDir" "/Fo$fixtureOut\identity-test.obj" "/Fe$fixtureOut\identity-test.exe" (Join-Path $PSScriptRoot 'identity-test.c') @tail bcrypt.lib
        if ($LASTEXITCODE -ne 0) { throw "Identity test $arch failed" }
    }
}
[ordered]@{compiler = $version; compiler_path = $cc; linker_path = $link; sdk = $KitVersion; utc = [DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'build.json') -Encoding utf8
