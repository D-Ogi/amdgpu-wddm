# Builds nv12layout.exe (BD-223 V7: the linear NV12/P010 layout of the Vulkan driver) with clang-cl of LLVM 23.1.4, for
# x64 or x86 (PE machine checked). Vulkan headers: -VulkanHeaders (include/vulkan/include of the DXVK fork by default).
# Prints the SHA256 of the result, then checks the CLI: --help exits 0, an unknown argument and a run without --out
# exit 2.
param([ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\BC-250' })\scratch\build\nv12layout",
      [Parameter(Mandatory)][string]$VulkanHeaders, [string]$VsInstall)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$root = Get-Bc250Root $repo
if (-not (Test-Path -LiteralPath (Join-Path $VulkanHeaders 'vulkan\vulkan.h'))) { throw "no vulkan\vulkan.h under $VulkanHeaders" }
$Out = Join-Path $Out $Arch
$tmp = Join-Path $Out 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $tmp; $env:TMP = $tmp
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $tmp -Arch $Arch
    $cc = Resolve-Bc250UmdCompiler $root 'clang-cl' '' $Out $Arch
    $exe = Join-Path $Out 'nv12layout.exe'
    $flags = ConvertTo-Bc250ClangFlags @('/nologo', '/O2', '/EHsc', '/std:c++17', '/W4', '/WX', '/MT', '/Brepro') $Arch @($repo)
    Invoke-Bc250UmdCompiler $cc @flags "/imsvc$VulkanHeaders" (Join-Path $PSScriptRoot 'nv12layout.cpp') "/Fo$tmp\nv12layout.obj" "/Fe$exe" `
        /link /Brepro /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw "clang-cl exit $LASTEXITCODE" }
} finally { Restore-ProcessEnvironment $saved }
Remove-Item -Recurse -Force -LiteralPath $tmp
$bytes = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
$wantMachine = if ($Arch -eq 'x86') { 0x014C } else { 0x8664 }
if ($machine -ne $wantMachine) { throw ('PE machine {0:X4}, expected {1:X4}' -f $machine, $wantMachine) }
& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --unknown 2>$null
if ($LASTEXITCODE -ne 2) { throw 'an unknown argument was accepted' }
& $exe 2>$null
if ($LASTEXITCODE -ne 2) { throw 'a run without --out was accepted' }
('nv12layout.exe {0} machine {1:X4} {2}' -f $Arch, $machine, (Get-FileHash $exe -Algorithm SHA256).Hash)
