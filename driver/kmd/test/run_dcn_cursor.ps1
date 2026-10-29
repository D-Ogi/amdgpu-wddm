# Host test for driver\kmd\dcn_cursor_shape.h: the conversion of the WDDM pointer shapes into the DCN cursor surface,
# the hot spot rule, the placement with its negative clamp, its edges and its destination offset, and the rotation
# against display_rotation.h (docs/design/hardware-pointer.md). The test includes the production headers, so there is
# nothing to extract.
#
#   pwsh driver\kmd\test\run_dcn_cursor.ps1
#   pwsh driver\kmd\test\run_dcn_cursor.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_dcn_cursor.ps1 -Mutation lsb-first
#
# -Mutation changes one rule in a COPY of dcn_cursor_shape.h, never the tree, and the test must then fail with a FAIL
# line. A mutation whose text is no longer in the header stops the run instead of passing quietly. -Mutation all runs
# every control and fails if any of them passes. The binaries go under -Out; nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\dcn-cursor",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'all', 'xor-mask-offset', 'lsb-first', 'no-negative-clamp', 'rotate-90-clockwise',
                 'mask-not-normalized', 'xor-ignored', 'edge-viewport-origin', 'dst-ratio-inverted', 'hotspot-swapped',
                 'pitch-ignored', 'bytes-one-mask', 'hotspot-unbounded')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Each control: the exact text of one rule in dcn_cursor_shape.h, what replaces it, and the rule it breaks.
$mutations = [ordered]@{
    'xor-mask-offset' = @{ find = 'p + ((unsigned long long)Shape->Height + J) * Shape->Pitch'
        with = 'p + ((unsigned long long)1u + J) * Shape->Pitch'
        why = 'the monochrome XOR mask is read one row after the AND mask, not Height rows after it' }
    'lsb-first' = @{ find = '(Row[I >> 3] >> (7u - (I & 7u))) & 1u'; with = '(Row[I >> 3] >> (I & 7u)) & 1u'
        why = 'the least significant bit of a mask byte is taken as the leftmost pixel' }
    'no-negative-clamp' = @{ find = 'Out->HotX = left < 0 ? (unsigned int)-left : 0u;'; with = 'Out->HotX = 0u;'
        why = 'a pointer left of the plane jumps right by the part that is outside' }
    'rotate-90-clockwise' = @{ find = 'case 90u:  *I = Width - 1u - V; *J = U;              break;'
        with = 'case 90u:  *I = V; *J = Height - 1u - U; break;'
        why = 'the portrait pointer image turns the other way from the desktop' }
    'mask-not-normalized' = @{ find = 'if (Shape->Flags == BC250_POINTER_MASKED_COLOR) return (word & 0x00FFFFFFu) | ((word & 0x80000000u) ? 0xFF000000u : 0u);'
        with = 'if (Shape->Flags == BC250_POINTER_MASKED_COLOR) return word;'
        why = 'a masked-colour alpha byte other than 0 and 0xFF reaches the hardware as a partial alpha' }
    'xor-ignored' = @{ find = 'if ((word & 0xFF000000u) && (word & 0x00FFFFFFu)) xorPixels++;'
        with = 'if ((word & 0xFF000000u) && (word & 0x00FFFFFFu) && !word) xorPixels++;'
        why = 'an inverting pixel is converted to straight alpha and shows as a transparent hole' }
    'edge-viewport-origin' = @{ find = 'rx = left - (Plane->Map ? Plane->ViewportX : 0);'; with = 'rx = left;'
        why = 'the edges of an overlay plane with a viewport origin are taken from the surface origin' }
    'dst-ratio-inverted' = @{ find = 'dst = dst * (long long)Plane->DstNumerator / (long long)Plane->DstDenominator;'
        with = 'dst = dst * (long long)Plane->DstDenominator / (long long)Plane->DstNumerator;'
        why = 'the destination offset of a scaled plane is divided by recout over viewport instead of multiplied' }
    'hotspot-swapped' = @{ find = '{ X -= (int)XHot; Y -= (int)YHot; }'; with = '{ X -= (int)YHot; Y -= (int)XHot; }'
        why = 'the hot spot setting subtracts the hot spot of the other axis' }
    'pitch-ignored' = @{ find = 'unsigned int* row = Out + (unsigned long long)v * BC250_CURSOR_PITCH_PIXELS;'
        with = 'unsigned int* row = Out + (unsigned long long)v * Image->Width;'
        why = 'the surface rows are packed at the image width, not at CURSOR_PITCH 256' }
    'bytes-one-mask' = @{ find = '(Shape->Flags == BC250_POINTER_MONOCHROME ? 2u : 1u)'
        with = '(Shape->Flags == BC250_POINTER_MONOCHROME ? 1u : 1u)'
        why = 'the copy of a monochrome shape leaves out its XOR mask' }
    'hotspot-unbounded' = @{ find = 'if (Shape->XHot >= Shape->Width || Shape->YHot >= Shape->Height) return BC250_CURSOR_SHAPE_HOTSPOT;'
        with = 'if (Shape->XHot > Shape->Width || Shape->YHot > Shape->Height) return BC250_CURSOR_SHAPE_HOTSPOT;'
        why = 'a hot spot one pixel outside the image is taken' }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

# Builds and runs the test with the cursor header in $Include (display_rotation.h comes from the tree); returns the
# exit code and the output.
function Invoke-DcnCursor([string]$Dir, [string]$Include) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', "/I$Include", "/I$kmd", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Dir\dcn_cursor_test.exe",
        (Join-Path $here 'dcn_cursor_test.c'),
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\dcn_cursor_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

if (-not $Mutation) {
    $run = Invoke-DcnCursor $Out $kmd
    Write-Output $run.Output
    exit $run.Code
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$header = Get-Content -Raw (Join-Path $kmd 'dcn_cursor_shape.h')
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($header, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in dcn_cursor_shape.h, not once" }
    $dir = Join-Path $Out "mutation-$name"
    $include = Join-Path $dir 'include'
    New-Item -ItemType Directory -Force $include | Out-Null
    Set-Content -NoNewline -Path (Join-Path $include 'dcn_cursor_shape.h') -Value $header.Replace($m.find, $m.with)
    $run = Invoke-DcnCursor $dir $include
    $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
    Write-Output ("mutation {0}: {1} ({2})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "dcn cursor mutations: $($names.Count) caught"
exit 0
