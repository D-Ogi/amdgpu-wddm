# Gate: the raw diagnostic verbs reach their handlers in a lab build and no handler in a release build (BD-167,
# review r1 R4). Builds verbs_test.c in the flavour asked for (-Lab defines AMDGPU_WDDM_LAB_FLAVOUR, as the two CLI
# build scripts do for the tool) and runs it. The test replaces SetupAPI and the D3DKMT calls with fakes: no adapter is
# opened and no escape reaches a driver, so no lab command line runs against the PC that builds. Throws on a failure.
# Both CLI build scripts call it before they compile the CLI. The name has no "patch" in it (dispatch): Windows starts
# an x86 program without a manifest whose name holds "patch", "setup" or "install" only elevated (installer detection).
#   pwsh -File tools\win\bc250kmd_cli\test-verbs.ps1 -Kits P:\BC-250\toolchain\nuget -Out <dir> [-Arch x86] [-Lab]
param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [switch]$Lab
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
$labDefine = @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' })
$includes = @("/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared")
# The build scripts call this file in their own process, so the variables it changes are put back at the end.
$saved = @{ TEMP = $env:TEMP; TMP = $env:TMP; INCLUDE = $env:INCLUDE; LIB = $env:LIB }
try {
    # The directory must exist: cl given a TEMP that does not reports D8018 or D8050 and names no path at all.
    $env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
    New-Item -ItemType Directory -Force $Out, $env:TEMP | Out-Null
    $env:INCLUDE = ''; $env:LIB = ''
    $stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cl -Out "$Out\state" `
        -Flags (@('/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS') + $includes))
    & $cl @labDefine @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS') @includes @("/Fo$Out\verbs_test.obj",
        "/Fe$Out\verbs_test.exe", (Join-Path $here 'verbs_test.c')) @stateObjects @('/link',
        "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")", "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
        'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib') |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed on verbs_test.c ($LASTEXITCODE)" }
    & "$Out\verbs_test.exe" 2>&1 | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "verbs_test.c failed ($(if ($Lab) { 'lab' } else { 'release' }) flavour)" }
} finally {
    foreach ($name in $saved.Keys) { Set-Item "env:$name" $saved[$name] }
}
