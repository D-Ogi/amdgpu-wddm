param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [ValidateSet('none')][string]$Mutation = 'none'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Out,(Join-Path $Root 'scratch\tmp') | Out-Null
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
$actual = [IO.File]::ReadAllText("$PSScriptRoot\board_memory_write_test.c")
foreach ($pair in @(@('ACTUAL_WRITE','board_memory_write.c'),@('ACTUAL_UMA','uma.c'))) {
    $body = [IO.File]::ReadAllText("$repo\driver\kmd\$($pair[1])").Replace('#include "bc250kmd.h"','')
    $actual = $actual.Replace("/* $($pair[0]) */",$body)
}
[IO.File]::WriteAllText("$Out\actual.c",$actual)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$PSScriptRoot" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\board_memory_write_test.exe" "$Out\actual.c" "$repo\driver\kmd\board_memory_service.c" "$repo\driver\shim\bc250_uma.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'Service host compile failed' }
$lines = @(& "$Out\board_memory_write_test.exe")
$code = $LASTEXITCODE
$lines | Write-Output
$hashes = @{}
foreach ($path in 'driver/kmd/board_memory_service.c','driver/kmd/board_memory_service.h','driver/shim/bc250_uma.c','driver/shim/include/bc250_uma.h','driver/kmd/test/board_memory_service_test.c','driver/kmd/test/board_memory_write_test.c','driver/kmd/board_memory_write.c','driver/kmd/uma.c') {
    $hashes[$path] = (Get-FileHash (Join-Path $repo $path)).Hash
}
@{Sources=$hashes;Mutation=$Mutation;ExitCode=$code;Output=$lines;GeneratedSha256=(Get-FileHash "$Out\actual.c").Hash} | ConvertTo-Json -Depth 4 | Set-Content "$Out\record.json"
exit $code
