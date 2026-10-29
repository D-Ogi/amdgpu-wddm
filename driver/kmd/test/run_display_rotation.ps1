# Host test for driver\kmd\display_rotation.h: the path rotations the driver shows, the rotation support it states
# and the rotated bugcheck copy (BD-247, docs/design/display-rotation.md). The test includes the production header,
# so there is nothing to extract.
#
#   pwsh driver\kmd\test\run_display_rotation.ps1
#   pwsh driver\kmd\test\run_display_rotation.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_display_rotation.ps1 -Mutation clockwise-90
#
# -Mutation changes one rule in a COPY of display_rotation.h, never the tree, and the test must then fail with a FAIL
# line. A mutation whose text is no longer in the header stops the run instead of passing quietly. -Mutation all runs
# every control and fails if any of them passes. The binaries go under -Out; nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\display-rotation",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'all', 'clockwise-90', 'offset-admitted', 'display-only-rotates', 'offset90-stated',
                 'size-not-swapped', 'stride-unclipped', 'value-180-lost')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Each control: the exact text of one rule in display_rotation.h, what replaces it, and the rule it breaks.
$mutations = [ordered]@{
    'clockwise-90' = @{ find = 'case 90u:  *Px = Y;                      *Py = PhysicalHeight - 1u - X; break;'
        with = 'case 90u:  *Px = PhysicalWidth - 1u - Y; *Py = X; break;'
        why = 'portrait turns the bugcheck screen clockwise, the way of 270 degrees' }
    'offset-admitted' = @{ find = 'case BC250_VPPR_NOTSPECIFIED:'
        with = 'case BC250_VPPR_NOTSPECIFIED: case BC250_VPPR_IDENTITY_OFFSET90:'
        why = 'a path-independent offset (clone) is shown as the identity' }
    'display-only-rotates' = @{ find = 'case BC250_VPPR_ROTATE90: return FullWddm ? 90u : BC250_ROTATION_REFUSED;'
        with = 'case BC250_VPPR_ROTATE90: return FullWddm ? 90u : 90u;'
        why = 'the display-only table, whose present copies rows unrotated, admits portrait' }
    'offset90-stated' = @{ find = '(Support).Offset90 = 0;'; with = '(Support).Offset90 = (FullWddm) ? 1 : 0;'
        why = 'the primary path states Offset90, which bugchecks a test-signed system (E16 run 007)' }
    'size-not-swapped' = @{ find = 'const int swap = Angle == 90u || Angle == 270u;'; with = 'const int swap = Angle == 90u;'
        why = 'the bugcheck screen of portrait (flipped) keeps the landscape size' }
    'stride-unclipped' = @{ find = 'if (width > SourceStride / 4u) width = SourceStride / 4u;'
        with = 'if (width > SourceStride / 4u && !width) width = SourceStride / 4u;'
        why = 'a block wider than its source stride reads past each source row' }
    'value-180-lost' = @{ find = 'Angle == 180u ? BC250_VPPR_ROTATE180'; with = 'Angle == 180u ? BC250_VPPR_ROTATE270'
        why = 'the device records landscape (flipped) as portrait (flipped)' }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

# Builds and runs the test with the header in $Include; returns the exit code and the output.
function Invoke-DisplayRotation([string]$Dir, [string]$Include) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', "/I$Include", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Dir\display_rotation_test.exe",
        (Join-Path $here 'display_rotation_test.c'),
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\display_rotation_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

if (-not $Mutation) {
    $run = Invoke-DisplayRotation $Out $kmd
    Write-Output $run.Output
    exit $run.Code
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$header = Get-Content -Raw (Join-Path $kmd 'display_rotation.h')
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($header, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in display_rotation.h, not once" }
    $dir = Join-Path $Out "mutation-$name"
    $include = Join-Path $dir 'include'
    New-Item -ItemType Directory -Force $include | Out-Null
    Set-Content -NoNewline -Path (Join-Path $include 'display_rotation.h') -Value $header.Replace($m.find, $m.with)
    $run = Invoke-DisplayRotation $dir $include
    $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
    Write-Output ("mutation {0}: {1} ({2})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "display rotation mutations: $($names.Count) caught"
exit 0
