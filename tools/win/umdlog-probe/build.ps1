# Builds umdlog-probe.exe (BD-379) with clang-cl of LLVM 23.1.4, x86-64-v3, against the Windows SDK of the Visual
# Studio environment. Prints the SHA256 of the result, then checks the CLI: --help exits 0 and an unknown argument
# exits 2.
param([string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\BC-250' })\scratch\build\umdlog-probe",
      [string]$VsInstall)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$root = Get-Bc250Root $repo
$tmp = Join-Path $Out 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $tmp; $env:TMP = $tmp
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $tmp -Arch x64
    $cc = Resolve-Bc250UmdCompiler $root 'clang-cl' '' $Out 'x64'
    $exe = Join-Path $Out 'umdlog-probe.exe'
    Invoke-Bc250UmdCompiler $cc /nologo /O2 /EHsc /std:c++17 /W4 /WX /MT -march=x86-64-v3 -mtune=znver2 /Brepro `
        (Join-Path $PSScriptRoot 'umdlog-probe.cpp') "/Fo$tmp\umdlog-probe.obj" "/Fe$exe" /link /Brepro /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw "clang-cl exit $LASTEXITCODE" }
} finally { Restore-ProcessEnvironment $saved }
Remove-Item -Recurse -Force -LiteralPath $tmp
& $exe --help 2>$null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --unknown 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown argument was accepted' }
"umdlog-probe.exe $((Get-FileHash $exe -Algorithm SHA256).Hash)"
