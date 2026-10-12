param([Parameter(Mandatory)][string]$Out,[Parameter(Mandatory)][string]$Kits,
      [switch]$OmitGeneration,[switch]$OmitOpenCleanup,[switch]$OmitColdRecovery,
      [switch]$OmitFirstFailure,[switch]$NestedHdmiStop,[switch]$OmitDiagnosticReplay,[switch]$WriteRequestedValue,[switch]$WrongRecoveryTarget,[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$oldTemp,$oldTmp=$env:TEMP,$env:TMP
try {
    $env:TEMP=$Out;$env:TMP=$Out
    $extract=@('-B',"$PSScriptRoot/extract_connector_link.py",'--out',$Out)
    if($OmitGeneration){$extract+='--omit-generation'}
    if($OmitOpenCleanup){$extract+='--omit-open-cleanup'}
    if($OmitColdRecovery){$extract+='--omit-cold-recovery'}
    if($OmitFirstFailure){$extract+='--omit-first-failure'}
    if($NestedHdmiStop){$extract+='--nested-hdmi-stop'}
    if($OmitDiagnosticReplay){$extract+='--omit-diagnostic-replay'}
    if($WriteRequestedValue){$extract+='--write-requested-value'}
    if($WrongRecoveryTarget){$extract+='--wrong-recovery-target'}
    & python @extract
    if($LASTEXITCODE){throw 'Connector transaction extraction failed'}
    $vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
    $msvc=Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $cl="$($msvc.FullName)/bin/Hostx64/x64/cl.exe"
    $sdk="$Kits/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0"
    $lib="$Kits/microsoft.windows.sdk.cpp.x64/c"
    & $cl /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)/include" "/I$sdk/ucrt" "/I$Out" `
        "/Fo$Out/" "/Fe$Out/connector_link_test.exe" "$PSScriptRoot/connector_link_test.c" /link `
        "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$lib/ucrt/x64" "/LIBPATH:$lib/um/x64"
    if($LASTEXITCODE){throw 'Connector transaction host build failed'}
    $result=& "$Out/connector_link_test.exe" 2>&1
    $code=$LASTEXITCODE
    $result | Set-Content -Encoding UTF8 "$Out/result.log"
    $result | Write-Output
    if($ExpectFailure) {
        if((!$OmitGeneration -and !$OmitOpenCleanup -and !$OmitColdRecovery -and !$OmitFirstFailure -and !$NestedHdmiStop -and !$OmitDiagnosticReplay -and !$WriteRequestedValue -and !$WrongRecoveryTarget) -or $code -ne 1 -or !($result -match '^FAIL line ')){throw 'Negative control did not execute an assertion failure'}
        Write-Output 'Connector transaction negative control: expected assertion failure'
    } elseif($code){throw 'Connector transaction assertions failed'}
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
