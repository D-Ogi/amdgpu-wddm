# Host test of DxgkDdiOpenAllocation (F-q44-mem-3): the actual Bc250WddmOpenAllocation and object list code of
# wddm.c, extracted by generate_open_allocation_test.py, with the pool allocation failed at the first, a middle and
# the last allocation of a call (open_allocation_test.c). Built and run with no WDK header.
#
#   pwsh driver\kmd\test\run_open_allocation.ps1 -Out P:\BC-250\scratch\open-allocation
#   pwsh driver\kmd\test\run_open_allocation.ps1 -Mutation --keep-earlier -ExpectFailure
#
# The negative controls each change the extracted copy, never the tree, and must fail by a CHECK (a FAIL line), not
# by a build error:
#   --success-null    an admitted blob with no object answers STATUS_SUCCESS with a NULL handle (before F-q44-mem-3)
#   --keep-earlier    the objects opened earlier in the failed call are not freed
#   --count-at-once   the LB7A open counts go straight into StdAlloc, the failed allocation among them as refused
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\open-allocation",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', '--success-null', '--keep-earlier', '--count-at-once')][string]$Mutation = '',
    [switch]$ExpectFailure
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path (Split-Path $kmd)
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
$env:INCLUDE = ''
$env:LIB = ''

$gen = @((Join-Path $here 'generate_open_allocation_test.py'), $repo, (Join-Path $Out 'open_allocation_actual.inc'))
if ($Mutation) { $gen += $Mutation }
# -E, so that PYTHONOPTIMIZE from the environment cannot strip the generator's anchor guards.
& python -E @gen
if ($LASTEXITCODE -ne 0) { throw 'open allocation extraction failed' }

$incUser = @("/I$Out", "/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, no WDK header)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\open_allocation_test.exe") +
    @((Join-Path $kmd 'dcn_translate.c'), (Join-Path $kmd 'umd_blob.c'), (Join-Path $here 'open_allocation_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
$log = Join-Path $Out 'run.log'
& "$Out\open_allocation_test.exe" 2>&1 | Tee-Object -FilePath $log
$code = $LASTEXITCODE
if ($ExpectFailure) {
    if ($code -eq 0) { throw "negative control $Mutation unexpectedly passed" }
    if (!(Select-String -Path $log -Pattern 'FAIL' -CaseSensitive -Quiet)) { throw "negative control $Mutation failed without a FAIL check, exit $code" }
    Write-Host "Expected negative control $Mutation failed"
    exit 0
}
Write-Host ''
Write-Host "open_allocation_test.exe exit code $code"
exit $code
