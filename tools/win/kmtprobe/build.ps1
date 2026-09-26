# Builds kmtprobe.exe without a WDK or SDK installation: headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250kmd_cli\build.ps1.
#
#   pwsh tools\win\kmtprobe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\kmtprobe

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\kmtprobe",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null

# kmtprobe.c repeats constants that belong to the driver and to the vendored AMD headers: the allocation
# private blob's magic, the SET_UCONFIG_REG packet numbers and the BAR5 offset of GC.SCRATCH_REG0. Repo rules
# 1 and 2 - no register address or name from memory - only hold if a copy that drifts is a build failure, so
# each one is compared against its source here. A missing source is a warning, not a silent pass.
$tool = Join-Path $here 'kmtprobe.c'
function Assert-Constant($SourcePath, $Pattern, $Name) {
    if (-not (Test-Path $SourcePath)) {
        Write-Warning "$SourcePath not found, skipping the $Name check"
        return
    }
    $inSource = (Select-String -Path $SourcePath -Pattern $Pattern | Select-Object -First 1)
    $inTool = (Select-String -Path $tool -Pattern $Pattern | Select-Object -First 1)
    if ($null -eq $inSource) { throw "$Name not found in $SourcePath - has it been renamed?" }
    if ($null -eq $inTool) { throw "$Name not found in kmtprobe.c" }
    $a = $inSource.Matches[0].Groups[1].Value
    $b = $inTool.Matches[0].Groups[1].Value
    if ([Convert]::ToUInt64(($a -replace '^0x', ''), 16) -ne [Convert]::ToUInt64(($b -replace '^0x', ''), 16)) {
        throw "$Name differs: $(Split-Path -Leaf $SourcePath) $a, kmtprobe.c $b"
    }
    Write-Host "  $Name $b matches $(Split-Path -Leaf $SourcePath)"
}

$kmd = Join-Path $here '..\..\..\driver\kmd'
$import = Join-Path $here '..\..\..\driver\amdgpu-import'
Assert-Constant (Join-Path $kmd 'wddm.c') '#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC\s+(0x[0-9A-Fa-f]+)' 'allocation private magic'
Assert-Constant (Join-Path $kmd 'regs.generated.h') '#define BC250_REG_GC_SCRATCH_REG0\s+(0x[0-9A-Fa-f]+)' 'GC.SCRATCH_REG0'
Assert-Constant (Join-Path $import 'nvd.h') '#define\s+PACKET3_SET_UCONFIG_REG\s+(0x[0-9A-Fa-f]+)' 'PACKET3_SET_UCONFIG_REG'
Assert-Constant (Join-Path $import 'nvd.h') '#define\s+PACKET3_SET_UCONFIG_REG_START\s+(0x[0-9A-Fa-f]+)' 'PACKET3_SET_UCONFIG_REG_START'
Assert-Constant (Join-Path $import 'nvd.h') '#define\s+PACKET3_SET_UCONFIG_REG_END\s+(0x[0-9A-Fa-f]+)' 'PACKET3_SET_UCONFIG_REG_END'
Assert-Constant (Join-Path $import 'nvd.h') '#define\s+PACKET3_NOP\s+(0x[0-9A-Fa-f]+)' 'PACKET3_NOP'

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\kmtprobe.obj", "/Fe$Out\kmtprobe.exe",
    (Join-Path $here 'kmtprobe.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

Get-Item "$Out\kmtprobe.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
