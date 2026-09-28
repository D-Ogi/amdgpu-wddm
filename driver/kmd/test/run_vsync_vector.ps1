param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\vsync-vector",[switch]$DropVectorDispatch,[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out|Out-Null
$text=Get-Content "$repo\driver\kmd\dcn.c" -Raw
$a=$text.IndexOf('BOOLEAN DcnVsyncInterrupt(');$b=$text.IndexOf('// ---- the present path',$a)
if($a -lt 0 -or $b -le $a){throw 'DCN extraction failed'}
[IO.File]::WriteAllText("$Out\vsync_vector_actual.inc",$text.Substring($a,$b-$a))
$text=Get-Content "$repo\driver\kmd\pnp.c" -Raw
$a=$text.IndexOf('void Bc250DpcRoutine(');$b=$text.IndexOf('NTSTATUS Bc250QueryChildRelations(',$a)
if($a -lt 0 -or $b -le $a){throw 'DPC extraction failed'}
$code=$text.Substring($a,$b-$a)
if($DropVectorDispatch){$code=$code.Replace('DcnVsyncFromVector((BC250_DEVICE*)MiniportDeviceContext);','(void)0;')}
[IO.File]::WriteAllText("$Out\vsync_dpc_actual.inc",$code)
Copy-Item "$PSScriptRoot\vsync_vector_test.c" "$Out\test.c" -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\vsync_vector_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'IH consumer host build failed'}
& "$Out\vsync_vector_test.exe"
if($ExpectFailure){if($LASTEXITCODE -eq 0){throw "negative control unexpectedly passed"};Write-Host "Expected negative control failed";exit 0};exit $LASTEXITCODE
