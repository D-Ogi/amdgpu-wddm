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
    # Exercise the complete dispatcher with the production CLI initializer and
    # EscapeOn. Capture the KMT thunk; no Windows driver or hardware is loaded.
    $compiler="$($msvc.FullName)/bin/Hostx64/x64/cl.exe"
    foreach($mode in @('release','lab','old-id-runtime','old-id-compile')) {
        $defines=@()
        if($mode -ne 'release'){$defines+='/DAMDGPU_WDDM_LAB_FLAVOUR=1'}
        if($mode -like 'old-id-*'){$defines+='/DBC250_TEST_OLD_REPLUG_COMMAND=1'}
        if($mode -eq 'old-id-runtime'){$defines+='/DBC250_TEST_SKIP_COMMAND_CHECKS=1'}
        $buildLog="$Out/dispatch-$mode-build.log"
        & $compiler /nologo /TC /W4 /WX /O2 /MT @defines "/I$kmd" `
            "/I$($msvc.FullName)/include" "/I$sdk/ucrt" "/Fo$Out/dispatch-$mode.obj" "/Fe$Out/dispatch-$mode.exe" `
            "$Out/connector_dispatch_actual.c" /link "/LIBPATH:$($msvc.FullName)/lib/x64" `
            "/LIBPATH:$lib/ucrt/x64" "/LIBPATH:$lib/um/x64" *> $buildLog
        if($mode -eq 'old-id-compile') {
            if(!$LASTEXITCODE -or !(Select-String -LiteralPath $buildLog -Pattern 'C2196')) {
                Get-Content $buildLog;throw 'Duplicate-command compile control was not rejected'
            }
            'connector dispatch: old command rejected by compile-time uniqueness'
            continue
        }
        if($LASTEXITCODE){Get-Content $buildLog;throw "Dispatch $mode host build failed"}
        $runLog="$Out/dispatch-$mode.log"
        & "$Out/dispatch-$mode.exe" *> $runLog
        if($mode -eq 'old-id-runtime') {
            if(!$LASTEXITCODE -or !(Select-String -LiteralPath $runLog -Pattern '^FAIL')) {
                Get-Content $runLog;throw 'Old-command runtime regression was not detected'
            }
            'connector dispatch: old command caught with uniqueness guard removed'
        } else {
            if($LASTEXITCODE){Get-Content $runLog;throw "Dispatch $mode assertions failed"}
            Get-Content $runLog -Tail 1
        }
    }
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
