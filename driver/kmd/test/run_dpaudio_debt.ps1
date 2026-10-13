param([Parameter(Mandatory)][string]$Root,[Parameter(Mandatory)][string]$Out,[string]$Kits='')
$ErrorActionPreference='Stop'
if(!$Kits){$Kits=Join-Path $Root 'toolchain/nuget'}
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp/c'
$libs=Join-Path $Kits 'microsoft.windows.sdk.cpp.x64/c'
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl="$($msvc.FullName)/bin/Hostx64/x64/cl.exe"
$oldPath,$oldInclude,$oldLib=$env:PATH,$env:INCLUDE,$env:LIB
try {
    $env:PATH="$(Split-Path $cl);$oldPath";$env:INCLUDE='';$env:LIB=''
    New-Item -ItemType Directory -Force $Out | Out-Null
    $body=Get-Content "$repo/driver/kmd/dpaudio.c" -Raw
    $begin=[regex]::Match($body,'(?s)static BOOLEAN BeginStartLocked.*?\r?\n}').Value
    $debt=[regex]::Match($body,'(?s)audio->Written = run->Wrote != 0;.*?audio->TmdsMode=tmds;').Value
    $quiesced=[regex]::Match($body,'(?s)BOOLEAN DpAudioQuiesced.*?\r?\n}').Value
    if(!$begin -or !$debt -or ![regex]::IsMatch($body,'KeAcquireSpinLock\(&audio->Lock, &irql\);\s*if\(!BeginStartLocked')) {
        throw 'Actual leaf-lock admission not found'
    }
    if(!$quiesced){throw 'Actual quiescence query not found'}
    $text=$begin+"`nstatic void RecordDebtActual(BC250_DPAUDIO* audio,const TEST_RUN* run,BOOLEAN tmds){`n"+$debt+"`n}`n"+$quiesced+"`n"
    [IO.File]::WriteAllText((Join-Path $Out 'dpaudio_debt_actual.inc'),$text)
    & $cl /nologo /TC /W4 /WX /O2 /MT "/I$Out" "/I$repo/driver/kmd" "/I$($msvc.FullName)/include" `
        "/I$sdk/Include/10.0.26100.0/ucrt" "/Fo$Out/" "/Fe$Out/dpaudio_debt_test.exe" "$PSScriptRoot/dpaudio_debt_test.c" `
        /link "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$libs/ucrt/x64" "/LIBPATH:$libs/um/x64"
    if($LASTEXITCODE){throw 'Audio debt build failed'}
    & "$Out/dpaudio_debt_test.exe"
    if($LASTEXITCODE){throw 'Audio debt tests failed'}
    $mutations=@(
        @('admission','if(audio->Written || audio->StreamWritten)return FALSE;',''),
        @('stream-debt','audio->StreamWritten = run->StreamWrote != 0;','audio->StreamWritten = FALSE;')
    )
    try {
        foreach($mutation in $mutations) {
            if(!$text.Contains($mutation[1])){throw 'Mutation target missing'}
            [IO.File]::WriteAllText((Join-Path $Out 'dpaudio_debt_actual.inc'),$text.Replace($mutation[1],$mutation[2]))
            & $cl /nologo /TC /W4 /WX /O2 /MT "/I$Out" "/I$repo/driver/kmd" "/I$($msvc.FullName)/include" `
                "/I$sdk/Include/10.0.26100.0/ucrt" "/Fo$Out/" "/Fe$Out/mutant-$($mutation[0]).exe" "$PSScriptRoot/dpaudio_debt_test.c" `
                /link "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$libs/ucrt/x64" "/LIBPATH:$libs/um/x64"
            if($LASTEXITCODE){throw 'Audio debt mutant did not compile'}
            & "$Out/mutant-$($mutation[0]).exe" > "$Out/mutant-$($mutation[0]).txt"
            if($LASTEXITCODE -ne 1){throw 'Audio debt mutant was not rejected'}
            Write-Output "Audio debt $($mutation[0]) mutant rejected"
        }
    } finally {[IO.File]::WriteAllText((Join-Path $Out 'dpaudio_debt_actual.inc'),$text)}
} finally {$env:PATH=$oldPath;$env:INCLUDE=$oldInclude;$env:LIB=$oldLib}
exit 0
