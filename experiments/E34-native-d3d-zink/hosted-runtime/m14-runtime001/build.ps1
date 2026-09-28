param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path }),[string]$OutDir='')
$ErrorActionPreference='Stop'
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$out=if($OutDir){$OutDir}else{"$Root\scratch\g0-hosted\m14-runtime001-build"}
New-Item -ItemType Directory -Force $out | Out-Null
$stem='router'
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /LD /EHsc /std:c++17 /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\winrt" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$out\" "/Fe$out\$stem.dll" "$PSScriptRoot\$stem.cpp" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" user32.lib d3d11.lib dxgi.lib
if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}

& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++17 /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$out\" "/Fe$out\selection-test.exe" "$PSScriptRoot\selection-test.cpp" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" kernel32.lib
if($LASTEXITCODE -ne 0){throw 'Selection test build failed'}
& "$out\selection-test.exe"
if($LASTEXITCODE -ne 0){throw 'Selection policy test failed'}
(Get-FileHash "$out\router.dll").Hash

& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++17 /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$out\" "/Fe$out\debug-child.exe" "$PSScriptRoot\debug-child.cpp" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" kernel32.lib
if($LASTEXITCODE -ne 0){throw 'Debug child build failed'}
foreach($case in @(@{mode='ok';seconds=5;expected=0},@{mode='fail';seconds=5;expected=7},@{mode='sleep';seconds=1;expected=124},@{mode='tree';seconds=5;expected=0},@{mode='crash';seconds=5;expected=-1073741819})){
 $text=(& "$out\debug-child.exe" $case.seconds "$out\debug-child.exe" --fixture $case.mode | Out-String)
 if($LASTEXITCODE -ne $case.expected -or $text -notmatch 'DEBUG M14 DEBUG STRING CONTROL' -or $text -notmatch 'DEBUG M14 WIDE STRING CONTROL' -or $text -notmatch 'tree_closed=1' -or $text -notmatch 'DEBUG M14 PAGE EDGE CONTROL'){throw "Debug capture control failed: $text"}
 if($case.mode -eq 'crash' -and ($text -notmatch 'CONTEXT rip=' -or $text -notmatch 'STACK [0-9a-f]+ [0-9a-f]+' -or $text -notmatch 'MODULE base=.+path=')){throw "Crash context capture failed: $text"}
 $text|Set-Content "$out\debug-$($case.mode).txt"
}
'PASS debug capture: ANSI/Unicode, successful/failed child, timeout'
