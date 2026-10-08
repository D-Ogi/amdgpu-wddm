# Host tests for display modes, stage A: the EDID parser and mode list (edid.c), the DP AUX engine sequence and
# EDID read (dpaux.c, display_io.c) and the scaler math and register sequence (dcn_scale.c), each compiled as the
# miniport compiles it and driven by its test (edid_test.c, dpaux_test.c, dcn_scale_test.c say what they prove).
# The run first checks that driver\kmd\scl_filter_4tap_64p_upscale.inc is still the verbatim extraction of the
# reference file (driver\amdgpu-import\PROVENANCE.md).
#
#   pwsh driver\kmd\test\run_modeset.ps1
#   pwsh driver\kmd\test\run_modeset.ps1 -Out $env:BC250_ROOT\scratch\modeset
#
# The binaries go under -Out. Nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\modeset",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path (Split-Path $kmd)
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Relative paths from the repository root: the generated banner records them (run_gfx.ps1 does the same).
Push-Location $repo
try {
    & python tools\import\extract_table.py --source driver/amdgpu-import/reference/dce_scl_filters.c `
        --array filter_4tap_64p_upscale --out driver/kmd/scl_filter_4tap_64p_upscale.inc `
        --kernel-path drivers/gpu/drm/amd/display/dc/dce/dce_scl_filters.c --check
    if ($LASTEXITCODE -ne 0) { throw 'scl_filter_4tap_64p_upscale.inc is not the extraction of its reference file' }
} finally { Pop-Location }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

$tests = [ordered]@{
    'edid_test'      = @('edid.c')
    'dpaux_test'     = @('dpaux.c', 'display_io.c', 'edid.c')
    'dcn_scale_test' = @('dcn_scale.c', 'display_io.c', 'edid.c')
}
$failed = @()
foreach ($t in $tests.Keys) {
    $tobj = Join-Path $obj $t
    New-Item -ItemType Directory -Force $tobj | Out-Null
    Remove-Item "$tobj\*.obj" -Force -ErrorAction SilentlyContinue
    $sources = @((Join-Path $here "$t.c")) + @($tests[$t] | ForEach-Object { Join-Path $kmd $_ })
    $clArgs = @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT',
        "/I$kmd", "/I$repo\third_party\linux-amdgpu", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/Fo$tobj\", "/Fe$Out\$t.exe") + $sources + @(
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64")
    & $cl @clArgs
    if ($LASTEXITCODE -ne 0) { throw "Host build of $t failed" }
    & "$Out\$t.exe"
    if ($LASTEXITCODE -ne 0) { $failed += $t }
}
if ($failed.Count) { Write-Host "modeset host tests failed: $($failed -join ', ')"; exit 1 }
Write-Host 'modeset host tests passed'
exit 0
