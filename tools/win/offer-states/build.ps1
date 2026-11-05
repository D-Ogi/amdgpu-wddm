# Builds offer-states.exe (BD-308 repro) with clang-cl of LLVM 23.1.4, x86-64-v3, against the Windows SDK of the Visual
# Studio environment. Prints the SHA256 of the result, then checks the CLI: --help exits 0 and an unknown argument
# exits 2.
param([string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\BC-250' })\scratch\build\offer-states",
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
    $exe = Join-Path $Out 'offer-states.exe'
    Invoke-Bc250UmdCompiler $cc /nologo /O2 /EHsc /std:c++17 /W4 /WX /MT -march=x86-64-v3 -mtune=znver2 /Brepro `
        (Join-Path $PSScriptRoot 'offer-states.cpp') "/Fo$tmp\offer-states.obj" "/Fe$exe" /link /Brepro /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw "clang-cl exit $LASTEXITCODE" }
} finally { Restore-ProcessEnvironment $saved }
Remove-Item -Recurse -Force -LiteralPath $tmp
& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --unknown 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown argument was accepted' }
& $exe --out x.json 2>$null
if ($LASTEXITCODE -ne 2) { throw 'a run without --save was accepted' }
& $exe --out x.json --save y.txt --states 3 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown offer state was accepted' }
"offer-states.exe $((Get-FileHash $exe -Algorithm SHA256).Hash)"
