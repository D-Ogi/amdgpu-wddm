# Builds the TPM engine of the stage (tpm_engine.lib) and its host test, then runs the host test.
#
#   pwsh tools\efi\tpm\engine\build.ps1
#   pwsh tools\efi\tpm\engine\build.ps1 -Out P:\BC-250\scratch\build\tpm-engine -SkipTest
#
# tpm_engine.lib is one static archive for the stage target (clang for x86_64-unknown-windows, freestanding, the
# flags of tools\efi\stage\build.ps1): libtpms (TPM 2 part), OpenSSL libcrypto, the C runtime (tpm\crt) and the
# engine (tpm\engine). The host test links the same archive into a Windows console program.
#
# libtpms (BSD-3-Clause) and OpenSSL (Apache-2.0) are build inputs, not in this repository (tools\efi\tpm\PROVENANCE.md
# names the revisions). OpenSSL is extracted from its release archive into -Out and configured there; the libtpms
# checkout is read only. Everything is written under -Out. Nothing here touches a lab machine or a boot entry.
#
# The heavy compile runs under the CPU slot of the workspace (scratch\host-slot\slot.py) when -Slot is given.

param(
    [string]$Out = 'P:\BC-250\scratch\build\tpm-engine',
    [string]$Llvm = 'P:\BC-250\toolchain\llvm-23.1.4-win64\bin',
    [string]$Mingw = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64',
    [string]$Libtpms = 'P:\BC-250\scratch\t39\tpm-core-check\src',
    [string]$LibtpmsCommit = '165c303',
    [string]$OpenSslArchive = 'P:\BC-250\scratch\t39\tpm-core-check\openssl-3.5.9.tar.gz',
    [string]$OpenSslSha256 = '603F5602E2EEF00D77FBD429D34DCD5822BB301757A1BC9CDB24C670F1EB859A',
    # The perl of Git for Windows. Its Params::Check needs Locale::Maketext::Simple, which it does not ship.
    [string]$Perl = 'C:\Program Files\Git\usr\bin\perl.exe',
    [string]$PerlLib = '/p/BC-250/scratch/t39/tpm-core-check/perl-lib',
    [int]$Jobs = 8,
    [switch]$SkipTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tpmRoot = (Resolve-Path (Join-Path $here '..')).Path
$efiRoot = (Resolve-Path (Join-Path $here '..\..')).Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$crt = Join-Path $tpmRoot 'crt'
$posix = Join-Path $repo 'driver\tpm\core\posix'

$clang = Join-Path $Llvm 'clang.exe'
$ar = Join-Path $Llvm 'llvm-ar.exe'
$nm = Join-Path $Llvm 'llvm-nm.exe'
$lld = Join-Path $Llvm 'lld-link.exe'
$make = Join-Path $Mingw 'bin\mingw32-make.exe'

$objTpms = Join-Path $Out 'obj-libtpms'
$objOwn = Join-Path $Out 'obj-own'
$objTest = Join-Path $Out 'obj-test'
New-Item -ItemType Directory -Force $Out, $objTpms, $objOwn, $objTest | Out-Null
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}
function Slash([string]$path) { $path.Replace('\', '/') }

# The flags of the stage (tools\efi\stage\build.ps1), with one section for each function and datum so that /opt:ref
# of the stage link keeps only what the stage calls.
$stageFlags = @('--target=x86_64-unknown-windows', '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlibinc',
    '-fno-stack-protector', '-mno-stack-arg-probe', '-mno-red-zone', '-fno-ident', '-ffunction-sections', '-fdata-sections')
# The stage target is the Microsoft x64 ABI, so clang defines _WIN32, _WIN64 and _MSC_VER. OpenSSL and libtpms then
# take their Windows paths (windows.h, _ftime_s, _byteswap_*), which a freestanding image does not have. edk2
# CryptoPkg undefines the same three macros for OpenSSL. The ABI stays that of the target.
$noWindows = @('-U_WIN32', '-U_WIN64', '-U_MSC_VER')
# libtpms needs __GNUC__ or _MSC_VER (TPMCmd\tpm\include\tpm_public\CompilerDependencies.h). The Windows core builds
# it with llvm-mingw clang, which says GCC 4.2.1; the engine takes the same branch, so that the clock, the entropy
# and the byte order code are the same on both sides of the handoff.
$asGcc = @('-D__GNUC__=4', '-D__GNUC_MINOR__=2', '-D__GNUC_PATCHLEVEL__=1')
$platformHeader = Join-Path $here 'ossl_platform.h'

# ---- the inputs
$commit = (& git -C $Libtpms rev-parse --short=7 HEAD).Trim()
if ($commit -ne $LibtpmsCommit) { throw "libtpms at $Libtpms is $commit, not $LibtpmsCommit" }
if (& git -C $Libtpms status --porcelain) { throw "libtpms at $Libtpms has local changes" }
if ((Get-FileHash -Algorithm SHA256 $OpenSslArchive).Hash -ne $OpenSslSha256) { throw "$OpenSslArchive is not the pinned OpenSSL 3.5.9" }

# ---- OpenSSL libcrypto
# The options follow edk2 CryptoPkg (OpensslLib) where they apply: no threads, no files, no sockets, no dynamic
# loading, no configuration file, no error strings, and the seed source of tpm\engine\ossl_seed.c. The algorithms
# that libtpms uses stay: RSA, ECC, AES, Camellia, TDES, SM4, the SHA families and the KDFs. The post-quantum
# algorithms are out: libtpms 165c303 does not use them.
$sslOptions = @('no-asm', 'no-shared', 'no-pic', 'no-threads', 'no-tests', 'no-apps', 'no-docs', 'no-sock', 'no-dgram',
    'no-dso', 'no-engine', 'no-dynamic-engine', 'no-module', 'no-autoload-config', 'no-afalgeng', 'no-capieng',
    'no-padlockeng', 'no-devcryptoeng', 'no-loadereng', 'no-winstore', 'no-ktls', 'no-async', 'no-ui-console',
    'no-filenames', 'no-posix-io', 'no-secure-memory', 'no-stdio', 'no-err', 'no-atexit', 'no-makedepend', 'no-http',
    'no-ocsp', 'no-cmp', 'no-cms', 'no-ts', 'no-ct', 'no-srp', 'no-ssl', 'no-tls', 'no-dtls', 'no-quic', 'no-ml-kem',
    'no-ml-dsa', 'no-slh-dsa', '--with-rand-seed=none')
$sslConf = Join-Path $here 'openssl\bc250-uefi.conf'
$sslCflags = ($stageFlags + $noWindows + @('-w', "-I$(Slash "$crt\include")", '-include', (Slash $platformHeader))) -join ' '
$ssl = Join-Path $Out 'openssl-3.5.9'
$stamp = Join-Path $Out 'openssl.stamp'
$wanted = ((Get-FileHash -Algorithm SHA256 $sslConf).Hash, ($sslOptions -join ' '), $sslCflags,
    (Get-FileHash -Algorithm SHA256 $platformHeader).Hash) -join "`n"
$buildPath = "$Llvm;$Mingw\bin;$(Split-Path -Parent $Perl);$env:PATH"
$env:PERL5LIB = $PerlLib
if (-not (Test-Path $stamp) -or (Get-Content -Raw $stamp) -ne $wanted -or -not (Test-Path (Join-Path $ssl 'configdata.pm'))) {
    # A changed configuration starts from the release archive: objects of another configuration must not stay.
    if (Test-Path $ssl) { Remove-Item -Recurse -Force $ssl }
    Remove-Item -Force $stamp -ErrorAction SilentlyContinue
    Write-Host 'extract OpenSSL 3.5.9'
    Invoke-Tool 'tar' @('-xzf', $OpenSslArchive, '-C', $Out)
    Write-Host 'configure OpenSSL for the stage target'
    Push-Location $ssl
    try {
        $old = $env:PATH; $env:PATH = $buildPath
        & $Perl Configure "--config=$(Slash $sslConf)" BC250-UEFI-x86_64 @sslOptions CC=clang.exe AR=llvm-ar.exe RANLIB=llvm-ranlib.exe 'CFLAGS=-O2' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "OpenSSL Configure failed ($LASTEXITCODE)" }
    } finally { $env:PATH = $old; Pop-Location }
    Set-Content -NoNewline -Encoding ascii $stamp $wanted
}
Write-Host 'compile OpenSSL libcrypto'
Push-Location $ssl
try {
    $old = $env:PATH; $env:PATH = $buildPath
    $makeLog = Join-Path $Out 'openssl-make.log'
    # The generated headers first: the libcrypto.a target does not depend on them.
    foreach ($target in @('build_generated', 'libcrypto.a')) {
        & $make -j $Jobs "CFLAGS=$sslCflags" $target *> $makeLog
        if ($LASTEXITCODE -ne 0) {
            Get-Content $makeLog | Where-Object { $_ -match 'error' } | Select-Object -First 20
            throw "OpenSSL make $target failed ($LASTEXITCODE), log $makeLog"
        }
    }
} finally { $env:PATH = $old; Pop-Location }
$libcrypto = Join-Path $ssl 'libcrypto.a'

# ---- libtpms, TPM 2 part
@'
#define WITH_TPM2 1
#define USE_OPENSSL_CRYPTO_LIBRARY 1
'@ | Set-Content -Encoding ascii (Join-Path $Out 'config.h')
$t = Join-Path $Libtpms 'src\tpm2'
# The define set of the Windows core (driver\tpm\core\build.ps1): the upstream build (src\Makefile.am) with
# RADIX_BITS for x64 and the POSIX branches. The same set on both sides keeps the state blobs compatible.
$tpmsDefines = @('-include', 'tpm_library_conf.h', '-DTPM_LIBTPMS_CALLBACKS', '-DTPM_NV_DISK', '-D_POSIX_', '-DTPM_POSIX',
    '-DRADIX_BITS=64', '-D__linux__')
$sslInclude = @("-I$ssl\include")
$tpmsInclude = @("-I$Out", "-I$Libtpms\src", "-I$Libtpms\include", "-I$Libtpms\include\libtpms",
    "-I$t", "-I$t\crypto\openssl", "-I$t\TPMCmd\Platform\include", "-I$t\TPMCmd\Simulator\include",
    "-I$t\TPMCmd\Simulator\include\prototypes", "-I$t\TPMCmd\TpmConfiguration", "-I$t\TPMCmd\tpm\include",
    "-I$t\TPMCmd\tpm\include\private", "-I$t\TPMCmd\tpm\include\private\prototypes",
    "-I$t\TPMCmd\tpm\cryptolibs\common\include", "-I$t\TPMCmd\tpm\cryptolibs\TpmBigNum\include",
    "-I$t\TPMCmd\tpm\cryptolibs\TpmBigNum\include\TpmBigNum", "-I$t\TPMCmd\tpm\cryptolibs",
    "-I$t\TPMCmd\tpm\cryptolibs\Ossl", "-I$t\TPMCmd\tpm\cryptolibs\Ossl\include",
    "-I$t\TPMCmd\tpm\cryptolibs\Ossl\include\Ossl")
# The C runtime first; then the POSIX stand-ins of the Windows core (regex.h, netinet\in.h), which both builds share.
# Some libtpms files use printf functions and get <stdio.h> only through the OpenSSL headers, which leave it out
# with no-stdio: -include stdio.h gives it back.
$tpmsFlags = $stageFlags + $noWindows + $asGcc + @('-w', '-include', $platformHeader, '-include', 'stdio.h', "-I$crt\include", "-I$posix") +
    $tpmsDefines + $sslInclude + $tpmsInclude
$tpmsSources = @(Get-ChildItem -Recurse -File -Filter *.c $t | Where-Object Name -ne 'TPMCmdp.c' | ForEach-Object FullName)
# TPMCmdp.c is the simulator's power and command signals over a TCP socket; posix\signals.c of the Windows core has
# the same bodies without the socket.
$tpmsSources += Join-Path $posix 'signals.c'
$tpmsSources += @('disabled_interface.c', 'tpm_debug.c', 'tpm_library.c', 'tpm_memory.c', 'tpm_nvfile.c',
    'tpm_tpm2_interface.c', 'tpm_tpm2_tis.c') | ForEach-Object { Join-Path $Libtpms "src\$_" }
Remove-Item "$objTpms\*.o" -Force -ErrorAction SilentlyContinue
Write-Host "compile libtpms ($($tpmsSources.Count) files)"
$failed = $tpmsSources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $o = Join-Path $using:objTpms ('tpms_' + [IO.Path]::GetFileNameWithoutExtension($_) + '.o')
    $err = & $using:clang @using:tpmsFlags -c $_ -o $o 2>&1
    if ($LASTEXITCODE -ne 0) { "FAIL $_ :: $(($err | Where-Object { $_ -match 'error' } | Select-Object -First 3) -join ' | ')" }
}
if ($failed) { $failed; throw 'libtpms compile failed' }

# ---- our code: the C runtime and the engine, with the warnings of the stage
$strict = @('-Wall', '-Wextra', '-Werror')
Remove-Item "$objOwn\*.o" -Force -ErrorAction SilentlyContinue
Write-Host 'compile the C runtime and the engine'
foreach ($source in Get-ChildItem "$crt\*.c") {
    Invoke-Tool $clang ($stageFlags + $strict + @("-I$crt\include", "-I$crt", '-c', $source.FullName,
        '-o', (Join-Path $objOwn "crt_$($source.BaseName -replace '^crt_','').o")))
}
# The engine includes the headers of OpenSSL and libtpms: the same macro set as their objects, and -isystem so that
# their own warnings do not count as ours.
$engineFlags = $stageFlags + $noWindows + $strict + @('-include', $platformHeader, "-I$crt\include", "-I$crt", "-I$here",
    '-isystem', "$ssl\include", '-isystem', "$Libtpms\include")
foreach ($source in Get-ChildItem "$here\*.c") {
    Invoke-Tool $clang ($engineFlags + @("-I$repo\driver\tpm", '-c', $source.FullName, '-o', (Join-Path $objOwn "engine_$($source.BaseName).o")))
}
# The AES-256-GCM of the state file: one source with the Windows core (driver\tpm\gcm_openssl.c).
Invoke-Tool $clang ($engineFlags + @("-I$repo\driver\tpm", '-c', (Join-Path $repo 'driver\tpm\gcm_openssl.c'), '-o', (Join-Path $objOwn 'engine_gcm_openssl.o')))

# ---- the archive
$lib = Join-Path $Out 'tpm_engine.lib'
Remove-Item $lib -Force -ErrorAction SilentlyContinue
$members = @(Get-ChildItem "$objOwn\*.o" | Sort-Object Name | ForEach-Object FullName) +
    @(Get-ChildItem "$objTpms\*.o" | Sort-Object Name | ForEach-Object FullName)
Invoke-Tool $ar (@('qc', '--format=coff', $lib) + $members)
# The members of libcrypto.a go in as members (L), not as a nested archive.
Invoke-Tool $ar @('qcL', '--format=coff', $lib, $libcrypto)
Invoke-Tool $ar @('s', $lib)

# ---- the symbol check: every symbol that a member needs is defined in the archive, and no symbol is defined twice
Write-Host 'symbol check'
# Symbol names are case sensitive (d2i_ECPKPARAMETERS and d2i_ECPKParameters are two functions).
$defined = New-Object 'System.Collections.Generic.Dictionary[string,System.Collections.Generic.List[string]]' ([StringComparer]::Ordinal)
$undefined = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
$member = ''
foreach ($line in (& $nm -A -g --no-sort $lib 2>$null)) {
    if ($line -notmatch '^.*\.lib:([^:]+):\s+(?:([0-9a-f]+)\s+)?([A-Za-z])\s+(\S+)$') { continue }
    $member = $matches[1]; $type = $matches[3]; $name = $matches[4]
    if ($type -eq 'U') { [void]$undefined.Add($name) }
    elseif ($type -cmatch '^[TDBR]$') {
        if (-not $defined.ContainsKey($name)) { $defined[$name] = New-Object System.Collections.Generic.List[string] }
        $defined[$name].Add($member)
    }
}
$missing = @($undefined | Where-Object { -not $defined.ContainsKey($_) } | Sort-Object)
# The constants that clang puts in COMDAT sections with "any" selection (string literals ??_C@, vector and floating
# point constants __xmm@ and __real@) are in many members by design; the linker keeps one.
$twice = @($defined.Keys | Where-Object { $defined[$_].Count -gt 1 -and $_ -notmatch '^(\?\?_C@|__xmm@|__ymm@|__real@)' } | Sort-Object)
"members {0}, defined symbols {1}, undefined references {2}" -f $members.Count, $defined.Count, $undefined.Count
if ($missing) { "undefined in the archive: $($missing -join ' ')"; throw 'tpm_engine.lib has undefined symbols' }
if ($twice) { "defined twice: $(($twice | ForEach-Object { "$_ ($($defined[$_] -join ', '))" }) -join '; ')"; throw 'tpm_engine.lib defines symbols twice' }
'every symbol that a member needs is defined in tpm_engine.lib; none is defined twice'
"tpm_engine.lib {0} bytes" -f (Get-Item $lib).Length

# ---- the size in a UEFI image: a probe application that calls each function of tpm_engine.h, linked as the stage is
# linked, with the efl_libc.o of the stage (so the six functions of crt_mem.c come from the stage, as they will).
Write-Host 'link the size probe (UEFI application)'
$efiLibc = Join-Path $objTest 'efl_libc.o'
Invoke-Tool $clang ($stageFlags + $strict + @("-I$efiRoot\lib\include", "-I$efiRoot\lib\include\libc", '-c',
    (Join-Path $efiRoot 'lib\efl_libc.c'), '-o', $efiLibc))
Invoke-Tool $clang ($stageFlags + $strict + @("-I$here", '-c', (Join-Path $here 'test\size_probe.c'), '-o', (Join-Path $objTest 'size_probe.o')))
$linkFlags = @('/nologo', '/nodefaultlib', '/machine:x64', '/opt:ref', '/Brepro', '/debug:none')
$probe = Join-Path $Out 'size_probe.efi'
$empty = Join-Path $Out 'size_probe_empty.efi'
Invoke-Tool $lld ($linkFlags + @('/entry:efi_main', '/subsystem:efi_application', "/out:$probe", (Join-Path $objTest 'size_probe.o'), $efiLibc, $lib))
Invoke-Tool $clang ($stageFlags + $strict + @('-DSIZE_PROBE_EMPTY', "-I$here", '-c', (Join-Path $here 'test\size_probe.c'), '-o', (Join-Path $objTest 'size_probe_empty.o')))
Invoke-Tool $lld ($linkFlags + @('/entry:efi_main', '/subsystem:efi_application', "/out:$empty", (Join-Path $objTest 'size_probe_empty.o'), $efiLibc))
# Every member at once (/wholearchive), without efl_libc.o: lld-link refuses a symbol that two members define and a
# symbol that no member defines. This is the link-level form of the symbol check above.
$whole = Join-Path $Out 'size_probe_whole.efi'
Invoke-Tool $lld ($linkFlags + @('/entry:efi_main', '/subsystem:efi_application', "/out:$whole", (Join-Path $objTest 'size_probe.o'), "/wholearchive:$lib"))
'all members of tpm_engine.lib link together into one UEFI image'
"the engine in a UEFI image: {0} bytes ({1} bytes with the engine, {2} without)" -f `
    ((Get-Item $probe).Length - (Get-Item $empty).Length), (Get-Item $probe).Length, (Get-Item $empty).Length

if ($SkipTest) { exit 0 }

# ---- the host test: the same archive in a Windows console program, with a host binding (test\host_binding.c)
Write-Host 'compile and link the host test'
$hostSources = @('host_binding.c', 'engine_test.c', 'tpm_cmd.c')
foreach ($name in $hostSources) {
    Invoke-Tool $clang ($stageFlags + $strict + @("-I$crt\include", "-I$crt", "-I$here", "-I$here\test", '-c',
        (Join-Path $here "test\$name"), '-o', (Join-Path $objTest ([IO.Path]::ChangeExtension($name, '.o')))))
}
$test = Join-Path $Out 'engine_test.exe'
$importLibs = @((Join-Path $Mingw 'x86_64-w64-mingw32\lib\libkernel32.a'), (Join-Path $Mingw 'x86_64-w64-mingw32\lib\libbcrypt.a'))
Invoke-Tool $lld ($linkFlags + @('/entry:host_entry', '/subsystem:console', "/out:$test") +
    @($hostSources | ForEach-Object { Join-Path $objTest ([IO.Path]::ChangeExtension($_, '.o')) }) + @($lib) + $importLibs)

Write-Host 'run the host test'
& $test
$code = $LASTEXITCODE
Write-Host "engine host test exit code $code"
if ($code -ne 0) { exit $code }

# Each mutation breaks one check on purpose (engine_test.c). A mutation that passes means that its check proves
# nothing, so the build fails.
foreach ($mutation in @('pcr-after-resume', 'volatile-intact', 'gcm-tag')) {
    $lines = & $test "--mutate=$mutation"
    $mutationCode = $LASTEXITCODE
    $lines | Where-Object { $_ -match '^  FAIL |^(PASS|FAIL):' } | ForEach-Object { Write-Host "  $_" }
    if ($mutationCode -eq 0) { throw "the mutation $mutation passed: its check does not detect the fault" }
    Write-Host "mutation $mutation fails as required (exit code $mutationCode)"
}
exit 0
