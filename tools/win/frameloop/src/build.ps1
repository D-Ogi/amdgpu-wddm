param(
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0'
)
# Builds amdgpu_wddm_frameloop.exe: the two HLSL sources with the SDK's dxc into gen\*.h, then one cl
# invocation, then the CPU-only gates (--help 0, --invalid 2, --selftest 0). The previous exe is kept under
# retained\ by its hash, as the other clients in this workspace do, so a reviewed artifact is never lost.
#
# The binary never lands in the repository: without -Out it goes to <workspace>\scratch\m15\frameloop\build,
# which is where the lab runner looks for it. BC250_ROOT names the workspace root (by default the parent
# directory of this repository), BC250_FRAMELOOP_WORK the work directory under it.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
# The workspace root is the first directory above this one that holds the toolchain.
$root = if ($env:BC250_ROOT) { (Resolve-Path -LiteralPath $env:BC250_ROOT).Path } else {
    $probe = $here
    while ($probe -and -not (Test-Path -LiteralPath (Join-Path $probe 'toolchain'))) { $probe = Split-Path -Parent $probe }
    $probe
}
if (-not $root -or -not (Test-Path -LiteralPath (Join-Path $root 'toolchain'))) { throw "workspace root not found from $here" }
$work = if ($env:BC250_FRAMELOOP_WORK) { $env:BC250_FRAMELOOP_WORK } else { Join-Path $root 'scratch\m15\frameloop' }
if (-not $Kits) { $Kits = Join-Path $root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $work 'build' }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"
if (-not (Test-Path -LiteralPath $dxc)) { throw "dxc not found: $dxc" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Nothing is written to C:: the compiler's temporaries stay on P: with the rest of the build work.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Shaders: one header per program, shader model 6.0, the dxc identity recorded next to them.
$gen = Join-Path $here 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$programs = @(
    @{ Name = 'spin_cs';    Profile = 'cs_6_0'; Entry = 'cs_main'; File = 'spin.hlsl' },
    @{ Name = 'present_vs'; Profile = 'vs_6_0'; Entry = 'vs_main'; File = 'present.hlsl' },
    @{ Name = 'present_ps'; Profile = 'ps_6_0'; Entry = 'ps_main'; File = 'present.hlsl' }
)
$dxcVersion = (& $dxc --version 2>&1 | Select-Object -First 1)
$log = @("dxc $dxc", "dxc version $dxcVersion", "dxc sha256 $((Get-FileHash -LiteralPath $dxc).Hash)")
foreach ($p in $programs) {
    $source = Join-Path $here "shaders\$($p.File)"
    $header = Join-Path $gen "$($p.Name).h"
    $arguments = @('-nologo', '-T', $p.Profile, '-E', $p.Entry, '-Vn', "g_$($p.Name)", '-Fh', $header, $source)
    & $dxc @arguments
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $($p.Name) ($LASTEXITCODE)" }
    $log += "dxc -nologo -T $($p.Profile) -E $($p.Entry) -Vn g_$($p.Name) -Fh gen\$($p.Name).h shaders\$($p.File)"
}
$log | Set-Content -LiteralPath (Join-Path $gen 'dxc.txt') -Encoding ascii

$exe = Join-Path $Out 'amdgpu_wddm_frameloop.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\amdgpu_wddm_frameloop-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\amdgpu_wddm_frameloop-$hash.exe"
}

# No dxgi.lib and no d3d12.lib: both runtime DLLs are loaded by full System32 path at startup, so an
# application-local copy next to the exe cannot be picked up.
# /Brepro on both the compiler and the linker: without it MSVC stamps the time of the build into the image and
# two builds of the same sources hash differently, so the SHA-256 in a trial record would name a build instead
# of a source revision. With it the exe is a function of its inputs and the hash can be re-derived.
$env:INCLUDE = ''; $env:LIB = ''
$clArgs = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/Brepro', '/DUNICODE', '/D_UNICODE', "/I$here",
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt",
    "/Fo$Out\amdgpu_wddm_frameloop.obj", "/Fe$exe", (Join-Path $here 'main.cpp'), '/link', '/Brepro',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'user32.lib', 'kernel32.lib')
& $cl @clArgs | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# The gates run the exe, and a refused option prints its usage to stderr. A native command's stderr becomes an
# error record, which $ErrorActionPreference = 'Stop' turns into a failed build of a sound exe, so the gates run
# with it on 'Continue' and are judged by their exit codes alone.
$strict = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $exe --help 2>&1 | Out-Null
$helpCode = $LASTEXITCODE
& $exe --invalid 2>&1 | Out-Null
$invalidCode = $LASTEXITCODE
$selftest = & $exe --selftest 2>&1
$selftestCode = $LASTEXITCODE
$ErrorActionPreference = $strict
if ($helpCode -ne 0) { throw "help check failed (exit $helpCode)" }
if ($invalidCode -ne 2) { throw "an invalid CLI was accepted (exit $invalidCode)" }
if ($selftestCode -ne 0) { $selftest | Write-Host; throw 'selftest failed' }
Write-Host "  $($selftest | Select-Object -Last 1)"
Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
