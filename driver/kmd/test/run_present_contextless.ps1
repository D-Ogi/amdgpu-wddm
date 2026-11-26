param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\present-contextless",[switch]$EveryLevel,[switch]$ServeWithoutContext)
# Host control of present_contextless.h (BD-403): the level of SupportContextlessPresent and the route of a
# DxgkDdiPresent call without a context. The header under test is copied next to the build, so that a negative
# control changes the copy and never the tree (/I$Out comes first, so the copy wins).
# -EveryLevel: the cap is reported at every level, also below 2.4. -ServeWithoutContext: a call without a context
# that asks for a copy is answered as a token, which drops the copy with success. Each control must fail the test.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$h=Get-Content "$repo\driver\kmd\present_contextless.h" -Raw
if($EveryLevel){
    $live='    return Level >= BC250_WDDM_LEVEL_2_4;'
    if(!$h.Contains($live)){throw 'negative control: the level rule moved'}
    $h=$h.Replace($live,'    return Level >= BC250_WDDM_LEVEL_2_0;')
}
if($ServeWithoutContext){
    $live='    if (WantsCopy || HasDmaBuffer) return BC250_PRESENT_CONTEXTLESS_REFUSED;'
    if(!$h.Contains($live)){throw 'negative control: the refusal moved'}
    $h=$h.Replace($live,'    (void)WantsCopy; if (HasDmaBuffer) return BC250_PRESENT_CONTEXTLESS_REFUSED;')
}
[IO.File]::WriteAllText((Join-Path $Out 'present_contextless.h'),$h)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\present_contextless_test.exe" "$PSScriptRoot\present_contextless_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'present contextless host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\present_contextless_test.exe"
