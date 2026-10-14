param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\m9\bd003-011\restore",[string]$Source='',[switch]$OldRuntimeOrder,[switch]$WrongSavedSurface,[switch]$DepositIgnored,[switch]$DepositProgrammedEarly)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& python "$PSScriptRoot\generate_post_display_test.py" --source $Source --out "$Out\post_display_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
if($OldRuntimeOrder) {
    $path="$Out\post_display_actual.c"
    $text=[IO.File]::ReadAllText($path)
    $pattern='(?s)    // DCN201 selects dcn10_program_pipe.*?    InterlockedExchange\(&Device->RuntimePlaneBlank,0\);\r?\n'
    $runtimeEnd=$text.IndexOf("NTSTATUS DcnLinkSurfaceCapture(")
    if($runtimeEnd -lt 0){throw "Runtime scope anchor missing"}
    $matches=[regex]::Matches($text.Substring(0,$runtimeEnd),$pattern)
    if($matches.Count -ne 1){throw 'Runtime-order mutation anchor missing or ambiguous'}
    $block=$matches[0].Value
    $text=$text.Remove($matches[0].Index,$matches[0].Length)
    $anchor='    if (restoreVisible) {'
    if(([regex]::Matches($text,[regex]::Escape($anchor))).Count -ne 1){throw 'Runtime-order destination ambiguous'}
    $text=$text.Replace($anchor,$block+$anchor)
    [IO.File]::WriteAllText($path,$text)
}

if($WrongSavedSurface) {
    $path="$Out\post_display_actual.c"
    $text=[IO.File]::ReadAllText($path)
    $anchor='*Surface=saved;return STATUS_SUCCESS;'
    if(([regex]::Matches($text,[regex]::Escape($anchor))).Count -ne 1){throw 'Saved surface mutation anchor changed'}
    $text=$text.Replace($anchor,'saved.Physical=Device->DcnFirmwareAddress;*Surface=saved;return STATUS_SUCCESS;')
    [IO.File]::WriteAllText($path,$text)
}
# BD-170 r6 mutants of DcnLinkSurfaceRestore: the deposit Resume hands out is ignored (the saved surface, which the OS
# may have released, is unblanked instead), or reported as programmed before the poll proved it on the plane.
if($DepositIgnored) {
    $path="$Out\post_display_actual.c"
    $text=[IO.File]::ReadAllText($path)
    $anchor='deposit=WddmWriterResume(Device,&desired);'
    if(([regex]::Matches($text,[regex]::Escape($anchor))).Count -ne 1){throw 'Deposit mutation anchor changed'}
    $text=$text.Replace($anchor,'(void)WddmWriterResume(Device,&desired);deposit=FALSE;')
    [IO.File]::WriteAllText($path,$text)
}
if($DepositProgrammedEarly) {
    $path="$Out\post_display_actual.c"
    $text=[IO.File]::ReadAllText($path)
    $late='    if (deposit) WddmWriterDepositProgrammed(Device,desired.Sequence);'
    $write='target.Pitch,4,&format,FALSE,TRUE);'
    if(([regex]::Matches($text,[regex]::Escape($late))).Count -ne 1 -or
       ([regex]::Matches($text,[regex]::Escape($write))).Count -ne 1){throw 'Deposit order mutation anchor changed'}
    $text=$text.Replace($late,'').Replace($write,$write+' if (deposit) WddmWriterDepositProgrammed(Device,desired.Sequence);')
    [IO.File]::WriteAllText($path,$text)
}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\post_display_test.exe" "$Out\post_display_actual.c" "$repo\driver\kmd\dcn_translate.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& "$Out\post_display_test.exe"
exit $LASTEXITCODE
