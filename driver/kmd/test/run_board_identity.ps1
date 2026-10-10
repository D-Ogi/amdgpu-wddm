param([string]$Root=$env:BC250_ROOT,[string]$Out, [ValidateSet("none","foreign","firmware","probe")][string]$Mutation="none")
$ErrorActionPreference='Stop'
if(-not $Root -or -not $Out){throw 'Explicit Root and Out are required'}
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Out,(Join-Path $Root 'scratch\tmp') | Out-Null
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
$source=Get-Content -Raw "$repo\driver\kmd\uma_probe.c"
if(-not $source.Contains('#include "bc250kmd.h"')){throw 'Kernel include anchor missing'}
$source=$source.Replace('#include "bc250kmd.h"','/* Host boundary supplied by fixture. */')
$uma=(Get-Content -Raw "$repo\driver\kmd\uma.c").Replace('#include "bc250kmd.h"','')
$identity=Get-Content -Raw "$repo\driver\kmd\board_identity.c"
function Replace-One([string]$Text, [string]$Anchor, [string]$Replacement) {
    if ($Text.Split(@($Anchor),[StringSplitOptions]::None).Count -ne 2) {
        throw "Mutation anchor missing or ambiguous: $Anchor"
    }
    return $Text.Replace($Anchor,$Replacement)
}
if($Mutation -eq 'foreign') {
    $identity=Replace-One $identity 'read16(pci + 2) != 0x13fe' '0'
}
if($Mutation -eq 'firmware') {
    $identity=Replace-One $identity 'return bios == 1 && biosMatch;' '(void)biosMatch; return 1;'
}
if($Mutation -eq 'probe') {
    $source=Replace-One $source 'if (provider) {' 'if (1) {'
    $source=Replace-One $source 'provider->Probe(Device, Data, Admin, EscapeFlags);' '(void)provider; AblBoardMemoryProbeRequest(Device, Data, Admin, EscapeFlags);'
}
$capture=(Get-Content -Raw "$repo\driver\kmd\board_memory_identity.c").Replace('#include "bc250kmd.h"','').Replace('#include <aux_klib.h>','')
$template=Get-Content -Raw "$PSScriptRoot\board_identity_test.c"
[IO.File]::WriteAllText("$Out\actual.c",$template.Replace('/* ACTUAL_PROBE */',$source).Replace('/* ACTUAL_UMA */',$uma).Replace('/* ACTUAL_IDENTITY */',$identity).Replace('/* ACTUAL_CAPTURE */',$capture))
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\board_identity_test.exe" "$Out\actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'UMA host compile failed'}
$lines=@(& "$Out\board_identity_test.exe")
$code=$LASTEXITCODE
$lines | Write-Output
$hashes=@{}
foreach($name in 'board_identity.c','board_identity.h','board_memory_identity.c','uma.c','uma_probe.c') {
    $hashes[$name]=(Get-FileHash "$repo\driver\kmd\$name").Hash
}
@{Sources=$hashes;FixtureSha256=(Get-FileHash "$PSScriptRoot\board_identity_test.c").Hash;
  ExitCode=$code;Mutation=$Mutation;GeneratedSha256=(Get-FileHash "$Out\actual.c").Hash;Output=$lines} |
    ConvertTo-Json -Depth 4 | Set-Content "$Out\record.json"
if ($Mutation -eq 'none') { exit $code }
if ($code -ne 1 -or -not ($lines -match '^FAIL CHECK')) { throw 'Mutation did not fail runtime checks' }
exit 0
