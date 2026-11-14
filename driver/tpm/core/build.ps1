# Builds tpmcored.exe, the user-mode TPM core service, and the host bridge test.
#
#   pwsh driver\tpm\core\build.ps1 -Libtpms <libtpms checkout> -OpenSsl <OpenSSL install with lib64\libcrypto.a> -Out <dir>
#
# libtpms (BSD-3-Clause) and OpenSSL (Apache-2.0) are not in this repository; PROVENANCE.md names the revisions.
# The compiler is llvm-mingw clang: libtpms needs a C library with POSIX-like headers that clang-cl does not give,
# and the posix\ stand-ins cover the rest. This is a recorded deviation from the clang-cl rule for user mode.
param(
    [Parameter(Mandatory)][string]$Libtpms,
    [Parameter(Mandatory)][string]$OpenSsl,
    [Parameter(Mandatory)][string]$Out,
    [string]$Clang = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tpm = Split-Path $here
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
@'
#define WITH_TPM2 1
#define USE_OPENSSL_CRYPTO_LIBRARY 1
'@ | Set-Content -Encoding ascii (Join-Path $Out 'config.h')

$t = Join-Path $Libtpms 'src\tpm2'
# The defines of the upstream build (src\Makefile.am: common_CFLAGS and libtpms_tpm2_la_CFLAGS), plus RADIX_BITS for
# x64 and __linux__, which selects the POSIX branches that the posix\ stand-ins satisfy.
$defines = @('-include', 'tpm_library_conf.h', '-DTPM_LIBTPMS_CALLBACKS', '-DTPM_NV_DISK', '-D_POSIX_', '-DTPM_POSIX',
    '-DRADIX_BITS=64', '-D__linux__')
$libInc = @("-I$Out", "-I$here\posix", "-I$OpenSsl\include",
    "-I$Libtpms\src", "-I$Libtpms\include", "-I$Libtpms\include\libtpms",
    "-I$t", "-I$t\crypto\openssl", "-I$t\TPMCmd\Platform\include", "-I$t\TPMCmd\Simulator\include",
    "-I$t\TPMCmd\Simulator\include\prototypes", "-I$t\TPMCmd\TpmConfiguration", "-I$t\TPMCmd\tpm\include",
    "-I$t\TPMCmd\tpm\include\private", "-I$t\TPMCmd\tpm\include\private\prototypes",
    "-I$t\TPMCmd\tpm\cryptolibs\common\include", "-I$t\TPMCmd\tpm\cryptolibs\TpmBigNum\include",
    "-I$t\TPMCmd\tpm\cryptolibs\TpmBigNum\include\TpmBigNum", "-I$t\TPMCmd\tpm\cryptolibs",
    "-I$t\TPMCmd\tpm\cryptolibs\Ossl", "-I$t\TPMCmd\tpm\cryptolibs\Ossl\include",
    "-I$t\TPMCmd\tpm\cryptolibs\Ossl\include\Ossl")
# Third-party code: its own warnings are not ours to fix, so -w; our files build with -Wall -Werror below.
$libFlags = @('-std=c11', '-O2', '-w') + $defines + $libInc

# The TPM 2 sources of the upstream build (src\Makefile.am, TPM 2 part): every .c under src\tpm2, and the TPM 2
# interface files of src. The TPM 1.2 half is not built.
$sources = @(Get-ChildItem -Recurse -File -Filter *.c $t | Where-Object Name -ne 'TPMCmdp.c' | ForEach-Object FullName)
# TPMCmdp.c is the simulator's power and command signals over a TCP socket; posix\signals.c has its bodies without
# the socket.
$sources += Join-Path $here 'posix\signals.c'
$sources += @('disabled_interface.c', 'tpm_debug.c', 'tpm_library.c', 'tpm_memory.c', 'tpm_nvfile.c',
    'tpm_tpm2_interface.c', 'tpm_tpm2_tis.c') | ForEach-Object { Join-Path $Libtpms "src\$_" }
Write-Host "compile libtpms ($($sources.Count) files)"
$jobs = $sources | ForEach-Object -ThrottleLimit 8 -Parallel {
    $o = Join-Path $using:obj ('lib_' + [IO.Path]::GetFileNameWithoutExtension($_) + '.o')
    $extra = if ($_ -like '*tpm_library.c') { @('-include', "$using:here\posix\vdprintf.h") } else { @() }
    $err = & $using:Clang @using:libFlags @extra -c $_ -o $o 2>&1
    if ($LASTEXITCODE -ne 0) { "FAIL $_ :: $(($err | Where-Object { $_ -match 'error:' } | Select-Object -First 1))" }
}
if ($jobs) { $jobs; throw 'libtpms compile failed' }

$ownFlags = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-municode', '-DUNICODE', '-D_UNICODE') + $defines + $libInc
Write-Host 'compile the core'
& $Clang @ownFlags -c (Join-Path $here 'posix\vdprintf.c') -o (Join-Path $obj 'own_vdprintf.o'); if ($LASTEXITCODE) { throw 'vdprintf.c' }
& $Clang @ownFlags -c (Join-Path $here 'tpmcore.c') -o (Join-Path $obj 'own_tpmcore.o'); if ($LASTEXITCODE) { throw 'tpmcore.c' }
& $Clang @ownFlags -c (Join-Path $here 'tpmcored.c') -o (Join-Path $obj 'own_tpmcored.o'); if ($LASTEXITCODE) { throw 'tpmcored.c' }
& $Clang @ownFlags -c (Join-Path $tpm 'handoff.c') -o (Join-Path $obj 'own_handoff.o'); if ($LASTEXITCODE) { throw 'handoff.c' }
& $Clang @ownFlags -c (Join-Path $tpm 'crb.c') -o (Join-Path $obj 'test_crb.o'); if ($LASTEXITCODE) { throw 'crb.c' }
& $Clang @ownFlags -c (Join-Path $tpm 'test\crb_bridge_test.c') -o (Join-Path $obj 'test_bridge.o'); if ($LASTEXITCODE) { throw 'crb_bridge_test.c' }

$lib = @(Get-ChildItem (Join-Path $obj 'lib_*.o') | Sort-Object Name | ForEach-Object FullName)
$sys = @((Join-Path $OpenSsl 'lib64\libcrypto.a'), '-lws2_32', '-lcrypt32', '-lbcrypt', '-lcfgmgr32', '-ladvapi32', '-luser32',
    '-lpthread')   # winpthreads: clock_gettime of the TPM clock (Clock.c)
# Static C runtime pieces: the lab has no llvm-mingw DLLs.
$link = @('-O2', '-municode', '-static')
Write-Host 'link'
& $Clang @link ($lib + @((Join-Path $obj 'own_vdprintf.o'), (Join-Path $obj 'own_tpmcore.o'), (Join-Path $obj 'own_tpmcored.o'), (Join-Path $obj 'own_handoff.o')) + $sys + @('-o', (Join-Path $Out 'tpmcored.exe')))
if ($LASTEXITCODE) { throw 'link tpmcored.exe' }
& $Clang -O2 -static ($lib + @((Join-Path $obj 'own_vdprintf.o'), (Join-Path $obj 'own_tpmcore.o'), (Join-Path $obj 'test_crb.o'), (Join-Path $obj 'test_bridge.o')) + $sys + @('-o', (Join-Path $Out 'crb_bridge_test.exe')))
if ($LASTEXITCODE) { throw 'link crb_bridge_test.exe' }
foreach ($f in 'tpmcored.exe', 'crb_bridge_test.exe') { '{0,9}  {1}  {2}' -f (Get-Item (Join-Path $Out $f)).Length, (Get-FileHash (Join-Path $Out $f)).Hash.Substring(0, 8), $f }
