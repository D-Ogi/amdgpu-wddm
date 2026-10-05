param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('shadowtest', 'presenttest', 'both')][string]$Harness = 'both'
)
# Builds the two M16 WSI harnesses, shadowtest.exe and presenttest.exe. Both are console processes with no
# window and nothing resident. Neither one touches the lab: they run on the development PC.
# The flags are the ones that produced the E56 binaries. The source hashes this prints are the ones E56
# records. The binary hash depends on the compiler and on the SDK in use, so it is a property of the run and
# not of the source: each E56 manifest names the compiler of its own run.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
if (-not (Test-Path -LiteralPath (Join-Path $sdk "Include\$KitVersion\um\dcomp.h"))) {
    throw "SDK $KitVersion not found under $sdk"
}

# The workspace root: BC250_ROOT when it is set, else the grandparent of -Kits, which is
# <workspace>\toolchain\nuget. It is never guessed from this script's own place, because a build from a git
# worktree sits under <workspace>\scratch\... and the parent of the checkout is then not the workspace.
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\wsi-dxgi' }
New-Item -ItemType Directory -Force $Out | Out-Null
# Temporary files of the compiler stay in the workspace, never on the system drive.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = ''; $env:LIB = ''

$names = if ($Harness -eq 'both') { @('shadowtest', 'presenttest') } else { @($Harness) }
foreach ($name in $names) {
    $source = Join-Path $here "$name.cpp"
    $exe = Join-Path $Out "$name.exe"
    $clArgs = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/O2', '/DUNICODE', '/D_UNICODE',
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt",
        "/Fo$Out\$name.obj", "/Fe$exe", $source, '/link',
        "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
        'psapi.lib')
    & $cl @clArgs | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $name ($LASTEXITCODE)" }
    Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
    '{0,9}  {1}.cpp  sha256 {2}' -f (Get-Item $source).Length, $name, (Get-FileHash -LiteralPath $source).Hash
}
