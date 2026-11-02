# Builds vk_wbs_hang.c for x86 and x64 with clang-cl and lld-link. Headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the Vulkan headers from -VulkanInclude, the C runtime libraries from the
# installed Visual Studio Build Tools. /O2 /MT /Brepro and no PDB, so two builds of one source give the same bytes.
# Both architectures by default: the ICD comes as a pair, and an x86 ICD only loads into a 32-bit process.
#
#   pwsh tools\win\vkwbs\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\vkwbs\build.ps1 -Kits ... -Arch x64
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = '',
    [string]$ClangToolchain = '',
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both'
)
$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\vkwbs' }
if (-not $VulkanInclude) { $VulkanInclude = Join-Path $Root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include' }
if (-not $ClangToolchain) { $ClangToolchain = Join-Path $Root 'toolchain\llvm-23.1.4-win64' }
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
$cc = Join-Path $ClangToolchain 'bin\clang-cl.exe'
$ld = Join-Path $ClangToolchain 'bin\lld-link.exe'
foreach ($t in @($cc, $ld)) { if (-not (Test-Path -LiteralPath $t)) { throw "not found: $t (the compiler is clang-cl, no fallback)" } }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$src = Join-Path $PSScriptRoot 'vk_wbs_hang.c'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP; $env:INCLUDE = ''; $env:LIB = ''
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
"compiler: $(& $cc --version | Select-Object -First 1)"
$targets = @(
    @{ a = 'x86'; t = 'i686-pc-windows-msvc'; n = '32' },
    @{ a = 'x64'; t = 'x86_64-pc-windows-msvc'; n = '64' }
) | Where-Object { $Arch -eq 'both' -or $_.a -eq $Arch }
foreach ($tg in $targets) {
    $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$($tg.a)\c"
    $obj = Join-Path $Out "vk_wbs_hang_$($tg.n).obj"
    $exe = Join-Path $Out "vk_wbs_hang_$($tg.n).exe"
    & $cc --target=$($tg.t) /nologo /W3 /O2 /MT /TC /std:c11 /Brepro /D_CRT_SECURE_NO_WARNINGS `
        "/I$VulkanInclude" "/imsvc$(Join-Path $msvc.FullName 'include')" "/imsvc$sdk\Include\$KitVersion\ucrt" `
        "/imsvc$sdk\Include\$KitVersion\um" "/imsvc$sdk\Include\$KitVersion\shared" /c "/Fo$obj" $src
    if ($LASTEXITCODE -ne 0) { throw "clang-cl $($tg.a) failed ($LASTEXITCODE)" }
    & $ld /nologo /Brepro /SUBSYSTEM:CONSOLE "/OUT:$exe" $obj "/LIBPATH:$(Join-Path $msvc.FullName "lib\$($tg.a)")" `
        "/LIBPATH:$sdkLib\ucrt\$($tg.a)" "/LIBPATH:$sdkLib\um\$($tg.a)" kernel32.lib psapi.lib bcrypt.lib
    if ($LASTEXITCODE -ne 0) { throw "lld-link $($tg.a) failed ($LASTEXITCODE)" }
    Remove-Item $obj
    "$($tg.a) $exe sha256 $((Get-FileHash $exe).Hash)"
}
"msvc crt $($msvc.Name), sdk $KitVersion"
