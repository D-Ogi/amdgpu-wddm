param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\m9\dcn-flip",[string]$Source='',[ValidateSet('','PendingOnly','AddressOnly','BoundAtStart','OverrunUncounted')][string]$Mutation='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd\dcn.c"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& python "$PSScriptRoot\generate_dcn_flip_test.py" --source $Source --out "$Out\dcn_flip_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
# BD-170 r6 mutants of DcnImmediateObserve, each must fail: either term of P' dropped, the bound held at the start of a
# sample instead of after its reads, and an observation that ended past the bound left uncounted.
if($Mutation) {
    $path="$Out\dcn_flip_actual.c"
    $text=[IO.File]::ReadAllText($path)
    $p='if (NT_SUCCESS(status) && !pendingSet && primary == request)'
    $b='if (DcnQpcUs(now.QuadPart - ProgramStartQpc, frequency) - record.ProgramUs >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US ||'
    $o='InterlockedIncrement(&Device->DcnImmediateOverruns);'
    $m=@{PendingOnly=@($p,'if (NT_SUCCESS(status) && !pendingSet)');AddressOnly=@($p,'if (NT_SUCCESS(status) && primary == request)')
         BoundAtStart=@($b,'if (us - record.ProgramUs >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US ||');OverrunUncounted=@($o,'(void)0;')}[$Mutation]
    if(([regex]::Matches($text,[regex]::Escape($m[0]))).Count -ne 1){throw "$Mutation mutation anchor changed"}
    [IO.File]::WriteAllText($path,$text.Replace($m[0],$m[1]))
}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\dcn_flip_test.exe" "$Out\dcn_flip_actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& "$Out\dcn_flip_test.exe"
exit $LASTEXITCODE
