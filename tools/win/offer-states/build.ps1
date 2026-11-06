# Builds offer-states.exe (BD-308 repro) with clang-cl of LLVM 23.1.4, x86-64-v3, against the Windows SDK of the Visual
# Studio environment. -Arch x86 builds offer-states-x86.exe, a 32-bit program that loads the WOW64 D3D11 shell: the
# same cases for a 32-bit process. Prints the SHA256 of the result, then checks the CLI: --help exits 0 and an unknown
# argument exits 2.
param([ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\BC-250' })\scratch\build\offer-states$(if ($Arch -eq 'x86') { '-x86' })",
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
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $tmp -Arch $Arch
    $cc = Resolve-Bc250UmdCompiler $root 'clang-cl' '' $Out $Arch
    $target = if ($Arch -eq 'x86') { 'i686-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
    $exe = Join-Path $Out $(if ($Arch -eq 'x86') { 'offer-states-x86.exe' } else { 'offer-states.exe' })
    Invoke-Bc250UmdCompiler $cc /nologo "--target=$target" /O2 /EHsc /std:c++17 /W4 /WX /MT -march=x86-64-v3 -mtune=znver2 /Brepro `
        (Join-Path $PSScriptRoot 'offer-states.cpp') "/Fo$tmp\offer-states.obj" "/Fe$exe" /link /Brepro /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw "clang-cl exit $LASTEXITCODE" }
} finally { Restore-ProcessEnvironment $saved }
Remove-Item -Recurse -Force -LiteralPath $tmp
$machine = [BitConverter]::ToUInt16([IO.File]::ReadAllBytes($exe), [BitConverter]::ToInt32([IO.File]::ReadAllBytes($exe), 0x3C) + 4)
$want = if ($Arch -eq 'x86') { 0x14C } else { 0x8664 }
if ($machine -ne $want) { throw ("{0} has PE machine {1:X4}, expected {2:X4}" -f $exe, $machine, $want) }
& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --unknown 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown argument was accepted' }
& $exe --out x.json 2>$null
if ($LASTEXITCODE -ne 2) { throw 'a run without --save was accepted' }
& $exe --out x.json --save y.txt --states 3 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown offer state was accepted' }
& $exe --out x.json --save y.txt --texture other 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown texture kind was accepted' }
"$(Split-Path -Leaf $exe) $((Get-FileHash $exe -Algorithm SHA256).Hash)"
