param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    # Host test build only (its own output directory): the DISPATCH_RAYS command signature counts as refused, as the
    # D3D12 shell refuses it today. It checks the client's report of that path on WARP. Never a lab artifact.
    [switch]$TestRefuseSignature
)
# Builds amdgpu_wddm_conformance.exe: the shaders with the SDK's dxc into gen\*.h, then one cl invocation, then the
# CPU-only gates (--help 0, --invalid 2, --selftest 0). The previous exe stays under retained\ by its hash.
# The client takes the interactive protocol from ..\d3d12queue\interactive.h, which includes conformance.h when
# INTERACTIVE_CONFORMANCE is defined.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$queue = (Resolve-Path (Join-Path $here '..\d3d12queue')).Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"
if (-not (Test-Path -LiteralPath $dxc)) { throw "dxc not found: $dxc" }

# The workspace root: BC250_ROOT when it is set, else the grandparent of -Kits, which is
# <workspace>\toolchain\nuget. It is taken from -Kits and never from this script's own place, because a
# build in a git worktree sits under <workspace>\scratch\... and the parent of the checkout is then not
# the workspace. A wrong root used to put the compiler temporary files beside the worktree and to write a
# machine-local absolute path into the tracked record below.
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\amdgpu_wddm_conformance' }
if ($TestRefuseSignature -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-test-refuse" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Temporary files of the compiler stay in the workspace, never on the system drive.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Shaders: one header per program, the dxc identity recorded next to them in gen\dxc.txt. dxc is deterministic, so a
# rebuild with the same SDK writes the same bytes as the tracked headers.
$gen = Join-Path $here 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$programs = @(
    @{ Name = 'raster_vs';      Profile = 'vs_6_0';  Entry = 'vs_main'; File = 'raster.hlsl';   Defines = @() },
    @{ Name = 'rov_ps_ordered'; Profile = 'ps_6_0';  Entry = 'ps_fold'; File = 'raster.hlsl';   Defines = @('-D', 'ORDERED') },
    @{ Name = 'rov_ps_plain';   Profile = 'ps_6_0';  Entry = 'ps_fold'; File = 'raster.hlsl';   Defines = @() },
    @{ Name = 'cr_ps_tag';      Profile = 'ps_6_0';  Entry = 'ps_tag';  File = 'raster.hlsl';   Defines = @() },
    @{ Name = 'cr_ps_inner';    Profile = 'ps_6_0';  Entry = 'ps_inner'; File = 'raster.hlsl';  Defines = @() },
    @{ Name = 'dxr_lib';        Profile = 'lib_6_3'; Entry = '';        File = 'dxr.hlsl';      Defines = @() },
    @{ Name = 'dxr_args_cs';    Profile = 'cs_6_0';  Entry = 'main';    File = 'dxr_args.hlsl'; Defines = @() }
)
$dxcVersion = (& $dxc --version 2>&1 | Select-Object -First 1)
# The workspace root is written as <BC250_ROOT>, so the record does not depend on the caller's -Kits spelling.
$sanitized = $dxc.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
$dxcPath = if ($sanitized) { '<BC250_ROOT>' + $dxc.Substring($root.Length) } else { $dxc }
$log = @("dxc $dxcPath", "dxc version $dxcVersion", "dxc sha256 $((Get-FileHash -LiteralPath $dxc).Hash)")
foreach ($p in $programs) {
    $source = Join-Path $here "shaders\$($p.File)"
    $header = Join-Path $gen "$($p.Name).h"
    $arguments = @('-nologo', '-T', $p.Profile) + $(if ($p.Entry) { @('-E', $p.Entry) } else { @() }) + $p.Defines +
        @('-Vn', "g_$($p.Name)", '-Fh', $header, $source)
    & $dxc @arguments
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $($p.Name) ($LASTEXITCODE)" }
    # dxc writes CRLF. .gitattributes checks these headers out with LF, so the line endings are normalized
    # here. Without it every build leaves seven tracked files reported as modified with no content change.
    [IO.File]::WriteAllText($header, ([IO.File]::ReadAllText($header) -replace "`r`n", "`n"))
    $log += "dxc $($arguments[0..($arguments.Count - 4)] -join ' ') -Fh gen\$($p.Name).h shaders\$($p.File)"
}
# gen\dxc.txt is a tracked file, so the build writes it only when the record really changed, and only when
# the dxc path came out sanitized. A build whose dxc sits outside the workspace root keeps the tracked
# record and says so, instead of leaving an absolute toolchain path in the working tree for the next
# `git add -A` to publish.
$record = Join-Path $gen 'dxc.txt'
$current = if (Test-Path -LiteralPath $record) { @(Get-Content -LiteralPath $record) } else { @() }
if ($current.Count -eq $log.Count -and -not (Compare-Object $current $log -SyncWindow 0)) {
    Write-Host '  gen\dxc.txt unchanged'
} elseif ($sanitized) {
    $log | Set-Content -LiteralPath $record -Encoding ascii
    Write-Host '  gen\dxc.txt rewritten'
} else {
    Write-Warning ("dxc is outside the workspace root $root, so gen\dxc.txt keeps its tracked content. " +
        'Pass -Kits under the workspace, or set BC250_ROOT, to record the dxc identity.')
}

# A reviewed artifact is never lost to a rebuild: the existing binary stays under retained\ by its full hash.
$exe = Join-Path $Out 'amdgpu_wddm_conformance.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\amdgpu_wddm_conformance-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\amdgpu_wddm_conformance-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
$test = if ($TestRefuseSignature) { @('/DCONFORMANCE_TEST_REFUSE_SIGNATURE') } else { @() }
$clArgs = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    '/DINTERACTIVE_CONFORMANCE', '/DINTERACTIVE_FEATURE_LEVEL_12_1', "/I$here", "/I$queue") + $test + @(
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\amdgpu_wddm_conformance.obj",
    "/Fe$exe", (Join-Path $here 'main.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'user32.lib', 'psapi.lib', 'bcrypt.lib', 'kernel32.lib')
& $cl @clArgs | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --invalid
if ($LASTEXITCODE -ne 2) { throw 'Invalid CLI accepted' }
& $exe --selftest | Tee-Object -Variable selftest | Out-Null
if ($LASTEXITCODE -ne 0) { $selftest | Write-Host; throw 'selftest failed' }
Write-Host "  $($selftest | Select-Object -Last 1)"
Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
