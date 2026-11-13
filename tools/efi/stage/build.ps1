# Builds the BC-250 stage (bc250-stage.efi) and the host tests, then runs the host tests.
#
#   pwsh tools\efi\stage\build.ps1
#   pwsh tools\efi\stage\build.ps1 -Out P:\BC-250\scratch\build\efi-stage
#   pwsh tools\efi\stage\build.ps1 -SkipTests
#   pwsh tools\efi\stage\build.ps1 -SignedImage <a signed UEFI image> -SecondSignedImage <another one>
#
# The UEFI image: clang for x86_64-unknown-windows, freestanding, linked by lld-link. The stage is an EFI
# application.
# The host tests: the llvm-mingw clang of the workspace with its address sanitizer, so the test build needs no
# Visual Studio. The Secure Boot tests read two signed images of the Windows installation of this PC, as controls
# that this project did not make. They are read only. A control that is not there fails the build.
# Everything is written under -Out, never into the repository. Nothing here touches a lab machine or a boot entry.

param(
    [string]$Out = 'P:\BC-250\scratch\build\efi-stage',
    [string]$Llvm = 'P:\BC-250\toolchain\llvm-23.1.4-win64\bin',
    [string]$Mingw = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin',
    # The ACPI dump of unit A, outside the repository. The tests that read it say SKIP when it is not there.
    [string]$AcpiDump = 'P:\BC-250\firmware\unit-A-2026-09-21\acpi',
    # The ACPICA compiler, as a second parser of the built tables. Not there: that check says SKIP.
    [string]$Iasl = 'P:\BC-250\toolchain\acpica-iasl-20260930\iasl.exe',
    # Two real signed images, the positive controls of the Secure Boot tests. They are necessary.
    [string]$SignedImage = 'C:\Windows\Boot\EFI\bootmgfw.efi',
    [string]$SecondSignedImage = 'C:\Windows\System32\winload.efi',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$efiRoot = (Resolve-Path (Join-Path $here '..')).Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$shim = Join-Path $repo 'driver\shim'

$objStage = Join-Path $Out 'obj-stage'
$objHost = Join-Path $Out 'obj-host'
$fixtures = Join-Path $Out 'fixtures'
$tables = Join-Path $Out 'tables'
New-Item -ItemType Directory -Force $Out, $objStage, $objHost, $tables | Out-Null
Remove-Item "$objStage\*.o", "$objHost\*.o", "$tables\*" -Force -ErrorAction SilentlyContinue
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

function Sources([string]$directory, [string[]]$names) { $names | ForEach-Object { Join-Path $efiRoot "$directory\$_" } }

# The pure code of each part: it builds for the firmware and for the host tests.
$libPure = Sources 'lib' @('efl_sha256.c', 'efl_smbios.c', 'efl_text.c', 'acpi\efl_acpi.c', 'acpi\efl_aml.c', 'acpi\efl_amlw.c')
$corePure = Sources 'stage\src' @('stage_options.c')
$unlockPure = Sources 'core-unlock\src' @('cu_bar.c', 'cu_decide.c', 'cu_run.c', 'cu_smu.c', 'cu_msr.c', 'cu_report.c', 'cu_options.c')
$repairPure = Sources 'acpi-repair\src' @('ar_plan.c')
$securePure = Sources 'secure-boot\src' @('sb_der.c', 'sb_rsa.c', 'sb_x509.c', 'sb_pe.c', 'sb_pkcs7.c', 'sb_siglist.c',
    'sb_verify.c', 'sb_policy.c', 'sb_crc32.c')
$pure = @($libPure) + @($corePure) + @($unlockPure) + @($repairPure) + @($securePure)
# The UEFI bindings: the core and one file for each module.
$bindings = @(Sources 'stage\src' @('main.c')) + @(Sources 'core-unlock\src' @('cu_module.c', 'report_uefi.c')) +
    @(Sources 'acpi-repair\src' @('ar_module.c')) + @(Sources 'secure-boot\src' @('sb_module.c', 'sb_windows.c'))
# The small C library of the UEFI image. The host tests use the C library of the host.
$libc = @(Sources 'lib' @('efl_libc.c'))
# The repository sources that the stage shares with the driver. They keep their own warning level: they are the
# driver's code, built the same way here and there.
$shared = @((Join-Path $shim 'bc250_smu.c'), (Join-Path $shim 'bc250_cpu.c'), (Join-Path $shim 'bc250_oc.c'),
    (Join-Path $repo 'driver\common\board\board_identity.c'))
$tests = @((Join-Path $efiRoot 'stage\test\host_main.c'), (Join-Path $efiRoot 'stage\test\stage_test.c'),
    (Join-Path $efiRoot 'lib\test\lib_test.c'), (Join-Path $efiRoot 'core-unlock\test\host_test.c'),
    (Join-Path $efiRoot 'core-unlock\test\report_test.c'), (Join-Path $efiRoot 'acpi-repair\test\ar_test.c'),
    (Join-Path $efiRoot 'secure-boot\test\sb_test.c'), (Join-Path $efiRoot 'stage\test\firmware_test.c'))

$includes = @("-I$efiRoot\lib", "-I$efiRoot\lib\acpi", "-I$efiRoot\stage\src", "-I$efiRoot\core-unlock\src",
    "-I$efiRoot\acpi-repair\src", "-I$efiRoot\secure-boot\src",
    "-I$shim\include", "-I$shim", "-I$repo\driver\amdgpu-import", "-I$repo\third_party\linux-amdgpu", "-I$repo\driver\kmd", "-I$repo\driver\common\board")
$strict = @('-Wall', '-Wextra', '-Werror')
$relaxed = @('-Wall', '-Wno-unknown-pragmas', '-Wno-unused-parameter')

function Build-Objects([string]$cc, [string[]]$flags, [string]$objDir, [string[]]$sources, [string[]]$warnings) {
    foreach ($source in $sources) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        Invoke-Tool $cc ($flags + $warnings + @('-c', $source, '-o', (Join-Path $objDir "$name.o")))
    }
}

Write-Host 'compile (UEFI image)'
$clang = Join-Path $Llvm 'clang.exe'
$efiFlags = @('--target=x86_64-unknown-windows', '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlibinc',
    '-fno-stack-protector', '-mno-stack-arg-probe', '-mno-red-zone', '-fno-ident',
    "-I$efiRoot\lib\include", "-I$efiRoot\lib\include\libc") + $includes
Build-Objects $clang $efiFlags $objStage ($pure + $bindings + $libc) $strict
Build-Objects $clang $efiFlags $objStage $shared $relaxed

Write-Host 'link (UEFI image)'
$link = Join-Path $Llvm 'lld-link.exe'
$linkFlags = @('/nologo', '/entry:efi_main', '/nodefaultlib', '/machine:x64', '/opt:ref', '/Brepro', '/debug:none')
$stage = Join-Path $Out 'bc250-stage.efi'
Invoke-Tool $link ($linkFlags + @('/subsystem:efi_application', "/out:$stage") + (Get-ChildItem "$objStage\*.o").FullName)
$sums = foreach ($image in @($stage)) {
    $hash = (Get-FileHash -Algorithm SHA256 $image).Hash
    Write-Host "$(Split-Path -Leaf $image) $((Get-Item $image).Length) bytes, SHA256 $hash"
    "$hash  $(Split-Path -Leaf $image)"
}
$sums | Set-Content -Encoding ascii (Join-Path $Out 'SHA256SUMS')

if ($SkipTests) { exit 0 }

Write-Host 'compile (host tests)'
$hostCc = Join-Path $Mingw 'x86_64-w64-mingw32-clang.exe'
foreach ($control in @($SignedImage, $SecondSignedImage)) {
    if (-not $control -or -not (Test-Path -LiteralPath $control -PathType Leaf)) {
        throw "the signed image '$control' is not there: the Secure Boot tests have no control (-SignedImage, -SecondSignedImage)"
    }
}
# The address sanitizer stops the test at a read or a write outside a buffer. Its run-time library is a DLL beside
# the compiler.
$env:PATH = "$Mingw;$env:PATH"
$hostFlags = @('-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-DSB_RT_IN_PLACE', "-I$efiRoot\lib\include", "-I$efiRoot\lib\test") + $includes
# The bindings are in the host build too: firmware_test.c runs them against a model of the firmware.
Build-Objects $hostCc $hostFlags $objHost ($pure + $bindings + $tests) $strict
Build-Objects $hostCc $hostFlags $objHost $shared $relaxed
$test = Join-Path $Out 'host_test.exe'
Invoke-Tool $hostCc (@('-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-o', $test) + (Get-ChildItem "$objHost\*.o").FullName)

Write-Host 'fixtures (Secure Boot tests)'
# New throwaway keys at each run. Only public data is written, under -Out.
Remove-Item $fixtures -Recurse -Force -ErrorAction SilentlyContinue
& python (Join-Path $efiRoot 'secure-boot\test\make_fixtures.py') $fixtures
if ($LASTEXITCODE -ne 0) { throw "make_fixtures.py failed ($LASTEXITCODE): the Python package cryptography is necessary" }

Write-Host 'control (the fixtures under the signature check of Windows)'
# The fixtures come from this project's own encoder. Windows reads them with code that this project did not write.
# The test root is not a trusted root of this PC, so the status of a sound signature is UnknownError or NotTrusted
# with the signer certificate read. A changed image must be HashMismatch.
$sound = Get-AuthenticodeSignature -LiteralPath (Join-Path $fixtures 'signed.efi')
$changed = Get-AuthenticodeSignature -LiteralPath (Join-Path $fixtures 'signed_tampered.efi')
Write-Host "signed.efi: $($sound.Status), signer $($sound.SignerCertificate.Subject)"
Write-Host "signed_tampered.efi: $($changed.Status)"
if ($null -eq $sound.SignerCertificate -or $sound.SignerCertificate.Subject -notmatch 'BC-250 test signer' -or
    $sound.Status -in @('HashMismatch', 'NotSigned', 'Incompatible', 'NotSupportedFileFormat')) {
    throw 'Windows does not read the signature of the fixture signed.efi'
}
if ($changed.Status -ne 'HashMismatch') { throw 'Windows does not refuse the changed fixture signed_tampered.efi' }

Write-Host 'run (host tests)'
# The first directory is the stock ACPI tables that the repository stores as evidence.
& $test (Join-Path $repo 'evidence\linux\2026-09-21-E01-recon-binaries\acpi') $AcpiDump $fixtures $tables $SignedImage $SecondSignedImage
$code = $LASTEXITCODE
Write-Host "bc250-stage host tests exit code $code"

# A second parser for the tables that the ACPI repair built: ACPICA loads each one into a namespace together with
# the stock DSDT and the other tables, as an operating system does. The stock CPU table is not in that set,
# because the repair removes it. The load fails on a name that is defined twice. A scope whose target is not in
# the set shows as an External of a guessed type, and a target that is there shows as ProcessorObj.
$stock = Join-Path $repo 'evidence\linux\2026-09-21-E01-recon-binaries\acpi'
$built = @(Get-ChildItem "$tables\*.aml" -ErrorAction SilentlyContinue)
if (-not (Test-Path $Iasl)) { Write-Host "SKIP the ACPICA check of the built tables: no $Iasl" }
elseif ($built.Count -eq 0) { Write-Host 'SKIP the ACPICA check of the built tables: the tests wrote no table' }
else {
    $space = Join-Path $Out 'namespace'
    Remove-Item $space -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $space | Out-Null
    Copy-Item (Join-Path $stock 'DSDT.aml'), (Join-Path $stock 'SSDT1.aml'), (Join-Path $stock 'SSDT2.aml') $space
    Copy-Item $built.FullName $space
    Copy-Item (Join-Path $tables 'ar-idle.aml') (Join-Path $space 'copy-of-idle.aml')

    # Loads one table with the others as its namespace. Returns the exit code, the log and the External lines.
    function Read-InNamespace([string]$table, [string[]]$others) {
        $argv = @(); foreach ($other in $others) { $argv += '-e'; $argv += $other }
        Push-Location $space
        try {
            Remove-Item ([IO.Path]::ChangeExtension($table, '.dsl')) -ErrorAction SilentlyContinue
            $log = & $Iasl @argv -d $table 2>&1 | Out-String
            $exit = $LASTEXITCODE
            $dsl = [IO.Path]::ChangeExtension($table, '.dsl')
            $lines = if (Test-Path $dsl) { @(Get-Content $dsl) } else { @() }
        } finally { Pop-Location }
        [pscustomobject]@{ Exit = $exit; Log = $log; Lines = $lines; Externals = @($lines | Where-Object { $_ -match '^\s*External \(' }) }
    }
    function Test-Loaded($result) { $result.Exit -eq 0 -and $result.Lines.Count -gt 0 -and $result.Log -notmatch 'AE_|Could not' }

    # The controls first: the method must see the two defects that it is here to find.
    $blind = $false
    $control = Read-InNamespace 'SSDT1.aml' @('DSDT.aml', 'SSDT2.aml')
    if (@($control.Externals | Where-Object { $_ -match 'C000' -and $_ -notmatch 'ProcessorObj' }).Count -eq 0) {
        Write-Host 'FAIL the ACPICA control: the stock CPU table does not show its missing target'; $code = 1; $blind = $true
    }
    $control = Read-InNamespace 'ar-idle.aml' @('DSDT.aml', 'SSDT2.aml', 'copy-of-idle.aml')
    if ((Test-Loaded $control) -or $control.Log -notmatch 'AE_ALREADY_EXISTS') {
        Write-Host 'FAIL the ACPICA control: a name that is defined twice does not stop the load'; $code = 1; $blind = $true
    }
    if (-not $blind) { Write-Host 'ACPICA controls: the missing target of the stock CPU table and a double definition are both found' }

    foreach ($idle in @($built | Where-Object { $_.Name -like 'ar-idle*' })) {
        $result = Read-InNamespace $idle.Name @('DSDT.aml', 'SSDT2.aml', 'ar-stubs.aml')
        $guessed = @($result.Externals | Where-Object { $_ -notmatch 'ProcessorObj' })
        if (-not (Test-Loaded $result) -or $result.Externals.Count -eq 0 -or $guessed.Count -ne 0) {
            Write-Host "FAIL ACPICA did not load $($idle.Name) into the stock namespace:`n$($result.Log)`n$($guessed -join "`n")"
            $code = 1
        } else {
            Write-Host "ACPICA loaded $($idle.Name): $($result.Externals.Count) scopes, each on a Processor object of the DSDT, $($result.Lines.Count) lines of ASL"
        }
    }
    $result = Read-InNamespace 'ar-stubs.aml' @('DSDT.aml', 'SSDT2.aml', 'ar-idle.aml')
    if (-not (Test-Loaded $result)) { Write-Host "FAIL ACPICA did not load ar-stubs.aml into the stock namespace:`n$($result.Log)"; $code = 1 }
    else { Write-Host "ACPICA loaded ar-stubs.aml: $($result.Lines.Count) lines of ASL" }
    # The stub table with the TPM device of trial s8 loads in place of ar-stubs.aml, and ACPICA reads the _DSM of
    # the device as a serialized method with four arguments that compares Arg0 with the PPI UUID.
    if (Test-Path (Join-Path $tables 'ar-stubs-tpm.aml')) {
        $result = Read-InNamespace 'ar-stubs-tpm.aml' @('DSDT.aml', 'SSDT2.aml', 'ar-idle.aml')
        $text = $result.Lines -join "`n"
        if (-not (Test-Loaded $result) -or $text -notmatch 'Method \(_DSM, 4, Serialized\)' -or
            $text -notmatch '(?i)3dddfaa6-361b-4eb4-a424-8d10089d1653') {
            Write-Host "FAIL ACPICA did not read the TPM _DSM of ar-stubs-tpm.aml:`n$($result.Log)"; $code = 1
        } else { Write-Host "ACPICA loaded ar-stubs-tpm.aml: Method (_DSM, 4, Serialized) with the PPI UUID, $($result.Lines.Count) lines of ASL" }
    } else { Write-Host 'FAIL the tests wrote no ar-stubs-tpm.aml'; $code = 1 }
    # With the stubs in the namespace, the DSDT has no method without a definition.
    $result = Read-InNamespace 'DSDT.aml' @('SSDT2.aml', 'ar-idle.aml', 'ar-stubs.aml')
    $unknown = @($result.Lines | Where-Object { $_ -match 'Unknown method' })
    if ($result.Exit -ne 0 -or $result.Lines.Count -eq 0 -or $unknown.Count -ne 0) {
        Write-Host "FAIL the DSDT still has methods without a definition:`n$($unknown -join "`n")"; $code = 1
    } else { Write-Host 'ACPICA read the DSDT with the built tables: no method without a definition' }
}
exit $code
