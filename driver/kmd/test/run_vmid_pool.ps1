param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\vmid-pool",[switch]$IgnoreRetirement)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# The header under test is copied next to the build, so that the negative control changes the copy and never the
# tree. -IgnoreRetirement is that control: a chooser that recycles a VMID without asking whether its last job has
# retired, with the rule check left in place and the model checking the safety property. It must fail by a check.
$pool=Get-Content "$repo\driver\kmd\vmid_pool.h" -Raw
if($IgnoreRetirement){
    $live='if (!(Members & (1u << v)) || !Bc250VmidRetired(Table, v, Observed)) continue;'
    if(!$pool.Contains($live)){throw 'negative control: the chooser line moved'}
    $pool=$pool.Replace($live,'if (!(Members & (1u << v)) || (Observed & 0u)) continue;')
}
[IO.File]::WriteAllText((Join-Path $Out 'vmid_pool.h'),$pool)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\vmid_pool_test.exe" "$PSScriptRoot\vmid_pool_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'vmid pool host build failed'}
& "$Out\vmid_pool_test.exe"
exit $LASTEXITCODE
