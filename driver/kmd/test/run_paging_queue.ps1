param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\paging-queue",[ValidateSet('','--late-slot-release','--drop-quota','--drop-requeue','--flat-paging-budget')][string]$Mutation='',[switch]$ExpectFailure)
# The paging queue host test (experiments/E27-m9-inference/generate-paging-queue-test.py) against this tree:
# the actual drain, requeue, watchdog, stop drain and admission, with the real private-record parser.
# The mutations are negative controls, each of which must fail by a CHECK and never by a build error:
#   --late-slot-release     the completion is published before the OS slot is released.
#   --drop-quota            KMD171 behaviour: a drain retires for as long as retirements keep coming.
#   --drop-requeue          a quota without the requeue: the yield leaves the head with nothing to pick it up.
#   --flat-paging-budget    BD-114 7.1: node 1's deadline and timer are priced from the old flat 500 ms again,
#                           not from the budget WddmStart latched from TdrDelay.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$text=Get-Content "$repo\driver\kmd\wddm.c" -Raw
if($text -match '(?m)^<<<<<<<|^=======|^>>>>>>>'){throw 'Unresolved source conflict'}
$gen=@("$repo\experiments\E27-m9-inference\generate-paging-queue-test.py",$repo,"$Out\test.c")
if($Mutation){$gen+=$Mutation}
& python @gen
if($LASTEXITCODE -ne 0){throw 'paging queue test generation failed'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\paging_queue_test.exe" "$Out\test.c" "$repo\driver\kmd\paging_private.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'paging queue host build failed'}
& "$Out\paging_queue_test.exe"
if($ExpectFailure){if($LASTEXITCODE -eq 0){throw "negative control $Mutation unexpectedly passed"};Write-Host "Expected negative control $Mutation failed";exit 0};exit $LASTEXITCODE
