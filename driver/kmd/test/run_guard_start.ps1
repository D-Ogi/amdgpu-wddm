param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\guard144",[switch]$IgnoreDurability)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content (Join-Path $repo 'driver\kmd\guard.c') -Raw
$first=$source.IndexOf('ULONG GuardConsumeSetting(');$last=$source.IndexOf('void GuardLog(',$first)
$code=$source.Substring($first,$last-$first)
if($IgnoreDurability){$code=$code.Replace('return RequireDurable ? status : STATUS_SUCCESS;', 'return STATUS_SUCCESS;').Replace('if (RequireDurable) { ZwClose(key); return status; }','if (RequireDurable) { /* old ignored error */ }').Replace('if (RequireDurable) return status;','if (RequireDurable) { /* old ignored error */ }')}
[IO.File]::WriteAllText((Join-Path $Out 'guard_actual.inc'),$code)
# BD-090: the give-back after an orderly stop in a confirmed boot, to the end of guard.c.
$first=$source.IndexOf('// ---- BD-090:');if($first -lt 0){throw 'guard.c has no BD-090 section'}
[IO.File]::WriteAllText((Join-Path $Out 'release_actual.inc'),$source.Substring($first))
$source=Get-Content (Join-Path $repo 'driver\kmd\wddm.c') -Raw
$first=$source.IndexOf('BOOLEAN WddmGateOpen(');$last=$source.IndexOf('// ---- the log ----',$first)
[IO.File]::WriteAllText((Join-Path $Out 'gate_actual.inc'),$source.Substring($first,$last-$first))
$source=Get-Content (Join-Path $repo 'driver\kmd\pnp.c') -Raw
$first=$source.IndexOf('NTSTATUS Bc250StartDevice(');$last=$source.IndexOf('    device->StartInfo =',$first)
[IO.File]::WriteAllText((Join-Path $Out 'pnp_actual.inc'),$source.Substring($first,$last-$first)+"    (void)DxgkStartInfo; (void)DxgkInterface;`n    return STATUS_SUCCESS;`n}`n")
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'guard_start_test.c') -Destination (Join-Path $Out 'test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\guard_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'guard host build failed'}
& "$Out\guard_test.exe"
exit $LASTEXITCODE
