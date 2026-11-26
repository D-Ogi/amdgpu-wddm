# Builds and runs the host test of the 2 MiB VRAM layout and the 64 KiB host pages on one start
# (large_pages_combined_test.c): the production text of vidmm.c, wddm.c and vram.c, extracted, with the production
# shim translation, logical table state, 64 KiB size rule and placement rule linked in. Host only.
#
#   pwsh driver\kmd\test\run_large_pages_combined.ps1 -Out <dir>
#   pwsh driver\kmd\test\run_large_pages_combined.ps1 -Out <dir> -Mutation sizes-drop-2mib   (negative control: must FAIL)
param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
      [string]$Out="$Root\scratch\large-pages-combined",[string]$Source='',[string]$Kits="$Root\toolchain\nuget",
      [ValidateSet('none','sizes-drop-2mib')][string]$Mutation='none')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Kits\microsoft.windows.sdk.cpp\c"
$libs="$Kits\microsoft.windows.sdk.cpp.x64\c"
$shim="$repo\driver\shim"
$contract="$repo\driver\contract"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
$env:INCLUDE=''; $env:LIB=''
& python "$PSScriptRoot\generate_large_pages_combined_test.py" --mutation $Mutation --source $Source --out "$Out\large_pages_combined_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$Source" "/I$shim\include" "/I$shim" "/I$repo\driver\amdgpu-import" "/I$repo\third_party\linux-amdgpu" `
    "/I$contract" "/I$contract\third_party" "/I$contract\uapi-shim" `
    "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/I$($msvc.FullName)\include" `
    "/Fo$Out\" "/Fe$Out\large_pages_combined_test.exe" "$Out\large_pages_combined_actual.c" "$shim\bc250_pte.c" "$shim\bc250_gart.c" `
    "$Source\paging_pt_shadow.c" "$Source\paging_permutation.c" "$Source\paging_aperture_state.c" "$Source\paging_window.c" "$Source\umd_blob.c" `
    /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\large_pages_combined_test.exe"
