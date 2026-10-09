param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\log-rate",[switch]$NoGapReset)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# The header under test is copied next to the build, so that the negative control changes the copy and never the
# tree (/I$Out comes first, so the copy wins). -NoGapReset is that control: a rate limit that does not notice a
# quiet gap. The line then stays silent for up to one whole interval after the path starts working again, and the
# model's gap cases must fail - which is the half of BD-097 that matters, because the work the lab looks for is
# exactly the first work after an idle desktop.
$rate=Get-Content "$repo\driver\kmd\log_rate.h" -Raw
if($NoGapReset){
    $live='else if (since >= (unsigned long long)BC250_LOG_RATE_GAP_MS &&'
    if(!$rate.Contains($live)){throw 'negative control: the gap clause moved'}
    $rate=$rate.Replace($live,'else if (0 &&')
}
[IO.File]::WriteAllText((Join-Path $Out 'log_rate.h'),$rate)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\log_rate_test.exe" "$PSScriptRoot\log_rate_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'log rate host build failed'}
& "$Out\log_rate_test.exe"
exit $LASTEXITCODE
