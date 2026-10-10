param([string]$Root=$env:BC250_ROOT,[string]$Out, [ValidateSet("none","admin","tail","rtc")][string]$Mutation="none")
$ErrorActionPreference='Stop'
if(-not $Root -or -not $Out){throw 'Explicit Root and Out are required'}
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Out,(Join-Path $Root 'scratch\tmp') | Out-Null
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
$source=Get-Content -Raw "$repo\driver\kmd\uma_probe.c"
if(-not $source.Contains('#include "bc250kmd.h"')){throw 'Kernel include anchor missing'}
$source=$source.Replace('#include "bc250kmd.h"','/* Host boundary supplied by fixture. */')
if ($Mutation -ne 'none') {
    $anchor = switch ($Mutation) {
        'admin' { 'if (!Admin) {' }
        'tail' { 'if (count < Bytes) RtlZeroMemory((PUCHAR)Buffer + count, Bytes - count);' }
        'rtc' { '{0, 2, 4, 0x0d}' }
    }
    $replacement = switch ($Mutation) {
        'admin' { 'if (Admin == 255) {' }
        'tail' { '/* Deliberately expose unreturned tail. */' }
        'rtc' { '{0, 2, 4, 0x0c}' }
    }
    if ($source.Split(@($anchor),[StringSplitOptions]::None).Count -ne 2) { throw 'Mutation anchor changed' }
    $source=$source.Replace($anchor,$replacement)
}
$template=Get-Content -Raw "$PSScriptRoot\uma_probe_test.c"
[IO.File]::WriteAllText("$Out\actual.c",$template.Replace('/* ACTUAL_PROBE */',$source))
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\uma_probe_test.exe" "$Out\actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'UMA host compile failed'}
$lines=@(& "$Out\uma_probe_test.exe")
$code=$LASTEXITCODE
$lines | Write-Output
@{SourceSha256=(Get-FileHash "$repo\driver\kmd\uma_probe.c").Hash;FixtureSha256=(Get-FileHash "$PSScriptRoot\uma_probe_test.c").Hash;ExitCode=$code;Mutation=$Mutation;GeneratedSha256=(Get-FileHash "$Out\actual.c").Hash} | ConvertTo-Json | Set-Content "$Out\record.json"
if ($Mutation -eq 'none') { exit $code }
if ($code -ne 1 -or -not ($lines -match '^FAIL CHECK')) { throw 'Mutation did not fail runtime checks' }
exit 0
