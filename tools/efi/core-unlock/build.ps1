# Builds core-unlock.efi and the host tests, then runs the host tests.
#
#   pwsh tools\efi\core-unlock\build.ps1
#   pwsh tools\efi\core-unlock\build.ps1 -Out P:\BC-250\scratch\build\efi-core-unlock
#   pwsh tools\efi\core-unlock\build.ps1 -SkipTests
#
# The UEFI image: clang for x86_64-unknown-windows, freestanding, linked by lld-link as an EFI application.
# The host tests: the llvm-mingw clang of the workspace, so the test build needs no Visual Studio and nothing on C:.
# Everything is written under -Out, never into the repository. Nothing here touches a lab machine or a boot entry.

param(
    [string]$Out = 'P:\BC-250\scratch\build\efi-core-unlock',
    [string]$Llvm = 'P:\BC-250\toolchain\llvm-23.1.4-win64\bin',
    [string]$Mingw = 'P:\BC-250\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$shim = Join-Path $repo 'driver\shim'

$objEfi = Join-Path $Out 'obj-efi'
$objHost = Join-Path $Out 'obj-host'
New-Item -ItemType Directory -Force $Out, $objEfi, $objHost | Out-Null
Remove-Item "$objEfi\*.o", "$objHost\*.o" -Force -ErrorAction SilentlyContinue
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

# Our own sources, and the repository sources this component shares with the driver. The shared ones keep their
# own warning level: they are the driver's code, built the same way here and there.
$own = @('cu_decide.c', 'cu_run.c', 'cu_smu.c', 'cu_acpi.c', 'cu_smbios.c', 'cu_text.c',
    'cu_aml.c', 'cu_msr.c', 'cu_report.c') |
    ForEach-Object { Join-Path $here "src\$_" }
$shared = @((Join-Path $shim 'bc250_smu.c'), (Join-Path $shim 'bc250_cpu.c'), (Join-Path $shim 'bc250_oc.c'),
    (Join-Path $repo 'driver\kmd\board_identity.c'))
$includes = @("-I$here\src", "-I$shim\include", "-I$shim", "-I$repo\driver\amdgpu-import",
    "-I$repo\third_party\linux-amdgpu", "-I$repo\driver\kmd")
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
    "-I$here\include", "-I$here\include\libc") + $includes
Build-Objects $clang $efiFlags $objEfi ($own + @((Join-Path $here 'src\main.c'), (Join-Path $here 'src\report_uefi.c'),
    (Join-Path $here 'src\cu_libc.c'))) $strict
Build-Objects $clang $efiFlags $objEfi $shared $relaxed

Write-Host 'link (UEFI image)'
$efi = Join-Path $Out 'core-unlock.efi'
Invoke-Tool (Join-Path $Llvm 'lld-link.exe') (@('/nologo', '/subsystem:efi_application', '/entry:efi_main',
    '/nodefaultlib', '/machine:x64', '/opt:ref', '/Brepro', '/debug:none', "/out:$efi") +
    (Get-ChildItem "$objEfi\*.o").FullName)
$hash = (Get-FileHash -Algorithm SHA256 $efi).Hash
"$hash  core-unlock.efi" | Set-Content -Encoding ascii (Join-Path $Out 'SHA256SUMS')
Write-Host "core-unlock.efi $((Get-Item $efi).Length) bytes, SHA256 $hash"

if ($SkipTests) { exit 0 }

Write-Host 'compile (host tests)'
$hostCc = Join-Path $Mingw 'x86_64-w64-mingw32-clang.exe'
$hostFlags = @('-std=c11', '-O1', '-g') + $includes
Build-Objects $hostCc $hostFlags $objHost ($own + @((Join-Path $here 'test\host_test.c'), (Join-Path $here 'test\report_test.c'))) $strict
Build-Objects $hostCc $hostFlags $objHost $shared $relaxed
$test = Join-Path $Out 'host_test.exe'
Invoke-Tool $hostCc (@('-o', $test) + (Get-ChildItem "$objHost\*.o").FullName)

Write-Host 'run (host tests)'
# The report tests read the stock ACPI tables of unit A that the repository stores as evidence.
& $test (Join-Path $repo 'evidence\linux\2026-09-21-E01-recon-binaries\acpi')
$code = $LASTEXITCODE
Write-Host "core-unlock host tests exit code $code"
exit $code
