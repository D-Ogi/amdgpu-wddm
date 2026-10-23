param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    # Host test build only (its own output directory): the mesh shader tier reads as 0 on every adapter, so the skip
    # path and exit code 4 can be checked where no tier-0 adapter is at hand. Never a lab artifact.
    [switch]$TestForceTier0
)
# Builds amdgpu_wddm_mesh.exe, the D3D12 mesh shader witness: the shaders with the SDK's dxc into gen\mesh_*.h, then
# one cl invocation, then the CPU-only gates (--help 0, --invalid 2, --selftest 0). The previous exe stays under
# retained\ by its hash. This client is standalone: it takes no interactive protocol and no conformance.h.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"
if (-not (Test-Path -LiteralPath $dxc)) { throw "dxc not found: $dxc" }

# The workspace root: BC250_ROOT when it is set, else the grandparent of -Kits, which is
# <workspace>\toolchain\nuget. It is taken from -Kits and never from this script's own place, because a build in a
# git worktree sits under <workspace>\scratch\... and the parent of the checkout is then not the workspace.
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\amdgpu_wddm_mesh' }
if ($TestForceTier0 -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-test-tier0" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Temporary files of the compiler stay in the workspace, never on the system drive.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Shaders: one header per program, the dxc identity recorded next to them in gen\dxc-mesh.txt. dxc is
# deterministic, so a rebuild with the same SDK writes the same bytes as the tracked headers. Every program is
# shader model 6.5: that is the model a mesh or an amplification program needs.
$gen = Join-Path $here 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$programs = @(
    @{ Name = 'mesh_vs';         Profile = 'vs_6_5'; Entry = 'vs_main'; Defines = @() },
    @{ Name = 'mesh_ps';         Profile = 'ps_6_5'; Entry = 'ps_main'; Defines = @() },
    @{ Name = 'mesh_ms';         Profile = 'ms_6_5'; Entry = 'ms_main'; Defines = @() },
    @{ Name = 'mesh_ms_payload'; Profile = 'ms_6_5'; Entry = 'ms_main'; Defines = @('-D', 'WITNESS_PAYLOAD') },
    @{ Name = 'mesh_as';         Profile = 'as_6_5'; Entry = 'as_main'; Defines = @() },
    # --probe: the same programs, which also count their work in the root UAV u0 (shaders\mesh.hlsl).
    @{ Name = 'mesh_ps_probe';         Profile = 'ps_6_5'; Entry = 'ps_main'; Defines = @('-D', 'WITNESS_PROBE') },
    @{ Name = 'mesh_ms_probe';         Profile = 'ms_6_5'; Entry = 'ms_main'; Defines = @('-D', 'WITNESS_PROBE') },
    @{ Name = 'mesh_ms_payload_probe'; Profile = 'ms_6_5'; Entry = 'ms_main';
       Defines = @('-D', 'WITNESS_PAYLOAD', '-D', 'WITNESS_PROBE') },
    @{ Name = 'mesh_as_probe';         Profile = 'as_6_5'; Entry = 'as_main'; Defines = @('-D', 'WITNESS_PROBE') }
)
$dxcVersion = (& $dxc --version 2>&1 | Select-Object -First 1)
# The workspace root is written as <BC250_ROOT>, so the record does not depend on the caller's -Kits spelling.
$sanitized = $dxc.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
$dxcPath = if ($sanitized) { '<BC250_ROOT>' + $dxc.Substring($root.Length) } else { $dxc }
$log = @("dxc $dxcPath", "dxc version $dxcVersion", "dxc sha256 $((Get-FileHash -LiteralPath $dxc).Hash)")
$source = Join-Path $here 'shaders\mesh.hlsl'
foreach ($p in $programs) {
    $header = Join-Path $gen "$($p.Name).h"
    $arguments = @('-nologo', '-T', $p.Profile, '-E', $p.Entry) + $p.Defines +
        @('-Vn', "g_$($p.Name)", '-Fh', $header, $source)
    & $dxc @arguments
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $($p.Name) ($LASTEXITCODE)" }
    # dxc writes CRLF. .gitattributes checks these headers out with LF, so the line endings are normalized here.
    [IO.File]::WriteAllText($header, ([IO.File]::ReadAllText($header) -replace "`r`n", "`n"))
    $log += "dxc $($arguments[0..($arguments.Count - 4)] -join ' ') -Fh gen\$($p.Name).h shaders\mesh.hlsl"
}
# gen\dxc-mesh.txt is a tracked file, so the build writes it only when the record really changed, and only when the
# dxc path came out sanitized. A build whose dxc sits outside the workspace root keeps the tracked record and says
# so, instead of leaving an absolute toolchain path in the working tree.
$record = Join-Path $gen 'dxc-mesh.txt'
$current = if (Test-Path -LiteralPath $record) { @(Get-Content -LiteralPath $record) } else { @() }
if ($current.Count -eq $log.Count -and -not (Compare-Object $current $log -SyncWindow 0)) {
    Write-Host '  gen\dxc-mesh.txt unchanged'
} elseif ($sanitized) {
    $log | Set-Content -LiteralPath $record -Encoding ascii
    Write-Host '  gen\dxc-mesh.txt rewritten'
} else {
    Write-Warning ("dxc is outside the workspace root $root, so gen\dxc-mesh.txt keeps its tracked content. " +
        'Pass -Kits under the workspace, or set BC250_ROOT, to record the dxc identity.')
}

# A reviewed artifact is never lost to a rebuild: the existing binary stays under retained\ by its full hash.
$exe = Join-Path $Out 'amdgpu_wddm_mesh.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\amdgpu_wddm_mesh-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\amdgpu_wddm_mesh-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
$test = if ($TestForceTier0) { @('/DMESH_TEST_FORCE_TIER_0') } else { @() }
$clArgs = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE', "/I$here") + $test + @(
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\amdgpu_wddm_mesh.obj",
    "/Fe$exe", (Join-Path $here 'mesh.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'user32.lib', 'kernel32.lib')
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
