param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [ValidateSet('none','pending','backup')][string]$Mutation = 'none'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Out,(Join-Path $Root 'scratch\tmp') | Out-Null
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
$actual = [IO.File]::ReadAllText("$repo\driver\kmd\board_memory_service.c")
if ($Mutation -ne 'none') {
    $anchor = if ($Mutation -eq 'pending') { 'if (!stored(store, saved, 1))' } else { 'if (!stored(store, saved, 0))' }
    $at = $actual.IndexOf($anchor)
    if ($at -lt 0) { throw 'Mutation anchor missing' }
    # Remove only the first matching admission step; keep the final journal clear.
    $actual = $actual.Substring(0,$at) + 'if (0)' + $actual.Substring($at+$anchor.Length)
}
[IO.File]::WriteAllText("$Out\service.c",$actual)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\board_memory_service_test.exe" "$PSScriptRoot\board_memory_service_test.c" "$Out\service.c" "$repo\driver\shim\bc250_uma.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'Service host compile failed' }
$lines = @(& "$Out\board_memory_service_test.exe")
$code = $LASTEXITCODE
$lines | Write-Output
$hashes = @{}
foreach ($path in 'driver/kmd/board_memory_service.c','driver/kmd/board_memory_service.h','driver/shim/bc250_uma.c','driver/shim/include/bc250_uma.h','driver/kmd/test/board_memory_service_test.c') {
    $hashes[$path] = (Get-FileHash (Join-Path $repo $path)).Hash
}
@{Sources=$hashes;Mutation=$Mutation;ExitCode=$code;Output=$lines;GeneratedSha256=(Get-FileHash "$Out\service.c").Hash} | ConvertTo-Json -Depth 4 | Set-Content "$Out\record.json"
if ($Mutation -eq 'none') { exit $code }
if ($code -ne 1 -or -not ($lines -match '^FAIL CHECK')) { throw 'Mutation did not fail runtime checks' }
exit 0
