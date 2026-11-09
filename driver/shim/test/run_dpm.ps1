# Builds and runs the DPM policy host test (dpm_test.c): the operating-point table against its
# anchors, the SMU message allowlist, the settings and boot guard, the governor's load steps,
# hysteresis and thermal clamps, and the session marker. Also compile-checks bc250_dpm.c and
# bc250_clock.c with the kernel flags. Host-side only: nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run_dpm.ps1
#   pwsh driver\shim\test\run_dpm.ps1 -Out P:\BC-250\scratch\build\dpm
#
# Everything is written under -Out, never into the repository and never onto drive C:.

#
# Negative controls (BD-329), each must make the test fail. The policy source is copied under -Out and the copy is
# changed, never the tree:
#   -NoSustain       the sustained raise chooses nothing (the single-tick rule of the releases before BD-329)
#   -NoSustainGuard  the sustained raise ignores its landing guard and may land at or under the lowering threshold
#   -NoRampFast      the thermal ramp's fast lane chooses nothing (the one-level ramp of rc3)
#   -RampFastUnbounded  the fast lane ignores its temperature top (BC250_DPM_RAMP_FAST_MAX_MC, rate_from_mc)

param(
    [string]$Out = 'P:\BC-250\scratch\build\dpm',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$NoSustain,
    [switch]$NoSustainGuard,
    [switch]$NoRampFast,
    [switch]$RampFastUnbounded,
    [switch]$ExpectFailure          # a negative control: exit 0 only when the test fails
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code|^Generowanie') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$sources = @('bc250_dpm.c', 'bc250_clock.c') | ForEach-Object { Join-Path $shim $_ }
if ($NoSustain -or $NoSustainGuard -or $NoRampFast -or $RampFastUnbounded) {
    $policy = [IO.File]::ReadAllText((Join-Path $shim 'bc250_dpm.c'))
    if ($NoSustain) {
        $live = 'want = next;'
        if (([regex]::Matches($policy, [regex]::Escape($live))).Count -ne 1) { throw 'negative control: the sustained raise moved' }
        $policy = $policy.Replace($live, '(void)next;')
    }
    if ($NoSustainGuard) {
        $live = '(unsigned long long)t->down_permille * bc250_dpm_level_mhz(next) <'
        if (-not $policy.Contains($live)) { throw 'negative control: the sustained raise guard moved' }
        $policy = $policy.Replace($live, '0ull * bc250_dpm_level_mhz(next) <')
    }
    if ($NoRampFast) {
        $live = 'ramp = fast;'
        if (([regex]::Matches($policy, [regex]::Escape($live))).Count -ne 1) { throw 'negative control: the fast lane moved' }
        $policy = $policy.Replace($live, '(void)fast;')
    }
    if ($RampFastUnbounded) {
        $live = 'in->temperature_mc < top &&'
        if (([regex]::Matches($policy, [regex]::Escape($live))).Count -ne 1) { throw 'negative control: the fast lane top moved' }
        $policy = $policy.Replace($live, 'in->temperature_mc < top + 100000 &&')
    }
    $mutant = Join-Path $Out 'mutant'
    New-Item -ItemType Directory -Force $mutant | Out-Null
    [IO.File]::WriteAllText((Join-Path $mutant 'bc250_dpm.c'), $policy)
    $sources = @((Join-Path $mutant 'bc250_dpm.c'), (Join-Path $shim 'bc250_clock.c'))
}
$incUser = @("/I$shim\include", "/I$imports",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS') +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $sources + @((Join-Path $shim 'test\dpm_test.c')))

Write-Host 'compile (kernel flags, bc250_dpm.c and bc250_clock.c)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$imports", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\") + $sources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\dpm_test.exe", "/PDB:$Out\dpm_test.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
& "$Out\dpm_test.exe"
$code = $LASTEXITCODE
Write-Host "dpm_test.exe exit code $code"
if ($ExpectFailure) {
    if ($code -eq 0) { Write-Host 'negative control: the test passed against the mutant'; exit 1 }
    Write-Host 'negative control: the test failed against the mutant, as it must'
    exit 0
}
exit $code
