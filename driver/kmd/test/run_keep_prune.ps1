param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\keep-prune",[switch]$IgnoreOrder)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# The header under test is copied next to the build, so that the negative control changes the copy and never the
# tree. -IgnoreOrder is that control: an offer that drops the oldest name it holds without first asking whether the
# new name is newer. The table then holds an arbitrary 200 of the files instead of the newest 200, and the model's
# selection check must fail.
$prune=Get-Content "$repo\driver\kmd\guard_keep_prune.h" -Raw
if($IgnoreOrder){
    $live='if (table->Count == BC250_KEEP_FILES && Bc250KeepCompare(held, table->Newest[0]) <= 0) return 1;'
    if(!$prune.Contains($live)){throw 'negative control: the fast-path line moved'}
    $prune=$prune.Replace($live,'if (table->Count == BC250_KEEP_FILES && 0) return 1;')
}
[IO.File]::WriteAllText((Join-Path $Out 'guard_keep_prune.h'),$prune)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\keep_prune_test.exe" "$PSScriptRoot\keep_prune_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'keep prune host build failed'}
& "$Out\keep_prune_test.exe"
exit $LASTEXITCODE
