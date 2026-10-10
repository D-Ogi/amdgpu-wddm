param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out,
 [ValidateSet('none','ambiguous','rtc','checksum')][string]$Mutation='none')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path
$env:TEMP=$Out;$env:TMP=$Out
$detector=Get-Content -Raw "$repo/driver/kmd/board_memory_detect.c"
$anchor='';$replacement=''
switch($Mutation) {
 'ambiguous' {$anchor='if (passes != 1)';$replacement='if (passes == 0)'}
 'rtc' {$anchor='if (!rtc_progress(mapping)) continue;';$replacement='(void)rtc_progress(mapping);'}
 'checksum' {$anchor='!bc250_uma_validate(candidate)) continue;';$replacement='0) continue;'}
}
if($Mutation -ne 'none') {
 if($detector.Split(@($anchor),[StringSplitOptions]::None).Count -ne 2){throw 'Mutation anchor changed'}
 $detector=$detector.Replace($anchor,$replacement)
}
$production=(Get-Content -Raw "$repo/driver/shim/bc250_uma.c") + "`n" + (Get-Content -Raw "$repo/driver/kmd/uma_transport.c") + "`n" + $detector
$production=$production.Replace('#include <ntddk.h>','/* mocked kernel boundary */')
$template=Get-Content -Raw "$PSScriptRoot/board_memory_detect_test.c"
Set-Content -Encoding ascii "$Out/actual.c" $template.Replace('/* PRODUCTION */',$production)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\detect.exe" "$Out\actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host compiler failed'}
$lines=@(& "$Out/detect.exe");$code=$LASTEXITCODE
$lines | Tee-Object "$Out/test.log"
@{Mutation=$Mutation;ExitCode=$code;SourceSha256=(Get-FileHash "$repo/driver/kmd/board_memory_detect.c").Hash;GeneratedSha256=(Get-FileHash "$Out/actual.c").Hash} | ConvertTo-Json | Set-Content "$Out/receipt.json"
if($Mutation -eq 'none'){exit $code}
if($code -ne 1 -or -not ($lines -match '^FAIL CHECK')){throw 'Mutation must fail runtime CHECK'}
exit 0
