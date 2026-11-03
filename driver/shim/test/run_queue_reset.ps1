# Builds and runs the host check of the per-queue resets (bc250_gfx_reset_kcq, bc250_gfx_reset_kgq,
# docs/design/queue-reset.md section 8). Host-side only: nothing here touches the lab.
#
#   pwsh driver\shim\test\run_queue_reset.ps1
#   pwsh driver\shim\test\run_queue_reset.ps1 -NoRptrZero   # the negative control: must fail
#
# -NoRptrZero compiles a copy of bc250_gfx.c in which the compute reset keeps the sampled read
# pointer in the MQD. The check must catch it. Everything is written under -Out.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\build\queue-reset",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$NoRptrZero
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'
$libdrm = Join-Path $repo 'third_party\libdrm'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-user'
New-Item -ItemType Directory -Force $Out, $objUser | Out-Null
Remove-Item "$objUser\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# The file under test is compiled from a copy next to the build, so that the negative control
# changes the copy and never the tree.
$underTest = Join-Path $Out 'bc250_gfx.c'
$source = Get-Content (Join-Path $shim 'bc250_gfx.c') -Raw
if ($NoRptrZero) {
    $live = '((struct v10_compute_mqd *)ring->mqd_ptr)->cp_hqd_pq_rptr = 0;'
    if (-not $source.Contains($live)) { throw 'negative control: the read pointer line moved' }
    $source = $source.Replace($live, '')
}
[IO.File]::WriteAllText($underTest, $source)

#   C4245  as in run_gfx.ps1: AMD's PACKET3() in the imported nvd.h yields a signed int with bit 31
#          set, and every use assigns it to a u32.
$packetWarn = @('/wd4245')
$packetSources = @($underTest, (Join-Path $shim 'bc250_ring.c'), (Join-Path $shim 'bc250_sdma.c'))
$plainSources = @('shim.c', 'bc250_gmc.c', 'bc250_gart.c', 'bc250_nbio.c', 'bc250_irq.c') |
    ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\queue_reset.c'))
# The same two warnings run_gfx.ps1 turns off for the imported hub sources, for the same reasons.
$importWarn = @('/wd4244', '/wd4701')
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c') |
    ForEach-Object { Join-Path $imports $_ }

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, queue reset model)'
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
        "/OUT:$Out\queue_reset.exe", "/PDB:$Out\queue_reset.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
. "$here\..\..\kmd\test\host_native.ps1"
Invoke-NativeStep "$Out\queue_reset.exe" $argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "queue_reset.exe exit code $code"
exit $code
