param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\wddm-level",[switch]$NoClamp,[switch]$WithLevel23)
# Host control of wddm_level.h (ADR 0019 stage B4): the WDDMVersion of a start from the WddmVersionCap setting.
# The header under test is copied next to the build, so that a negative control changes the copy and never the
# tree (/I$Out comes first, so the copy wins). The catalog's allowed list of WddmVersionCap is passed to the
# program, which checks it against Bc250WddmLevels: the catalog and the header must name the same set.
# -NoClamp is the mechanism: a resolver that reports the stored level above the ceiling of the interface or the
# OS. -WithLevel23 is the set: 2.3 in the set, the level whose AMD-only dxgkrnl workaround is not resolved. Each
# control must fail the test.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$h=Get-Content "$repo\driver\kmd\wddm_level.h" -Raw
if($NoClamp){
    $live='    if (r.Requested > ceiling) {'
    if(!$h.Contains($live)){throw 'negative control: the clamp clause moved'}
    $h=$h.Replace($live,'    if (r.Requested > ceiling && clampWhy == 0xFFu) {')
}
if($WithLevel23){
    $live='BC250_WDDM_LEVEL_2_0, BC250_WDDM_LEVEL_2_1, BC250_WDDM_LEVEL_2_2, BC250_WDDM_LEVEL_2_4,'
    if(!$h.Contains($live)){throw 'negative control: the level set moved'}
    $h=$h.Replace($live,'BC250_WDDM_LEVEL_2_0, BC250_WDDM_LEVEL_2_1, BC250_WDDM_LEVEL_2_2, BC250_WDDM_LEVEL_2_3, BC250_WDDM_LEVEL_2_4,')
}
[IO.File]::WriteAllText((Join-Path $Out 'wddm_level.h'),$h)
$catalog=Get-Content "$repo\driver\state\catalog.json" -Raw | ConvertFrom-Json
$entry=@($catalog.values | Where-Object { $_.path -eq '' -and $_.name -eq 'WddmVersionCap' })
if($entry.Count -ne 1){throw 'driver/state/catalog.json has no root WddmVersionCap'}
$allowed=@($entry[0].allowed | ForEach-Object { "$_" })
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\wddm_level_test.exe" "$PSScriptRoot\wddm_level_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'wddm level host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\wddm_level_test.exe" $allowed
