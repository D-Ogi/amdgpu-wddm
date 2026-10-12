param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path
$env:TEMP=$Out;$env:TMP=$Out
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\identity.exe" "$repo\tools\build\board_identity_qualification_test.c" "$repo\driver\kmd\board_identity.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Identity qualification host compile failed'}
$lines=@(& "$Out/identity.exe");$code=$LASTEXITCODE
$lines | Tee-Object "$Out/test.log"
$hashes=@{}
foreach($path in @('driver/kmd/board_identity.c','driver/kmd/board_identity.h','tools/build/board_identity_qualification_test.c')) {
 $hashes[$path]=(Get-FileHash (Join-Path $repo $path)).Hash
}
@{Sources=$hashes;ExitCode=$code;Output=$lines} | ConvertTo-Json -Depth 4 | Set-Content "$Out/record.json"
exit $code
