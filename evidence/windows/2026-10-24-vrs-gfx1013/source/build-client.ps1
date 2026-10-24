#Requires -Version 7.0
# Builds vrsprobe-x64.exe with clang-cl (LLVM 23.1.4, compiler policy of 2026-10-21) against the MSVC runtime of the
# installed Visual Studio and the Windows SDK NuGet packages in toolchain\nuget. Shaders: glslc of toolchain\vk-build.
# Then: --help, a usage check, the host self-test of the image analysis, and the SHA-256 of the artifact.
param([string]$Out = 'P:\BC-250\scratch\vrs-probe\client\build',
      [string]$Kits = 'P:\BC-250\toolchain\nuget', [string]$KitVersion = '10.0.26100.0',
      [string]$VulkanInclude = 'P:\BC-250\scratch\m15\vkd3d\khronos\Vulkan-Headers\include')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$clang = 'P:\BC-250\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
$glslc = 'P:\BC-250\toolchain\vk-build\Library\bin\glslc.exe'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = 'P:\BC-250\scratch\vrs-probe\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
"compiler $((& $clang --version | Select-Object -First 1))"
"glslc    $((& $glslc --version | Select-Object -First 1))"

$spv = @(
    @{ Name = 'kVsPlain'; Src = 'vrsprobe.vert'; Defs = @() },
    @{ Name = 'kVsRate'; Src = 'vrsprobe.vert'; Defs = @('-DWRITE_RATE') },
    @{ Name = 'kFsPlain'; Src = 'vrsprobe.frag'; Defs = @() },
    @{ Name = 'kFsRate'; Src = 'vrsprobe.frag'; Defs = @('-DREAD_RATE') })
$genArgs = @()
foreach ($s in $spv) {
    $o = Join-Path $Out "$($s.Name).spv"
    & $glslc --target-env=vulkan1.3 -O @($s.Defs) -o $o (Join-Path $here $s.Src)
    if ($LASTEXITCODE -ne 0) { throw "glslc failed for $($s.Name)" }
    $genArgs += "$($s.Name)=$o"
}
& python (Join-Path $here 'gen_spv_header.py') (Join-Path $Out 'vrsprobe_spv.h') @genArgs
if ($LASTEXITCODE -ne 0) { throw 'header generation failed' }

$exe = Join-Path $Out 'vrsprobe-x64.exe'
if (Test-Path -LiteralPath $exe) {
    $h = (Get-FileHash -LiteralPath $exe).Hash
    New-Item -ItemType Directory -Force (Join-Path $Out 'retained') | Out-Null
    Copy-Item -LiteralPath $exe (Join-Path $Out "retained\vrsprobe-x64-$h.exe") -Force
}
$env:INCLUDE = ''; $env:LIB = ''
& $clang @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld',
    # Vulkan structures start as { sType } with the rest zero, on purpose.
    '-Wno-missing-field-initializers',
    "/I$Out", "/imsvc$VulkanInclude", "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt",
    "/imsvc$sdk\Include\$KitVersion\um", "/imsvc$sdk\Include\$KitVersion\shared", "/Fo$Out\vrsprobe-x64.obj", "/Fe$exe",
    (Join-Path $here 'vrsprobe.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib', 'psapi.lib', 'bcrypt.lib')
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }

& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "help check failed ($LASTEXITCODE)" }
& $exe --bad-option 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "usage check failed ($LASTEXITCODE)" }
& $exe --self-test
if ($LASTEXITCODE -ne 0) { throw "self-test failed ($LASTEXITCODE)" }
'{0,9}  {1}  sha256 {2}' -f (Get-Item $exe).Length, (Split-Path -Leaf $exe), (Get-FileHash -LiteralPath $exe).Hash
