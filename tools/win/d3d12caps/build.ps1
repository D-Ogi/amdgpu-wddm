param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\d3d12caps\build",
    [string]$KitVersion = '10.0.26100.0',
    # Export D3D12SDKVersion = <n> and D3D12SDKPath = .\D3D12_0\ so that d3d12.dll loads the Agility SDK core placed
    # next to the executable, as a game that ships one does. 0 builds the plain variant without those exports.
    [ValidateRange(0, 100000)][int]$AgilitySdkVersion = 0,
    # d3d12caps: the capability dump; d3d12allocprobe: the probe of textures the driver cannot size; d3d12oversub: the
    # over-commit probe of the memory manager (README.md).
    [ValidateSet('d3d12caps', 'd3d12allocprobe', 'd3d12oversub')][string]$Tool = 'd3d12caps',
    # x86: a 32-bit build, which a WoW64 process runs, so that the dump shows what the UserModeDriverNameWow D3D12
    # entry reports. Pass another -Out: the file name does not change.
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$name = if ($AgilitySdkVersion) { "amdgpu_wddm_${Tool}_agility$AgilitySdkVersion" } else { "amdgpu_wddm_$Tool" }

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out "$name.exe"
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\$name-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\$name-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
# /Brepro on cl and link: a content hash where they write a time. /FC with /d1trimfile: __FILE__ and the name that
# MSVC gives an anonymous namespace (a hash of the source path) see only the path below the repository. Two builds
# of one commit in two directories give the same bytes (docs/design/reproducible-builds.md).
$repro = @('/Brepro', '/FC', "/d1trimfile:$((Resolve-Path (Join-Path $here '..\..\..')).Path)")
$variant = @(); if ($AgilitySdkVersion) { $variant = @("/DCAPS_AGILITY_SDK_VERSION=$AgilitySdkVersion") }
# d3d12.dll and dxgi.dll are loaded at run time by name, so that an application-local runtime (the per-application
# route) is used exactly as the game would use it; neither import library is linked.
& $cl @repro @($variant + '/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++20', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\$name.obj",
    "/Fe$Out\$name.exe", (Join-Path $here "$Tool.cpp"), '/link', '/Brepro',
    "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")", "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'version.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$|^\s*Creating library|\.exp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# The Agility exports decide which D3D12 core the runtime loads: check them on the artifact itself.
$exports = & $dumpbin /nologo /exports "$Out\$name.exe" | Out-String
$has = $exports -match '\bD3D12SDKVersion\b' -and $exports -match '\bD3D12SDKPath\b'
if ($AgilitySdkVersion -and -not $has) { throw 'Agility variant lacks the D3D12SDKVersion/D3D12SDKPath exports' }
if (-not $AgilitySdkVersion -and $exports -match 'D3D12SDK') { throw 'plain variant exports D3D12SDK*' }
$imports = & $dumpbin /nologo /imports "$Out\$name.exe" | Out-String
if ($imports -match '(?im)^\s+(d3d12|dxgi)\.dll\s*$') { throw 'd3d12.dll or dxgi.dll is imported statically' }

& "$Out\$name.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& "$Out\$name.exe" not-a-number 2>$null
if ($LASTEXITCODE -ne 2) { throw 'invalid adapter index accepted' }
if ($Tool -eq 'd3d12allocprobe') {
    # The deadline: a main thread stuck in its first step, before any device, still leaves a document naming the
    # step, and exit code 3.
    $stalled = Join-Path $Out 'deadline-test.json'
    Remove-Item -LiteralPath $stalled -ErrorAction SilentlyContinue
    $env:D3D12ALLOCPROBE_TEST_STALL = 'dxgi'
    try { & "$Out\$name.exe" 0 $stalled 2>$null } finally { Remove-Item Env:D3D12ALLOCPROBE_TEST_STALL }
    if ($LASTEXITCODE -ne 3) { throw "deadline test: exit $LASTEXITCODE, expected 3" }
    $doc = Get-Content -LiteralPath $stalled -Raw | ConvertFrom-Json
    if (-not $doc.deadline.hit -or $doc.deadline.step -ne 'dxgi') { throw 'deadline test: the document lacks the step' }
}
if ($Tool -eq 'd3d12oversub') {
    # The same deadline test at the shortest deadline the probe takes, and the refusal of an option out of range.
    $stalled = Join-Path $Out 'deadline-test.json'
    Remove-Item -LiteralPath $stalled -ErrorAction SilentlyContinue
    $env:D3D12OVERSUB_TEST_STALL = 'dxgi'
    try { & "$Out\$name.exe" 0 $stalled --seconds 5 2>$null } finally { Remove-Item Env:D3D12OVERSUB_TEST_STALL }
    if ($LASTEXITCODE -ne 3) { throw "deadline test: exit $LASTEXITCODE, expected 3" }
    $doc = Get-Content -LiteralPath $stalled -Raw | ConvertFrom-Json
    if (-not $doc.deadline.hit -or $doc.deadline.step -ne 'dxgi') { throw 'deadline test: the document lacks the step' }
    & "$Out\$name.exe" 0 --seconds 171 2>$null
    if ($LASTEXITCODE -ne 2) { throw 'a deadline above 170 s accepted' }
}
Get-Item "$Out\$name.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

python -B -m unittest discover -s $here -p 'test_*.py'
if ($LASTEXITCODE -ne 0) { throw 'diff-caps tests failed' }
