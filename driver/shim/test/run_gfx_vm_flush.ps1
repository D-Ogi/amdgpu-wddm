# Builds and runs the gfx job-frame and ring VM flush packet check (option (b) of
# docs/design/gfx-submit-root-serialization.md): is the frame of 0.7.216.24 unchanged with
# EnableRingVmFlush off, and are the 29 dwords in front of it with the gate on what
# gmc_v10_0_emit_flush_gpu_tlb() and gfx_v10_0_ring_emit_vm_flush() emit? Host-side only: nothing
# here touches the lab.
#
#   pwsh driver\shim\test\run_gfx_vm_flush.ps1
#   pwsh driver\shim\test\run_gfx_vm_flush.ps1 -WrongAckMask      # the negative control: must fail
#
# -WrongAckMask is that control. It compiles a copy of driver\shim\bc250_gfx.c whose acknowledge
# wait masks every bit instead of the VMID's own, which is the mistake the design note names as a CP
# that waits forever, and the check must catch it. The copy is built under -Out; the tree is never
# changed.
#
# Everything is written under -Out (default <BC250_ROOT>\scratch\build\gfx-vm-flush), never into the
# repository and never onto drive C:. BC250_ROOT is the workspace root: the environment variable,
# else the parent directory of this repository.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\build\gfx-vm-flush",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$WrongAckMask
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
if ($WrongAckMask) {
    $live = 'bc250_gfx_vm_reg_wait(ring, (u32)ack, 1u << vmid, 1u << vmid);'
    if (-not $source.Contains($live)) { throw 'negative control: the acknowledge wait line moved' }
    $source = $source.Replace($live, 'bc250_gfx_vm_reg_wait(ring, (u32)ack, 1u << vmid, ~0u);')
}
[IO.File]::WriteAllText($underTest, $source)

#   C4245  as in run_gfx.ps1: AMD's PACKET3() in the imported nvd.h yields a signed int with bit 31
#          set, and every use assigns it to a u32.
$packetWarn = @('/wd4245')
$packetSources = @($underTest, (Join-Path $shim 'bc250_ring.c'), (Join-Path $shim 'bc250_sdma.c'))
$plainSources = @('shim.c', 'bc250_gmc.c', 'bc250_gart.c', 'bc250_nbio.c', 'bc250_irq.c') |
    ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\gfx_vm_flush.c'))
# The same two warnings run_gfx.ps1 turns off for the imported hub sources, for the same reasons.
$importWarn = @('/wd4244', '/wd4701')
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c') |
    ForEach-Object { Join-Path $imports $_ }

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, gfx frame and ring VM flush packets)'
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
        "/OUT:$Out\gfx_vm_flush.exe", "/PDB:$Out\gfx_vm_flush.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\gfx_vm_flush.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "gfx_vm_flush.exe exit code $code"
exit $code
