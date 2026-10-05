# Builds and test-signs bc250kmd (the M3 display-only miniport) without Visual Studio project files.
#
#   pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd
#
# Output: <Out>\package\{bc250kmd.sys, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}
#
# With -UmdStub <directory holding bc250umd.dll> a SECOND package is written next to the first, for the second
# run of M7 stage A: the same signed .sys, the stub DLL, and an INF generated from the repository's with the
# UserModeDriverName block live. Build the DLL first:
#
#   pwsh driver\umd-stub\build.ps1 -Kits ... -Out P:\BC-250\scratch\build\bc250umd
#   pwsh driver\kmd\build.ps1     -Kits ... -Out P:\BC-250\scratch\build\bc250kmd `
#                                 -UmdStub P:\BC-250\scratch\build\bc250umd
#
# Output: <Out>\package-umd\{bc250kmd.sys, bc250umd.dll, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}
#
# The two packages must carry the same driver, or the second run measures two changes instead of one, so the
# .sys is copied from the first package after it is signed and both SHA256 hashes are printed at the end.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$UmdStub = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$CertSubject = 'CN=BC-250 lab test signing'
)

$ErrorActionPreference = 'Stop'
$here = 'P:\bc-250\bc250-win\driver\kmd'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$pkg = Join-Path $Out 'package'
$obj = Join-Path $Out 'obj'
if (Test-Path $obj) { Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $pkg, $obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

# The second run's INF, generated from the repository's so that the two cannot drift. The repository INF is never
# modified. Two markers, both documented in bc250kmd.inf itself:
#
#   ;@UMD <line>            enabled here (the prefix is removed)
#   <line> ;@PLAIN-ONLY     disabled here (a ';' is put in front)
#
# It throws rather than write a package quietly missing the UserModeDriverName block, because such a package is
# byte for byte the first run's experiment while claiming to be the second one - the one failure mode of this
# whole switch that would not announce itself on the lab machine. Hence the checks against the RESULT below, not
# only against the markers: what matters is what came out, not that something was substituted.
function Write-UmdInf([string]$Source, [string]$Target) {
    $enabled = 0
    $disabled = 0
    $bumped = ''
    $lines = Get-Content -LiteralPath $Source | ForEach-Object {
        if ($_ -match '^;@UMD ?(.*)$') { $enabled++; $Matches[1] }
        # Same hardware id, same date, same version: on such a tie Windows keeps the driver it already has, and
        # run 2 would quietly be run 1 with a second package in the store. The last field goes up by one here, so
        # that the run 2 package outranks the plain one and the device's driver version says which run it is.
        elseif ($_ -match '^(DriverVer\s*=\s*[\d/]+,\d+\.\d+\.\d+\.)(\d+)\s*$') { $bumped = $Matches[1] + ([int]$Matches[2] + 1); $bumped }
        elseif ($_ -match ';@PLAIN-ONLY\s*$') { $disabled++; ";$_" }
        else { $_ }
    }
    if ($enabled -eq 0) { throw "$Source has no ;@UMD lines: the stage A run 2 block is gone or was renamed" }
    if ($disabled -eq 0) { throw "$Source has no ;@PLAIN-ONLY lines: the plain CopyFiles line is gone or was renamed" }
    if ($bumped -eq '') { throw "$Source has no DriverVer line of the form date,a.b.c.d: the run 2 package would tie with the plain one" }
    $text = $lines -join "`r`n"
    foreach ($want in '(?m)^\s*HKR,,\s*UserModeDriverName\b', '(?m)^\s*CopyFiles\s*=\s*Bc250_Files,\s*Bc250_UmdFiles\b',
                      '(?m)^\s*\[Bc250_UmdFiles\]', '(?m)^\s*bc250umd\.dll\s*=\s*1\b', '(?m)^\s*Bc250_UmdFiles\s*=\s*11\b') {
        if ($text -notmatch $want) { throw "the generated INF does not match $want : the markers in $Source moved" }
    }
    if ($text -match '(?m)^\s*CopyFiles\s*=\s*Bc250_Files\s*(;.*)?$') { throw 'the generated INF still copies only Bc250_Files' }
    # utf8NoBOM, which is how it was read: ascii would turn a proverb in a comment into question marks.
    Set-Content -LiteralPath $Target -Value ($text + "`r`n") -Encoding utf8NoBOM -NoNewline
    Write-Host "  bc250kmd.inf generated: $enabled lines enabled, $disabled commented out, $($bumped -replace '\s+', ' ')"
}

$env:INCLUDE = ''; $env:LIB = ''
$sources = (Get-ChildItem (Join-Path $here '*.c')).FullName
# M4, M5: AMD's imported code and the shim it compiles against (ADR 0002). Same flags; the imports get the warning
# disables documented in driver\amdgpu-import\PROVENANCE.md, from the build line, never by editing them.
$repo = Split-Path (Split-Path $here)
$shimInc = @("/I$repo\driver\shim\include", "/I$repo\driver\amdgpu-import", "/I$repo\third_party\linux-amdgpu", "/I$repo\third_party\libdrm", '/DBC250_SHIM_KERNEL')
$shimSources = @("$repo\driver\shim\shim.c", "$repo\driver\shim\bc250_gmc.c", "$repo\driver\shim\bc250_gart.c", "$repo\driver\shim\bc250_pte.c", "$repo\driver\shim\bc250_psp.c")
# M5 second part: amdgpu's gfx/SDMA bring-up transcribed against AMD's imported tables. C4245: AMD's PACKET3() in the
# imported nvd.h is a signed int with bit 31 set (driver\shim\README.md).
# M6: bc250_ih.c, the interrupt ring (navi10_ih.c), is in this group for its include path.
$shimGfxSources = @('bc250_ring.c', 'bc250_gfx.c', 'bc250_sdma.c', 'bc250_sdma_copy.c', 'bc250_sdma_paging.c', 'bc250_nbio.c', 'bc250_irq.c', 'bc250_ih.c', 'bc250_dispatch.c', 'bc250_clock.c', 'bc250_smu.c') | ForEach-Object { "$repo\driver\shim\$_" }
# Named, not globbed: only what this driver runs is compiled into it.
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c', 'psp_v11_0_8.c') | ForEach-Object { "$repo\driver\amdgpu-import\$_" }
Write-Host 'compile'
$clFlags = @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214',           # nameless unions and bit fields in the WDK's own headers
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um",
    "/Fo$obj\", "/Fd$obj\cl.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + @((Join-Path $here 'vidmm.c')))
Write-Host "vidmm.c kernel compile PASS"
