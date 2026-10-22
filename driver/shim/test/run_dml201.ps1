param([Parameter(Mandatory=$true)][string]$Out,
      [Parameter(Mandatory=$true)][string]$Kits)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP=$Out;$env:TMP=$Out
& python -B "$repo\tools\import\extract_dml201.py" --check
if($LASTEXITCODE){throw 'DML import check failed'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl=Join-Path (Split-Path $Kits -Parent) 'llvm-23.1.4-win64\bin\clang-cl.exe'
& $cl --version
if($LASTEXITCODE){throw 'Explicit clang-cl unavailable'}
$sdk="$Kits\microsoft.windows.sdk.cpp\c\Include\10.0.26100.0"
$lib="$Kits\microsoft.windows.sdk.cpp.x64\c"
$argsList=@('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise','-fuse-ld=lld',
    "/I$($msvc.FullName)\include","/I$sdk\ucrt","/I$repo\driver\shim\dml201",
    "/Fo$Out\","/Fe$Out\dml201_test.exe","$PSScriptRoot\dml201_test.c","$repo\driver\shim\bc250_dml201.c",
    '/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\ucrt\x64","/LIBPATH:$lib\um\x64")
& $cl @argsList
if($LASTEXITCODE){throw 'DML host compilation failed'}
& "$Out\dml201_test.exe"
if($LASTEXITCODE){throw 'DML host assertions failed'}
$mpoArgs=@('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise','-fuse-ld=lld',
    "/I$($msvc.FullName)\include","/I$sdk\ucrt","/I$repo\driver\shim\dml201",
    "/Fo$Out\dml201_mpo_test.obj","/Fe$Out\dml201_mpo_test.exe","$PSScriptRoot\dml201_mpo_test.c",
    '/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\ucrt\x64","/LIBPATH:$lib\um\x64")
& $cl @mpoArgs
if($LASTEXITCODE){throw 'DML MPO host compilation failed'}
& "$Out\dml201_mpo_test.exe"
if($LASTEXITCODE){throw 'DML MPO host assertions failed'}
$groupArgs=@('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise','-fuse-ld=lld',
    '/DBC250_SCALER_GEOMETRY_TEST',
    "/I$($msvc.FullName)\include","/I$sdk\ucrt","/I$repo\driver\shim\dml201",
    "/Fo$Out\","/Fe$Out\dml201_group_test.exe","$PSScriptRoot\dml201_group_test.c",
    "$repo\driver\shim\bc250_dml201.c","$repo\driver\shim\bc250_scaler_plan.c",
    "$repo\driver\shim\bc250_scaler_geometry.c","$repo\driver\shim\bc250_scaler_buffer.c",
    "$repo\driver\shim\bc250_scaler_filter.c",
    '/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\ucrt\x64","/LIBPATH:$lib\um\x64")
& $cl @groupArgs
if($LASTEXITCODE){throw 'DML group host compilation failed'}
& "$Out\dml201_group_test.exe"
if($LASTEXITCODE){throw 'DML group host assertions failed'}
$nv12Args=@('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise','-fuse-ld=lld',
    '/DBC250_SCALER_GEOMETRY_TEST',
    "/I$($msvc.FullName)\include","/I$sdk\ucrt","/I$repo\driver\shim\dml201",
    "/Fo$Out\","/Fe$Out\dml201_nv12_test.exe","$PSScriptRoot\dml201_nv12_test.c",
    "$repo\driver\shim\bc250_scaler_plan.c","$repo\driver\shim\bc250_scaler_geometry.c",
    "$repo\driver\shim\bc250_scaler_buffer.c","$repo\driver\shim\bc250_scaler_filter.c",
    '/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\ucrt\x64","/LIBPATH:$lib\um\x64")
& $cl @nv12Args
if($LASTEXITCODE){throw 'DML NV12 host compilation failed'}
& "$Out\dml201_nv12_test.exe"
if($LASTEXITCODE){throw 'DML NV12 host assertions failed'}
