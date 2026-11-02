# Builds the fake driver of the kmd-reader host tests (fake_driver.c): fake_cli.exe and a fake bc250control.dll, both
# from the unchanged bc250kmd_cli.c, the compiler and the flags of the release CLI (tools\build\umd-compiler.ps1). No
# adapter is opened and no escape reaches a driver. Prints the output directory. Throws on a failure.
#   pwsh -File tools\win\kmdread\test\build-fake.ps1 -Kits P:\BC-250\toolchain\nuget -Out <dir>
param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = '',
    [string]$VsInstall = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$Kits = [IO.Path]::GetFullPath($Kits)
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$saved = @{ TEMP = $env:TEMP; TMP = $env:TMP; INCLUDE = $env:INCLUDE; LIB = $env:LIB; PATH = $env:PATH; CL = $env:CL; _CL_ = $env:_CL_ }
try {
    # The directory must exist: cl given a TEMP that does not reports D8018 or D8050 and names no path at all.
    $env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
    New-Item -ItemType Directory -Force $Out, $env:TEMP, "$Out\exe", "$Out\dll" | Out-Null
    $tool = Set-Bc250ToolEnvironment $Kits 'x64' $KitVersion $VsInstall
    $cc = Resolve-Bc250UmdCompiler (Split-Path (Split-Path $Kits -Parent) -Parent) $Compiler $MsvcReason $Out 'x64' $tool.Cl
    if ($Compiler -eq 'clang-cl') { $env:CL = $null; $env:_CL_ = $null }
    $stateFlags = Get-Bc250ToolFlags $Compiler @('/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS') 'x64' @($repo)
    $libs = @('gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib')
    $flags = Get-Bc250ToolFlags $Compiler @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS') 'x64' @($repo)
    $source = Join-Path $here 'fake_driver.c'

    $stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cc -Out "$Out\state" -Flags $stateFlags)
    Invoke-Bc250UmdCompiler $cc @flags @("/Fo$Out\exe\", "/Fe$Out\fake_cli.exe", $source) @stateObjects @('/link') @libs |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$Compiler failed on fake_driver.c as the CLI ($LASTEXITCODE)" }

    # The release DLL is bc250kmd_cli.c with BC250_CONTROL_DLL and the exporting state objects (bc250kmd_cli\build.ps1).
    $exportObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cc -Out "$Out\state-exports" -Flags $stateFlags -Exports)
    Invoke-Bc250UmdCompiler $cc @flags @('/LD', '/DBC250_CONTROL_DLL', "/Fo$Out\dll\", "/Fe$Out\bc250control.dll", $source) `
        @exportObjects @('/link') @libs |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$Compiler failed on fake_driver.c as the control DLL ($LASTEXITCODE)" }
    foreach ($product in 'fake_cli.exe', 'bc250control.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $Out $product))) { throw "$product was not built" }
    }
    $Out
} finally {
    foreach ($name in $saved.Keys) { Set-Item "env:$name" $saved[$name] }
}
