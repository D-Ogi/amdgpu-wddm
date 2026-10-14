# CPU-only test of the production lab gate, no hardware or operating-system policy changes.
param([Parameter(Mandatory)][string]$Out,[Parameter(Mandatory)][string]$Kits)
$ErrorActionPreference='Stop'
$oldTemp,$oldTmp=$env:TEMP,$env:TMP
try {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $env:TEMP=$Out;$env:TMP=$Out
    & python -B "$PSScriptRoot/lab_gate_extract.py" --out $Out
    if($LASTEXITCODE){throw 'Lab gate source extraction failed'}
    $vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
    $msvc=Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $sdk="$Kits/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0"
    $lib="$Kits/microsoft.windows.sdk.cpp.x64/c"
    & "$($msvc.FullName)/bin/Hostx64/x64/cl.exe" /nologo /TC /W4 /WX /O2 /MT /DAMDGPU_WDDM_LAB_FLAVOUR=1 `
        "/I$($msvc.FullName)/include" "/I$sdk/ucrt" "/Fo$Out/" "/Fe$Out/lab_gate_test.exe" `
        "$Out/lab_gate_test_actual.c" /link "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$lib/ucrt/x64" "/LIBPATH:$lib/um/x64"
    if($LASTEXITCODE){throw 'Lab gate host build failed'}
    & "$Out/lab_gate_test.exe"
    if($LASTEXITCODE){throw 'Lab gate assertions failed'}
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
