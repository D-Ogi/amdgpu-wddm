# Builds and runs the host test of the CRB state machine (crb_test.c) with the address and undefined-behaviour
# sanitizers of the llvm-mingw clang of the workspace. The bridge test (crb_bridge_test.c) needs the TPM core and
# is built by ..\core\build.ps1.
#
#   pwsh driver\tpm\test\run.ps1
#   pwsh driver\tpm\test\run.ps1 -Out P:\BC-250\scratch\build\tpm-crb-test
#
# Everything is written under -Out, never into the repository.
param(
    [string]$Out = 'P:\BC-250\scratch\build\tpm-crb-test',
    [string]$Mingw = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tpm = (Resolve-Path (Join-Path $here '..')).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$env:PATH = "$Mingw;$env:PATH"
$exe = Join-Path $Out 'crb_test.exe'
$flags = @('-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', "-I$tpm")
& (Join-Path $Mingw 'x86_64-w64-mingw32-clang.exe') @flags (Join-Path $here 'crb_test.c') (Join-Path $tpm 'crb.c') -o $exe
if ($LASTEXITCODE -ne 0) { throw "the build of crb_test failed ($LASTEXITCODE)" }
& $exe
exit $LASTEXITCODE
