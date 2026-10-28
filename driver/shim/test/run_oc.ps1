# Builds and runs the overclock tier host test (oc_test.c): the per-tier envelopes, the stored tier against the board
# it was chosen on, and the board fingerprint. Also compile-checks bc250_oc.c with the kernel flags. Host-side only:
# nothing here touches the lab machine. Everything is written under -Out, never into the repository.
#
#   pwsh driver\shim\test\run_oc.ps1 -Root P:\BC-250
param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\build\oc-policy')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path # the tree this script lives in; -Root only locates the toolchain
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$wdk="$Root\toolchain\nuget\microsoft.windows.wdk.x64\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp";$env:TMP=$env:TEMP
$inc=@("/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import")
$sources=@("$repo\driver\shim\bc250_oc.c","$repo\driver\shim\bc250_clock.c","$PSScriptRoot\oc_test.c")
& $cl /nologo /TC /W4 /WX /O2 /MT @inc "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\oc_test.exe" @sources /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& $cl /nologo /c /TC /kernel /GS- /Zp8 /W4 /WX /O2 /D_AMD64_ /DAMD64 /D_WIN64 @inc "/I$wdk\Include\10.0.26100.0\km\crt" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\oc-kernel.obj" "$repo\driver\shim\bc250_oc.c"
if($LASTEXITCODE -ne 0){throw 'Kernel compile failed'}
& "$Out\oc_test.exe"
exit $LASTEXITCODE
