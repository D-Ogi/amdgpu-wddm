# Builds and runs the M5 part B shim replay test - GFX10 CP/KIQ, SDMA and the interrupt enables -
# and compile-checks the same shim with the WDK kernel flags the miniport uses. Host-side only:
# nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run_gfx.ps1
#   pwsh driver\shim\test\run_gfx.ps1 -Out P:\BC-250\scratch\m5-gfx -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\m5-gfx), never into the repository and
# never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\m5-gfx',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$Cp1Checkpoints,
    [switch]$SkipTrace
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'
$libdrm = Join-Path $repo 'third_party\libdrm'      # the gfx10 dispatch shader the CP stub checks
$evid = Join-Path $repo 'evidence\linux\2026-09-21-E03-init-trace'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
$dumps = Join-Path $Out 'dumps'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern, $dumps | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# No hand-typed register value reaches the build: the two golden tables are cut out of the
# reference-only imports mechanically, and this re-runs both extractions and fails if what is
# checked in has drifted from what the import says.
# The paths are repo-relative on purpose: they are printed into the generated file's banner, so an
# absolute path here would make the check fail on a tree checked out anywhere else.
Write-Host 'verify the generated tables against their imports'
Push-Location $repo
try {
    & python tools\import\extract_table.py --source driver/amdgpu-import/reference/gfx_v10_0.c `
        --array golden_settings_gc_10_0_cyan_skillfish `
        --out driver/shim/generated/gfx10_golden_cyan_skillfish.inc --check
    if ($LASTEXITCODE -ne 0) { throw 'gfx10 golden table differs from the import' }
    & python tools\import\extract_table.py --source driver/amdgpu-import/reference/sdma_v5_0.c `
        --array golden_settings_sdma_cyan_skillfish `
        --out driver/shim/generated/sdma5_golden_cyan_skillfish.inc --check
    if ($LASTEXITCODE -ne 0) { throw 'sdma5 golden table differs from the import' }
} finally {
    Pop-Location
}

# /W4 /WX for our own code. One warning is turned off, and only for the three files that build PM4
# packets:
#   C4245  AMD's PACKET3() in the imported nvd.h yields a signed int with bit 31 set, and every use
#          assigns it to a u32. Casting inside our transcriptions would make them differ from
#          upstream for no behavioural reason.
# Everything else, including the test sources, compiles at a clean /W4 /WX with no exceptions.
$packetWarn = @('/wd4245')

$packetSources = @('bc250_ring.c', 'bc250_gfx.c', 'bc250_sdma.c') | ForEach-Object { Join-Path $shim $_ }
$plainSources = @('shim.c', 'bc250_gmc.c', 'bc250_gart.c', 'bc250_nbio.c',
                  'bc250_irq.c') | ForEach-Object { Join-Path $shim $_ }
$shimSources = $plainSources + $packetSources
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c') | ForEach-Object { Join-Path $imports $_ }
$testSources = @('test\backend_trace.c', 'test\backend_mem.c', 'test\replay_gfx.c') | ForEach-Object { Join-Path $shim $_ }

# The same two the M4 test turns off for the imported hub sources, and for the same reasons; see
# driver\shim\README.md.
$importWarn = @('/wd4244', '/wd4701')

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, host replay test)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $packetWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi') + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\replay_gfx.exe", "/PDB:$Out\replay_gfx.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

# The same sources with the flags of driver\kmd\build.ps1. Compile only.
Write-Host 'compile (kernel flags, same shim sources, no link)'
$incKern = @("/I$shim\include", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_SHIM_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $packetWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $plainSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $importWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $importSources)

# The two reference windows. Reads as well as writes: these start after the GART and the PSP have
# run, so the pre-driver sweep no longer answers every read (see the header of replay_gfx.c).
#   trace-gfx.txt   stages 1 to 9, one hw_init
#   trace-irq.txt   stage 10, a second later: the fence driver and the late init
$traceFile = Join-Path $Out 'trace-gfx.txt'
$traceIrq = Join-Path $Out 'trace-irq.txt'
function Get-Window([string]$path, [string]$since, [string]$until) {
    Write-Host "extract the reference trace $since to $until"
    & python (Join-Path $repo 'tools\trace\extract_phase.py') (Join-Path $evid 'amdgpu-events.txt') `
        --match '^(GC|NBIO)\.' --reads --no-fold --precision 6 --since $since --until $until |
        Set-Content -Encoding ascii $path
    if ($LASTEXITCODE -ne 0) { throw 'extract_phase.py failed' }
}
if (-not $SkipTrace -or -not (Test-Path $traceFile)) { Get-Window $traceFile 0.5496 0.5505 }
if (-not $SkipTrace -or -not (Test-Path $traceIrq)) { Get-Window $traceIrq 1.5601 1.5609 }

Write-Host 'run'
$argv = @((Join-Path $evid 'sweep-before-run1-GC-complete-then-hang.log'),
          (Join-Path $evid 'sweep-before-run2-nonGC.log'),
          $traceFile, $traceIrq, '--dump', $dumps, '--rings', (Join-Path $evid 'rings'),
          '--windows-sweep', (Join-Path $repo 'evidence\windows\2026-09-21-E10-run-001\sweep-GC-loaded-111010.log'))
if ($Verbose250) { $argv += '-v' }
if ($Cp1Checkpoints) { $argv += '--cp1-checkpoints' }
& "$Out\replay_gfx.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "replay_gfx.exe exit code $code"
exit $code
