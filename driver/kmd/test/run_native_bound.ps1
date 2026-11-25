# Builds and runs the host test of the bound of a native virtual fill or transfer (native_bound_test.c): wddm.c's
# own WddmNativeAllocation, extracted, with the production size rule and range check (paging_window.c) linked in.
# Host only.
#
#   pwsh driver\kmd\test\run_native_bound.ps1 -Out <dir>
#   pwsh driver\kmd\test\run_native_bound.ps1 -Out <dir> -Mutation blob-size     (negative control: must FAIL)
param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
      [string]$Out="$Root\scratch\native-bound",[string]$Source='',[string]$Kits="$Root\toolchain\nuget",
      [ValidateSet('none','blob-size')][string]$Mutation='none')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Kits\microsoft.windows.sdk.cpp\c"
$libs="$Kits\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
$env:INCLUDE=''; $env:LIB=''
& python "$PSScriptRoot\generate_native_bound_test.py" --mutation $Mutation --source $Source --out "$Out\native_bound_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$Source" `
    "/I$sdk\Include\10.0.26100.0\ucrt" "/I$($msvc.FullName)\include" `
    "/Fo$Out\" "/Fe$Out\native_bound_test.exe" "$Out\native_bound_actual.c" "$Source\paging_window.c" `
    /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\native_bound_test.exe"
