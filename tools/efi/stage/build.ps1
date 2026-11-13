# Builds the BC-250 stage (bc250-stage.efi) and the host tests, then runs the host tests.
#
#   pwsh tools\efi\stage\build.ps1
#   pwsh tools\efi\stage\build.ps1 -Out P:\BC-250\scratch\build\efi-stage
#   pwsh tools\efi\stage\build.ps1 -SkipTests
#   pwsh tools\efi\stage\build.ps1 -SignedImage <a signed UEFI image, for example a Windows Boot Manager>
#
# The UEFI image: clang for x86_64-unknown-windows, freestanding, linked by lld-link. The stage is an EFI
# application.
# The host tests: the llvm-mingw clang of the workspace, so the test build needs no Visual Studio and nothing on C:.
# Everything is written under -Out, never into the repository. Nothing here touches a lab machine or a boot entry.

param(
    [string]$Out = 'P:\BC-250\scratch\build\efi-stage',
    [string]$Llvm = 'P:\BC-250\toolchain\llvm-23.1.4-win64\bin',
    [string]$Mingw = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin',
    # The ACPI dump of unit A, outside the repository. The tests that read it say SKIP when it is not there.
    [string]$AcpiDump = 'P:\BC-250\firmware\unit-A-2026-09-21\acpi',
    # The ACPICA compiler, as a second parser of the built tables. Not there: that check says SKIP.
    [string]$Iasl = 'P:\BC-250\toolchain\acpica-iasl-20260930\iasl.exe',
    [string]$SignedImage = '',
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
$unlockPure = Sources 'core-unlock\src' @('cu_decide.c', 'cu_run.c', 'cu_smu.c', 'cu_msr.c', 'cu_report.c', 'cu_options.c')
$repairPure = Sources 'acpi-repair\src' @('ar_plan.c')
$securePure = Sources 'secure-boot\src' @('sb_der.c', 'sb_rsa.c', 'sb_x509.c', 'sb_pe.c', 'sb_pkcs7.c', 'sb_siglist.c',
    'sb_verify.c')
$pure = @($libPure) + @($corePure) + @($unlockPure) + @($repairPure) + @($securePure)
# The UEFI bindings: the core and one file for each module.
$bindings = @(Sources 'stage\src' @('main.c')) + @(Sources 'core-unlock\src' @('cu_module.c', 'report_uefi.c')) +
    @(Sources 'acpi-repair\src' @('ar_module.c')) + @(Sources 'secure-boot\src' @('sb_module.c')) +
    @(Sources 'lib' @('efl_libc.c'))
# The repository sources that the stage shares with the driver. They keep their own warning level: they are the
# driver's code, built the same way here and there.
$shared = @((Join-Path $shim 'bc250_smu.c'), (Join-Path $shim 'bc250_cpu.c'), (Join-Path $shim 'bc250_oc.c'),
    (Join-Path $repo 'driver\kmd\board_identity.c'))
$tests = @((Join-Path $efiRoot 'stage\test\host_main.c'), (Join-Path $efiRoot 'stage\test\stage_test.c'),
    (Join-Path $efiRoot 'lib\test\lib_test.c'), (Join-Path $efiRoot 'core-unlock\test\host_test.c'),
    (Join-Path $efiRoot 'core-unlock\test\report_test.c'), (Join-Path $efiRoot 'acpi-repair\test\ar_test.c'),
    (Join-Path $efiRoot 'secure-boot\test\sb_test.c'))

$includes = @("-I$efiRoot\lib", "-I$efiRoot\lib\acpi", "-I$efiRoot\stage\src", "-I$efiRoot\core-unlock\src",
    "-I$efiRoot\acpi-repair\src", "-I$efiRoot\secure-boot\src",
    "-I$shim\include", "-I$shim", "-I$repo\driver\amdgpu-import", "-I$repo\third_party\linux-amdgpu", "-I$repo\driver\kmd")
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
Build-Objects $clang $efiFlags $objStage ($pure + $bindings) $strict
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
$hostFlags = @('-std=c11', '-O1', '-g', "-I$efiRoot\lib\include", "-I$efiRoot\lib\test") + $includes
Build-Objects $hostCc $hostFlags $objHost ($pure + $tests) $strict
Build-Objects $hostCc $hostFlags $objHost $shared $relaxed
$test = Join-Path $Out 'host_test.exe'
Invoke-Tool $hostCc (@('-o', $test) + (Get-ChildItem "$objHost\*.o").FullName)

Write-Host 'fixtures (Secure Boot tests)'
# New throwaway keys at each run. Only public data is written, under -Out.
Remove-Item $fixtures -Recurse -Force -ErrorAction SilentlyContinue
& python (Join-Path $efiRoot 'secure-boot\test\make_fixtures.py') $fixtures
if ($LASTEXITCODE -eq 3) { Write-Host 'SKIP the Secure Boot fixtures: the Python package cryptography is not installed' }
elseif ($LASTEXITCODE -ne 0) { throw "make_fixtures.py failed ($LASTEXITCODE)" }

Write-Host 'run (host tests)'
# The first directory is the stock ACPI tables that the repository stores as evidence.
& $test (Join-Path $repo 'evidence\linux\2026-09-21-E01-recon-binaries\acpi') $AcpiDump $fixtures $tables $SignedImage
$code = $LASTEXITCODE
Write-Host "bc250-stage host tests exit code $code"

# A second parser for the tables that the ACPI repair built: the ACPICA disassembler must read each one whole.
$built = @(Get-ChildItem "$tables\*.aml" -ErrorAction SilentlyContinue)
if (-not (Test-Path $Iasl)) { Write-Host "SKIP the ACPICA check of the built tables: no $Iasl" }
elseif ($built.Count -eq 0) { Write-Host 'SKIP the ACPICA check of the built tables: the tests wrote no table' }
else {
    foreach ($table in $built) {
        $log = & $Iasl -d $table.FullName 2>&1 | Out-String
        $dsl = [IO.Path]::ChangeExtension($table.FullName, '.dsl')
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dsl) -or $log -match 'Error|could not') {
            Write-Host "FAIL the ACPICA disassembler refused $($table.Name):`n$log"
            $code = 1
        } else { Write-Host "ACPICA read $($table.Name): $((Get-Content $dsl | Measure-Object -Line).Lines) lines of ASL" }
    }
}
exit $code
