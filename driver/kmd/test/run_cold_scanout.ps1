# BD-341: the firmware scan-out size that a start without a firmware display keeps clear at the bottom of VRAM, on the
# host. The production bodies of modeset.c ModesetFirmwareScanoutBytes and vram.c VramFramebufferExtent are compiled
# unchanged against cold_scanout_test.c (docs/design/wddm-startup.md, "Start without a firmware display").
#
#   pwsh -NoProfile -File driver\kmd\test\run_cold_scanout.ps1 -Root <workspace> -Out <dir>
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [Parameter(Mandatory)][string]$Out,
    [string]$Kits = ''
)
$ErrorActionPreference = 'Stop'
if (!$Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null

function Get-Body([string]$File, [string]$Signature) {
    $text = Get-Content (Join-Path $repo "driver\kmd\$File") -Raw
    $first = $text.IndexOf($Signature)
    if ($first -lt 0 -or $text.IndexOf($Signature, $first + 1) -ge 0) { throw "${File}: '$Signature' does not occur exactly once" }
    $last = $text.IndexOf("`n}", $first)
    if ($last -lt 0) { throw "${File}: no end of '$Signature'" }
    return $text.Substring($first, $last + 2 - $first)
}
$body = (Get-Body 'modeset.c' 'ULONG ModesetFirmwareScanoutBytes(') + "`n" + (Get-Body 'vram.c' 'BOOLEAN VramFramebufferExtent(')
[IO.File]::WriteAllText((Join-Path $Out 'cold_scanout_actual.inc'), $body)
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
& $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', "/I$Out", "/I$repo\driver\kmd", "/I$repo\third_party\linux-amdgpu",
    "/I$($msvc.FullName)\include", "/I$sdk\Include\10.0.26100.0\ucrt", "/Fo$obj\", "/Fe$Out\cold_scanout_test.exe",
    "$PSScriptRoot\cold_scanout_test.c", '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64",
    "/LIBPATH:$sdklib\um\x64")
if ($LASTEXITCODE -ne 0) { throw 'Host build failed' }
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\cold_scanout_test.exe"