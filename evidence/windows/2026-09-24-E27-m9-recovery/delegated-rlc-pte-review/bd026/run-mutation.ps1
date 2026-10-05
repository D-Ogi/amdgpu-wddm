$ErrorActionPreference='Stop'
$env:TEMP='P:\bc-250\scratch\tmp';$env:TMP=$env:TEMP
$base='P:\bc-250'
$out=Join-Path $base 'scratch\build\bd026'
$work=Join-Path $base 'scratch\m9\bd026'
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin=Join-Path $msvc.FullName 'bin\Hostx64\x64'
$sdk=Join-Path $base 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$sdklib=Join-Path $base 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
$repo=Join-Path $base 'bc250-win'
$env:INCLUDE='';$env:LIB=''
& "$bin\cl.exe" /nologo /c /TC /W4 /WX /Od /Zi /D_CRT_SECURE_NO_WARNINGS /wd4201 /wd4214 "/I$repo\driver\shim\include" "/I$repo\driver\amdgpu-import" "/I$repo\third_party\linux-amdgpu" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/I$($msvc.FullName)\include" "/Fo$out\no-rlc-check.obj" "/Fd$out\no-rlc-check.pdb" "$work\bc250_psp-no-rlc-check.c"
if ($LASTEXITCODE -ne 0) { throw 'mutation compile failed' }
$objs=@((Get-ChildItem "$out\obj-user\*.obj" | Where-Object Name -ne 'bc250_psp.obj').FullName)+@("$out\no-rlc-check.obj")
& "$bin\link.exe" /nologo /DEBUG /MACHINE:X64 /SUBSYSTEM:CONSOLE "/LIBPATH:$sdklib\ucrt\x64" "/LIBPATH:$sdklib\um\x64" "/LIBPATH:$($msvc.FullName)\lib\x64" "/OUT:$out\replay_psp-no-rlc-check.exe" "/PDB:$out\replay_psp-no-rlc-check.pdb" @objs
if ($LASTEXITCODE -ne 0) { throw 'mutation link failed' }
$evid=Join-Path $repo 'evidence\linux\2026-09-21-E03-init-trace'
& "$out\replay_psp-no-rlc-check.exe" "$evid\sweep-before-run1-GC-complete-then-hang.log" "$evid\sweep-before-run2-nonGC.log" "$out\trace-psp.txt" "$base\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu"
if ($LASTEXITCODE -ne 1) { throw "Expected mutation failure, got $LASTEXITCODE" }
Write-Host 'Expected mutation failure observed.'
