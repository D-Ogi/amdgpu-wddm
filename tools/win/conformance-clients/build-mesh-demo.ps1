param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)
# Builds mesh-demo.exe, the windowed D3D12 mesh shader demo: the three programs of shaders\mesh-demo.hlsl with the
# SDK's dxc into <Out>\gen\mesh_demo_*.h, then one cl invocation, then the CPU-only gates (--help 0, --invalid 2).
# The generated headers live in the output directory, not in the checkout: the demo is a thing to watch, not a
# witness, so its DXIL has no tracked record. The previous exe stays under retained\ by its hash.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"
if (-not (Test-Path -LiteralPath $dxc)) { throw "dxc not found: $dxc" }

# The workspace root: BC250_ROOT when it is set, else the grandparent of -Kits, which is <workspace>\toolchain\nuget.
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\mesh-demo' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Temporary files of the compiler stay in the workspace, never on the system drive.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Shaders: one header per program, all shader model 6.5, the dxc identity next to them in gen\dxc-mesh-demo.txt.
$gen = Join-Path $Out 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$programs = @(
    @{ Name = 'mesh_demo_as'; Profile = 'as_6_5'; Entry = 'as_main' },
    @{ Name = 'mesh_demo_ms'; Profile = 'ms_6_5'; Entry = 'ms_main' },
    @{ Name = 'mesh_demo_ps'; Profile = 'ps_6_5'; Entry = 'ps_main' }
)
$source = Join-Path $here 'shaders\mesh-demo.hlsl'
$log = @("dxc $dxc", "dxc version $(& $dxc --version 2>&1 | Select-Object -First 1)",
         "dxc sha256 $((Get-FileHash -LiteralPath $dxc).Hash)", "source sha256 $((Get-FileHash -LiteralPath $source).Hash)")
foreach ($p in $programs) {
    $header = Join-Path $gen "$($p.Name).h"
    $arguments = @('-nologo', '-T', $p.Profile, '-E', $p.Entry, '-Vn', "g_$($p.Name)", '-Fh', $header, $source)
    & $dxc @arguments
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $($p.Name) ($LASTEXITCODE)" }
    Write-Host "  dxc $($p.Profile) $($p.Entry) -> gen\$($p.Name).h"
    $log += "dxc -T $($p.Profile) -E $($p.Entry) -Vn g_$($p.Name) -Fh gen\$($p.Name).h shaders\mesh-demo.hlsl"
}
$log | Set-Content -LiteralPath (Join-Path $gen 'dxc-mesh-demo.txt') -Encoding ascii

# A previous binary is never lost to a rebuild: it stays under retained\ by its full hash.
$exe = Join-Path $Out 'mesh-demo.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\mesh-demo-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\mesh-demo-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
$clArgs = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE', "/I$Out",
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\mesh-demo.obj",
    "/Fe$exe", (Join-Path $here 'mesh-demo.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'user32.lib', 'kernel32.lib')
& $cl @clArgs | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --invalid 2>$null
if ($LASTEXITCODE -ne 2) { throw 'Invalid CLI accepted' }
Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
