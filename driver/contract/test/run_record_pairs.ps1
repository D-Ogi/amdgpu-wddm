# Builds and runs the record pair test (driver\contract\test\record-pair-test.cpp): every producer of a shared-surface
# record against every consumer that reads it, over sizes, formats and bind flags. Then tools\quality\record_pairs.py
# compares the result with the expected-red list of driver\contract\record-pairs.json. Host only: no lab machine.
#
#   pwsh driver\contract\test\run_record_pairs.ps1 -Out P:\BC-250\scratch\record-pairs
#   pwsh driver\contract\test\run_record_pairs.ps1 -Arch x86
#
# Everything is written under -Out, never into the repository and never onto drive C:. The compiler is the one of the
# user-mode host tests (clang-cl, tools\build\umd-compiler.ps1).

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\record-pairs",
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$env:BC250_ROOT = $Root
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$Out = Join-Path $Out $Arch
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = [IO.Path]::GetFullPath($Out)

# BD-412: the D3D11 shell imports a runtime surface by its mapped range. The test models the import that the tree
# makes; record_pairs.py checks that the text below is still the name of one of the two forms.
$imports = Get-Content -LiteralPath (Join-Path $repo 'driver\umd\dxvk\runtime-surface.cpp') -Raw
$defines = @()
if ($imports.Contains('imported.size=surface.mapping.bytes')) { $defines += '/DRECORD_PAIR_IMPORT_MAPPED' }

$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $Out; $env:TMP = $Out
    $null = Import-VsDevEnvironment -TempDir $Out -Arch $Arch
    $cc = Resolve-Bc250UmdCompiler $Root 'clang-cl' '' $Out $Arch
    $flags = Get-Bc250HostTestFlags 'clang-cl' @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/O2') @(
        "$repo\driver\contract\third_party", "$repo\driver\contract\uapi-shim") $Arch @($repo)
    Push-Location $Out
    try {
        Write-Host "build ($Arch)"
        Invoke-Bc250UmdCompiler $cc @flags @defines "/I$repo" /Fe:record-pair-test.exe (Join-Path $here 'record-pair-test.cpp') "/Tp$repo\driver\kmd\dcn_translate.c"
        if ($LASTEXITCODE -ne 0) { throw 'record-pair-test build failed' }
        Write-Host 'run'
        & .\record-pair-test.exe | Set-Content -LiteralPath (Join-Path $Out 'pairs.tsv') -Encoding utf8
        Write-Host "record-pair-test.exe exit code $LASTEXITCODE"
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

& python -B (Join-Path $repo 'tools\quality\record_pairs.py') --root $repo --results (Join-Path $Out 'pairs.tsv')
exit $LASTEXITCODE
