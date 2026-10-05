# Builds the BC-250 H.264 encoder MFT and its host test without a WDK or SDK installation: headers and
# import libraries come from the SDK NuGet packages under -Kits, the compiler from the installed Visual
# Studio. Same flow as tools\win\d3d11bench\build.ps1.
#
#   pwsh driver\umd\mft-h264\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#
# The shaders are compiled offline with fxc from the same SDK package, so the shipped DLL carries no
# d3dcompiler dependency and the bytecode is part of the reproducible artifact.
#
# Gates, in order, each one fatal:
#   1. every shader entry point compiles with /WX;
#   2. the DLL and the test compile with /W4 /WX;
#   3. neither binary imports a C runtime DLL (the MFT is loaded into Game Bar, Chromium and the frame
#      server, exactly the situation that made the RADV ICD use /MT - see bc250-win\docs\build.md);
#   4. the test's --selftest passes (H.264 table structure, bit writer round trip), with no GPU needed.

# The workspace root: BC250_ROOT when the caller set it, else four levels up from this script
# (driver\umd\mft-h264 -> the repository -> the workspace). Nothing of the build lands on C:.
param(
    [string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [switch]$SkipSelfTest
)

$root = if ($env:BC250_ROOT) { $env:BC250_ROOT }
        else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }
if (-not $Kits) { $Kits = Join-Path $root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\mft-h264' }

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here 'src'
$shaderDir = Join-Path $here 'shaders'
$gen = Join-Path $shaderDir 'gen'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$fxc = Join-Path $sdk "bin\$KitVersion\x64\fxc.exe"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$link = Join-Path $msvc.FullName 'bin\Hostx64\x64\link.exe'
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'

New-Item -ItemType Directory -Force $Out | Out-Null
New-Item -ItemType Directory -Force $gen | Out-Null
# Nothing of ours lands on drive C:, including the compiler's temporary files.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:INCLUDE = ''; $env:LIB = ''

Write-Host "msvc    $($msvc.Name)"
Write-Host "sdk     $KitVersion"

# ---------------------------------------------------------------- shaders

$shaders = @(
    @{ File = 'cs_import.hlsl';  Entry = 'CSImportNV12';     Header = 'cs_import_nv12.h' },
    @{ File = 'cs_import.hlsl';  Entry = 'CSImportNV12Sys';  Header = 'cs_import_nv12sys.h' },
    @{ File = 'cs_import.hlsl';  Entry = 'CSImportBGRA';     Header = 'cs_import_bgra.h' },
    @{ File = 'cs_import.hlsl';  Entry = 'CSImportPlanar';   Header = 'cs_import_planar.h' },
    @{ File = 'cs_me.hlsl';      Entry = 'CSMotionEstimate'; Header = 'cs_motion.h' },
    @{ File = 'cs_mb.hlsl';      Entry = 'CSEncodeIntra';    Header = 'cs_encode_intra.h' },
    @{ File = 'cs_mb.hlsl';      Entry = 'CSEncodeInter';    Header = 'cs_encode_inter.h' },
    @{ File = 'cs_deblock.hlsl'; Entry = 'CSDeblock';        Header = 'cs_deblock.h' }
)

foreach ($s in $shaders) {
    $outHeader = Join-Path $gen $s.Header
    $args = @('/nologo', '/T', 'cs_5_0', '/E', $s.Entry, '/O3', '/WX',
              '/Fh', $outHeader, '/Vn', "g_$($s.Entry)", (Join-Path $shaderDir $s.File))
    $log = & $fxc @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host "  $_" }
        throw "fxc failed for $($s.Entry) ($LASTEXITCODE)"
    }
    $bytes = (Get-Item $outHeader).Length
    Write-Host ("  shader {0,-18} {1,8} bytes of header" -f $s.Entry, $bytes)
}

# The test's own source picture: a full-screen draw on the test's device, so the encoder is fed a
# real GPU surface. Test-only, never part of the shipped DLL.
$testGen = Join-Path $here 'tests\gen'
New-Item -ItemType Directory -Force $testGen | Out-Null
$testShaders = @(
    @{ Target = 'vs_5_0'; Entry = 'VSFullscreen';  Header = 'vs_fullscreen.h' },
    @{ Target = 'ps_5_0'; Entry = 'PSTestPattern'; Header = 'ps_testpattern.h' }
)
foreach ($s in $testShaders) {
    $outHeader = Join-Path $testGen $s.Header
    $log = & $fxc @('/nologo', '/T', $s.Target, '/E', $s.Entry, '/O3', '/WX',
                    '/Fh', $outHeader, '/Vn', "g_$($s.Entry)", (Join-Path $here 'tests\testpattern.hlsl')) 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host "  $_" }
        throw "fxc failed for $($s.Entry) ($LASTEXITCODE)"
    }
    Write-Host ("  shader {0,-18} {1,8} bytes of header" -f $s.Entry, (Get-Item $outHeader).Length)
}

# ---------------------------------------------------------------- common compiler flags

$inc = @("/I$(Join-Path $msvc.FullName 'include')",
         "/I$sdk\Include\$KitVersion\ucrt",
         "/I$sdk\Include\$KitVersion\um",
         "/I$sdk\Include\$KitVersion\shared",
         "/I$sdk\Include\$KitVersion\winrt",
         "/I$src", "/I$shaderDir")
# /Brepro removes the timestamp from the object and the image, so the same sources give the same bytes.
$cflags = @('/nologo', '/c', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/GR-',
            '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/Brepro', '/Zc:inline')
$libpath = @("/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
             "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")
$libs = @('mfplat.lib', 'mfuuid.lib', 'mf.lib', 'mfreadwrite.lib', 'd3d11.lib', 'dxgi.lib',
          'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'kernel32.lib', 'user32.lib', 'advapi32.lib',
          'bcrypt.lib', 'wmcodecdspuuid.lib')

function Invoke-Tool([string]$exe, [string[]]$toolArgs, [string]$what) {
    $log = & $exe @toolArgs 2>&1
    $log | ForEach-Object { "$_" } |
        Where-Object { $_ -and $_ -notmatch '^Microsoft \(R\)|^Copyright \(C\)|^\s*$|^\S+\.cpp$|^   Creating library' } |
        ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" }
}

# ---------------------------------------------------------------- core objects

$core = @('h264_tables.cpp', 'h264_syntax.cpp', 'h264_cavlc.cpp', 'gpu_pipeline.cpp',
          'encoder.cpp', 'mft_h264.cpp', 'mft_register.cpp')
$coreObjs = @()
foreach ($c in $core) {
    $obj = Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($c) + '.obj')
    Invoke-Tool $cl ($cflags + $inc + @("/Fo$obj", (Join-Path $src $c))) "cl $c"
    $coreObjs += $obj
}

# ---------------------------------------------------------------- the transform DLL

$dllObj = Join-Path $Out 'dllmain.obj'
Invoke-Tool $cl ($cflags + $inc + @("/Fo$dllObj", (Join-Path $src 'dllmain.cpp'))) 'cl dllmain.cpp'

$dll = Join-Path $Out 'amdgpu_wddm_mft_h264.dll'
if (Test-Path -LiteralPath $dll) {
    $hash = (Get-FileHash -LiteralPath $dll).Hash
    $keep = Join-Path $Out "retained\amdgpu_wddm_mft_h264-$hash.dll"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $dll -Destination $keep
    }
    Write-Host "  previous DLL retained as retained\amdgpu_wddm_mft_h264-$hash.dll"
}
Invoke-Tool $link (@('/nologo', '/DLL', '/Brepro', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF',
                     "/DEF:$(Join-Path $src 'amdgpu_wddm_mft_h264.def')", "/OUT:$dll",
                     "/IMPLIB:$Out\amdgpu_wddm_mft_h264.lib") +
                   $coreObjs + @($dllObj) + $libpath + $libs) 'link dll'

# ---------------------------------------------------------------- the host test

$testObjs = @()
foreach ($t in @('mfthost.cpp', 'mfthost_mf.cpp')) {
    $obj = Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($t) + '.obj')
    Invoke-Tool $cl ($cflags + $inc + @("/I$here\tests", "/Fo$obj", (Join-Path $here "tests\$t"))) "cl $t"
    $testObjs += $obj
}
$exe = Join-Path $Out 'mfthost.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\mfthost-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
}
# dllmain.obj comes along so that the test registers the same class factory the DLL exposes.
Invoke-Tool $link (@('/nologo', '/Brepro', '/INCREMENTAL:NO', "/OUT:$exe") +
                   $coreObjs + @($dllObj) + $testObjs + $libpath + $libs) 'link mfthost'

# ---------------------------------------------------------------- the registration tool

$regObj = Join-Path $Out 'mftreg.obj'
Invoke-Tool $cl ($cflags + $inc + @("/Fo$regObj", (Join-Path $here 'tools\mftreg\mftreg.cpp'))) 'cl mftreg.cpp'
$regExe = Join-Path $Out 'mftreg.exe'
Invoke-Tool $link (@('/nologo', '/Brepro', '/INCREMENTAL:NO', "/OUT:$regExe") +
                   $coreObjs + @($regObj) + $libpath + $libs) 'link mftreg'

# ---------------------------------------------------------------- gate: no C runtime DLL import

foreach ($bin in @($dll, $exe, $regExe)) {
    # dumpbin's own "Dump of file <path>.dll" line also ends in .dll, so it has to go out first.
    $imports = & $dumpbin '/nologo' '/imports' $bin 2>&1 |
        Where-Object { $_ -match '\.dll$' -and $_ -notmatch '^Dump of file' } |
        ForEach-Object { $_.Trim() } | Sort-Object -Unique
    $bad = $imports | Where-Object { $_ -match '(?i)^(vcruntime|msvcp|msvcr|api-ms-win-crt)' }
    if ($bad) {
        throw "$([IO.Path]::GetFileName($bin)) imports a C runtime DLL: $($bad -join ', ')"
    }
    Write-Host ("  imports {0,-28} {1}" -f [IO.Path]::GetFileName($bin), ($imports -join ' '))
}

# ---------------------------------------------------------------- gate: self test

if (-not $SkipSelfTest) {
    & $exe --selftest
    if ($LASTEXITCODE -ne 0) { throw "mfthost --selftest failed ($LASTEXITCODE)" }
}

Get-Item $dll, $exe, $regExe | ForEach-Object {
    '{0,9}  {1,-30}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash
}
