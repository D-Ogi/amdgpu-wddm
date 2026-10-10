# CPU-only comparison of the actual native mailbox transport with the old source.
param([string]$Root='P:\BC-250', [Parameter(Mandatory)][string]$Out,
      [string]$Baseline='', [ValidateSet('', 'gpu-point')][string]$Mutation='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$fixture=[IO.File]::ReadAllText("$PSScriptRoot\smu_native_test.c")
$main=$fixture.IndexOf('int main(void) {')
if($main -lt 0){throw 'Fixture main anchor missing'}
$fixture=$fixture.Substring(0,$main)
$needle='void NativeWrite(PULONG address,ULONG value) {'
if($fixture.Split(@($needle),[StringSplitOptions]::None).Count -ne 2){throw 'Write anchor changed'}
$fixture=$fixture.Replace($needle,$needle+@'

    printf("WRITE %08X %08X\n",(unsigned)((ULONG_PTR)address-(ULONG_PTR)registers),(unsigned)value);
'@)
$fixture+=@'
int main(void) {
    struct bc250_clock_report report;
    SmuOwnerInitialize(&owner); owner.BoardAllowed=TRUE;
    reply=1;mhz=1500;vid=104;
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    CHECK(SmuPrepareClock(&owner,&report)==STATUS_SUCCESS);
    CHECK(SmuSetPoint(&owner,1500,900,&report)==STATUS_SUCCESS);
    CHECK(SmuSetPoint(&owner,500,820,&report)==STATUS_SUCCESS);
    CHECK(SmuSetPoint(&owner,1000,820,&report)==STATUS_SUCCESS);
    SmuOwnerStop(&owner);
    printf("SEQUENCE %ld checks %ld failures\n",native_checks,native_failures);
    return native_failures?1:0;
}
'@
[IO.File]::WriteAllText("$Out\sequence.c",$fixture)
$variants=@('candidate')
if($Baseline) {
    $old=@(& git -C $repo show "${Baseline}:driver/kmd/smu.c")
    if($LASTEXITCODE -ne 0){throw 'Baseline extraction failed'}
    [IO.File]::WriteAllText("$Out\baseline-smu.c",($old -join "`n"))
    $variants=@('baseline','candidate')
} else {
    Copy-Item "$PSScriptRoot\board_sequence_baseline.txt" "$Out\baseline.log"
}
foreach($variant in $variants) {
    $source=if($variant -eq 'baseline'){"$Out\baseline-smu.c"}else{"$repo\driver\kmd\smu.c"}
    & powershell -NoProfile -File "$PSScriptRoot\run_smu_native.ps1" -Root $Root -Out "$Out\$variant" -Source $source -Fixture "$Out\sequence.c" *> "$Out\$variant.log"
    if($LASTEXITCODE -ne 0){throw "$variant sequence failed; see log"}
}
$a=@(Get-Content "$Out\baseline.log" | Where-Object {$_ -match '^WRITE '})
$b=@(Get-Content "$Out\candidate.log" | Where-Object {$_ -match '^WRITE '})
if($a.Count -eq 0 -or (Compare-Object $a $b -SyncWindow 0)){
    Write-Output 'FAIL: mailbox write sequence changed'
    throw 'Mailbox write sequence changed'
}
@{baseline=$Baseline;writes=$a.Count;identical=$true;scope='start, prepare floor, raise 1500/900, idle 500/820, restore 1000/820, stop';baselineSource=(Get-FileHash "$PSScriptRoot\board_sequence_baseline.txt").Hash;candidateSource=(Get-FileHash "$repo\driver\kmd\smu.c").Hash;fixture=(Get-FileHash "$Out\sequence.c").Hash} | ConvertTo-Json | Set-Content "$Out\comparison.json"
Write-Output "PASS: $($a.Count) identical native mailbox writes"

# Independently compile every moved constant and both tables with the old headers
# and old policy bodies. Sharing today's headers between these arms would hide
# an accidental change to a moved limit or operating point.
$oldRoot="$Out\old-policy"
if($Baseline) {
$paths=@(& git -C $repo ls-tree -r --name-only $Baseline driver/shim/include driver/shim/generated)
$paths+=@('driver/shim/bc250_clock.c','driver/shim/bc250_fan.c','driver/shim/bc250_hwmon.c')
foreach($path in $paths) {
    $destination=Join-Path $oldRoot $path
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    $content=@(& git -C $repo show "${Baseline}:$path")
    if($LASTEXITCODE -ne 0){throw "Cannot extract $path"}
    [IO.File]::WriteAllText($destination,($content -join "`n"))
}
}
$snapshot=[IO.File]::ReadAllText("$PSScriptRoot\board_policy_snapshot.c")
[IO.File]::WriteAllText("$Out\snapshot.c",$snapshot)
if(-not $Baseline) { Copy-Item "$PSScriptRoot\board_policy_baseline.txt" "$Out\baseline-snapshot.txt" }
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$candidateTree=$repo
if($Mutation -eq 'gpu-point') {
    $candidateTree="$Out\mutated-policy"
    New-Item -ItemType Directory -Force "$candidateTree\driver\shim" | Out-Null
    Copy-Item "$repo\driver\shim\include" "$candidateTree\driver\shim\include" -Recurse -Force
    Copy-Item "$repo\driver\shim\generated" "$candidateTree\driver\shim\generated" -Recurse -Force
    foreach($name in 'bc250_clock.c','bc250_fan.c','bc250_hwmon.c','bc250_board_gpu_points.inc','bc250_board_fan_profiles.inc') {
        Copy-Item "$repo\driver\shim\$name" "$candidateTree\driver\shim\$name"
    }
    $pointPath="$candidateTree\driver\shim\bc250_board_gpu_points.inc"
    $points=[IO.File]::ReadAllText($pointPath)
    $needle='{500,820,116}'
    if($points.Split(@($needle),[StringSplitOptions]::None).Count -ne 2){throw 'Point mutation anchor changed'}
    [IO.File]::WriteAllText($pointPath,$points.Replace($needle,'{500,821,116}'))
}
foreach($variant in $variants) {
    $tree=if($variant -eq 'baseline'){$oldRoot}else{$candidateTree}
    & "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT /Gy "/I$tree\driver\shim\include" "/I$tree\driver\shim\generated" "/I$repo\driver\amdgpu-import" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\$variant\" "/Fe$Out\$variant\snapshot.exe" "$Out\snapshot.c" "$tree\driver\shim\bc250_clock.c" "$tree\driver\shim\bc250_fan.c" "$tree\driver\shim\bc250_hwmon.c" /link /OPT:REF "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
    if($LASTEXITCODE -ne 0){throw "$variant snapshot compile failed"}
    & "$Out\$variant\snapshot.exe" | Set-Content "$Out\$variant-snapshot.txt"
    if($LASTEXITCODE -ne 0){throw "$variant snapshot failed"}
}
$baselineBytes=[Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText("$Out\baseline-snapshot.txt")).Replace("`r`n","`n"))
$candidateBytes=[Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText("$Out\candidate-snapshot.txt")).Replace("`r`n","`n"))
if([Convert]::ToBase64String($baselineBytes) -cne [Convert]::ToBase64String($candidateBytes)){
    Write-Output 'FAIL: compiled policy values or tables changed'
    throw 'Compiled policy values or tables changed'
}
Write-Output "PASS: 113 compiled constants and complete GPU/fan table values are byte-identical"
Get-FileHash "$Out\snapshot.c","$Out\baseline-snapshot.txt","$Out\candidate-snapshot.txt" | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$Out\policy-pins.json"
