param([string]$Root=$env:BC250_ROOT,[Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
if (!$Root) { throw 'Specify -Root' }
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content "$repo/driver/kmd/display.c" -Raw
$code=''
foreach($name in @('ScalingPinned','ConnectorSignal','ConnectorPathValid','ConnectorSourceMatches','ConnectorSignalMatches','ConnectorFindTiming')) {
    $match=[regex]::Match($source,'(?m)^static [^\r\n]*\b'+$name+'\(')
    if (!$match.Success) { throw "Missing actual helper $name" }
    $end=$source.IndexOf("`n}",$match.Index)
    if ($end -lt 0) { throw "Missing body end $name" }
    $code+=$source.Substring($match.Index,$end+3-$match.Index)+"`n"
}
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
$msvc=(Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$sdk="$Root/toolchain/nuget/microsoft.windows.sdk.cpp/c"
$libs="$Root/toolchain/nuget/microsoft.windows.sdk.cpp.x64/c"
$env:TEMP="$Root/scratch/tmp"; $env:TMP=$env:TEMP
$variants=@{current=$code}
$anchor='Signal->PixelRate == actual.PixelRate && '
if (($code.Split(@($anchor),[StringSplitOptions]::None)).Count -ne 2) { throw 'Mutation anchor not unique' }
$variants['wrong-clock']=$code.Replace($anchor,'')
foreach($name in @('current','wrong-clock')) {
    $dir=Join-Path $Out $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText("$dir/connector_modes_actual.inc",$variants[$name])
    Copy-Item "$PSScriptRoot/connector_modes_test.c" "$dir/test.c"
    & "$msvc/bin/Hostx64/x64/cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo/driver/kmd" "/I$msvc/include" "/I$sdk/Include/10.0.26100.0/ucrt" "/Fo$dir/" "/Fe$dir/test.exe" "$dir/test.c" "$repo/driver/kmd/dcn_translate.c" /link "/LIBPATH:$msvc/lib/x64" "/LIBPATH:$libs/ucrt/x64" "/LIBPATH:$libs/um/x64"
    if ($LASTEXITCODE -ne 0) { throw "$name compile failed" }
    $result=& "$dir/test.exe" 2>&1
    $exit=$LASTEXITCODE
    $result | Set-Content "$dir/result.txt"
    $result | Write-Output
    if ($name -eq 'current' -and $exit -ne 0) { throw 'Actual helper failed' }
    if ($name -ne 'current' -and ($exit -ne 1 -or !($result -match 'FAIL CHECK'))) { throw 'Mutation did not fail by CHECK' }
}
exit 0
