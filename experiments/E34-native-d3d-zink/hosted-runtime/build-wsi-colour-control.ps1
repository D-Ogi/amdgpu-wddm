param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$OutDir='')
$ErrorActionPreference='Stop'
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$out=if($OutDir){$OutDir}else{"$Root\scratch\g0-hosted\wsi-colour-control"}
New-Item -ItemType Directory -Force $out | Out-Null
$stem='wsi-colour-control'
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++17 /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/I$Root\ref\Vulkan-Headers\include" "/Fo$out\" "/Fe$out\$stem.exe" "$PSScriptRoot\$stem.cpp" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" user32.lib
if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}
& "$out\$stem.exe" --help
if($LASTEXITCODE -ne 0){throw 'Probe help check failed'}
