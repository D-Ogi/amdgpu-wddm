# CPU-only tests of the production replug request and its escape admission.
param([Parameter(Mandatory)][string]$Out,[Parameter(Mandatory)][string]$Kits)
$ErrorActionPreference='Stop'
$kmd=(Resolve-Path "$PSScriptRoot/..").Path
$oldTemp,$oldTmp=$env:TEMP,$env:TMP
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $env:TEMP=$Out;$env:TMP=$Out
    & python -B "$PSScriptRoot/connector_replug_extract.py" --out "$Out/connector_replug_actual.c"
    if($LASTEXITCODE){throw 'Replug source extraction failed'}
    $vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
    $msvc=Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $sdk="$Kits/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0"
    $lib="$Kits/microsoft.windows.sdk.cpp.x64/c"
    & "$($msvc.FullName)/bin/Hostx64/x64/cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$kmd" `
        "/I$($msvc.FullName)/include" "/I$sdk/ucrt" "/Fo$Out/" "/Fe$Out/connector_replug_test.exe" `
        "$Out/connector_replug_actual.c" /link "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$lib/ucrt/x64" "/LIBPATH:$lib/um/x64"
    if($LASTEXITCODE){throw 'Replug host build failed'}
    & "$Out/connector_replug_test.exe"
    if($LASTEXITCODE){throw 'Replug assertions failed'}
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
