# Builds and runs the host test of the 64 KiB page table code of vidmm.c (vidmm_64k_test.c): the production
# functions, extracted, with the production shim translation linked in. Host only.
#
#   pwsh driver\kmd\test\run_vidmm_64k.ps1 -Out <dir>
#   pwsh driver\kmd\test\run_vidmm_64k.ps1 -Out <dir> -Mutation one-slot     (negative control: must FAIL)
param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
      [string]$Out="$Root\scratch\vidmm-64k",[string]$Source='',[string]$Kits="$Root\toolchain\nuget",
      [ValidateSet('none','one-slot')][string]$Mutation='none')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Kits\microsoft.windows.sdk.cpp\c"
$libs="$Kits\microsoft.windows.sdk.cpp.x64\c"
$shim="$repo\driver\shim"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
$env:INCLUDE=''; $env:LIB=''
& python "$PSScriptRoot\generate_vidmm_64k_test.py" --mutation $Mutation --source $Source --out "$Out\vidmm_64k_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$shim\include" "/I$shim" "/I$repo\driver\amdgpu-import" "/I$repo\third_party\linux-amdgpu" `
    "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/I$($msvc.FullName)\include" `
    "/Fo$Out\" "/Fe$Out\vidmm_64k_test.exe" "$Out\vidmm_64k_actual.c" "$shim\bc250_pte.c" "$shim\bc250_gart.c" `
    /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\vidmm_64k_test.exe"
