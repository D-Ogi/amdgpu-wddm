# Builds and runs the two hardware-monitor host tests, and compile-checks the policy with the kernel flags.
#
#   hwmon_test.c         the policy (driver/shim/bc250_hwmon.c): the access sequence, the read allowlist, the
#                        refusal of every write, the base rules, the conversions, the plausibility rules, the
#                        identity and one sample, against the EC model in hwmon_ec_mock.h.
#   hwmon_native_test.c  the binding (driver/kmd/hwmon.c, compiled here against hwmon_native_mock.h): the
#                        registry gate, the start that never fails, the identity refusals, the sampler's retry
#                        and give-up rules, the published snapshot, the ageing and the escape.
#   fan_test.c           the fan control (driver/shim/bc250_fan.c, Part B): the write allowlist and the handshake
#                        order against the M803 engine model, the restore record, every exit path, doubt, the
#                        slope rule, the emergency, the lease and the chip's refusals.
#   fan_native_test.c    the fan control's binding (driver/kmd/fan.c with driver/kmd/hwmon.c, against
#                        fan_native_mock.h): the gate, the step, each exit path as the miniport calls it, the
#                        watchdog, the bugcheck callback, the escape and the stored choice.
#
# The second one compiles the shipping file itself, with only the Windows kernel primitives replaced, so it
# tests the driver and not a copy of it. Both assert that the EC model saw no write outside the page and the
# index port of its own window: that is the rule the whole read path rests on.
#
#   pwsh driver\shim\test\run_hwmon.ps1
#   pwsh driver\shim\test\run_hwmon.ps1 -Out P:\BC-250\scratch\build\hwmon
#   pwsh driver\shim\test\run_hwmon.ps1 -Mutation no-boost-raise      (a negative control: the suite must fail)
#
# -Mutation is the suite's own negative control (tools\quality\quick.ps1, Fails=$true). It changes one line of a
# COPY of the file under test, never the tree, and the suite must then fail by a FAIL line. A mutation whose line
# has moved stops the run instead of passing quietly, which is the half that keeps a control honest.
#
# Host-side only: nothing here touches the lab machine, and no port is written anywhere but in the model.
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\hwmon',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'no-boost-raise', 'boost-held-back', 'step-pays-all', 'telemetry-always', 'boost-claims-fixed',
                 'rise-outlives-doubt')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$kmd = Join-Path $repo 'driver\kmd'
# A mutated run keeps its own output directory unless the caller named one, so it never leaves a changed copy
# where the ordinary run's build is read afterwards.
if ($Mutation -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-$Mutation" }

# The negative controls of the fan rules (rule 10 of driver/shim/include/bc250_fan.h). Each one: which file, the
# one line it changes (a regular expression that must match exactly once), what takes its place, and which test
# must then report a FAIL.
$mutations = @{
    # "> BC250_FAN_FULL_PCT" and not "0 &&": a condition that is never true, and not one the compiler calls a
    # constant expression (/W4 /WX refuses C4127, and a control that cannot compile proves nothing).
    'no-boost-raise'   = @{ file = 'policy'; find = 'if \(target < BC250_FAN_FULL_PCT\)'
                            with = 'if (target > BC250_FAN_FULL_PCT)'
                            why = 'the feed-forward raises no duty: fan_test.c load_boost must fail' }
    'boost-held-back'  = @{ file = 'policy'; find = 'if \(!ctl->controlling\) \{(\s+)ctl->boost = 0;'
                            with = 'if (0) {$1ctl->boost = 0;'
                            why = 'the boost engages while the board has the fan: fan_test.c trace (f) must fail' }
    'step-pays-all'    = @{ file = 'policy'; find = 'dt > BC250_FAN_BOOST_STEP_MAX_MS \? BC250_FAN_BOOST_STEP_MAX_MS : dt'
                            with = 'dt'
                            why = 'one late step arms the boost alone: fan_test.c trace (g) must fail' }
    'telemetry-always' = @{ file = 'binding'; find = 'if \(!snap\.Ctl\.boost && snap\.Ctl\.boosts == 0\) return;'
                            with = 'if (0) return;'
                            why = 'the 5 s telemetry block grows to six fan lines (BD-097): telemetry_width must fail' }
    'boost-claims-fixed' = @{ file = 'policy'; find = 'ctl->boost_raised = 0;(\s+)track_rise\(ctl, dt\);'
                            with = 'ctl->boost_raised = ctl->boost;$1track_rise(ctl, dt);'
                            why = 'the arm claims a raise it has not made: fan_test.c trace (e) must fail' }
    'rise-outlives-doubt' = @{ file = 'policy'; find = 'forget_rise\(ctl\);(\s+)return;'
                            with = '(void)ctl;$1return;'
                            why = 'a measured rise outlives its reading: fan_test.c trace (j) must fail' }
}

function Use-Mutation([string]$text, [string]$which) {
    if (-not $Mutation -or $mutations[$Mutation].file -ne $which) { return $text }
    $m = $mutations[$Mutation]
    $hits = [regex]::Matches($text, $m.find).Count
    if ($hits -ne 1) { throw "negative control $Mutation : its line matched $hits times, once expected (has it moved?)" }
    Write-Host "mutation $Mutation ($which): $($m.why)"
    return [regex]::Replace($text, $m.find, $m.with)
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objPolicy = Join-Path $Out 'obj-policy'
$objNative = Join-Path $Out 'obj-native'
$objKern = Join-Path $Out 'obj-kernel'
$objFan = Join-Path $Out 'obj-fan'
$objFanNative = Join-Path $Out 'obj-fan-native'
New-Item -ItemType Directory -Force $Out, $objPolicy, $objNative, $objKern, $objFan, $objFanNative | Out-Null
Remove-Item "$objPolicy\*.obj", "$objNative\*.obj", "$objKern\*.obj", "$objFan\*.obj", "$objFanNative\*.obj" `
    -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code|^Generowanie') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$policy = Join-Path $shim 'bc250_hwmon.c'
$incUser = @("/I$shim\include",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

$fan = Join-Path $shim 'bc250_fan.c'
if ($Mutation -and $mutations[$Mutation].file -eq 'policy') {
    # The copy is compiled in place of the tree's file. Its own includes still come from driver\shim\include.
    $mutated = Join-Path $Out 'bc250_fan.mutated.c'
    [IO.File]::WriteAllText($mutated, (Use-Mutation (Get-Content -LiteralPath $fan -Raw) 'policy'),
        [Text.UTF8Encoding]::new($false))
    $fan = $mutated
}

# /wd4505: the model helpers that only one of the tests uses.
Write-Host 'compile (policy, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS', '/wd4505') +
    $incUser + @("/Fo$objPolicy\", "/Fd$objPolicy\cl.pdb", $policy, (Join-Path $here 'hwmon_test.c')))

Write-Host 'compile (fan control, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS', '/wd4505') +
    $incUser + @("/Fo$objFan\", "/Fd$objFan\cl.pdb", $policy, $fan, (Join-Path $here 'fan_test.c')))

Write-Host 'compile (policy and fan control, kernel flags)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\", $policy, $fan))

# The shipping binding, with its one kernel include swapped for the mock. Nothing else in the file is touched,
# and the generated copy is read back into the compiler so a diff of it is the whole difference under test.
Write-Host 'generate (the binding, with the kernel primitives mocked)'
$source = Get-Content -LiteralPath (Join-Path $kmd 'hwmon.c') -Raw
$needle = '#include "bc250kmd.h"'
if (-not $source.Contains($needle)) { throw "driver\kmd\hwmon.c no longer includes the miniport header: the mock cannot replace it" }
$native = $source.Replace($needle, '#include "hwmon_native_mock.h"')
[IO.File]::WriteAllText((Join-Path $Out 'hwmon-native.inc'), $native, [Text.UTF8Encoding]::new($false))
# The fan control's binding and the reader beside it, both against fan_native_mock.h (the same mock plus the
# watchdog's timer, the bugcheck callback and the request mutex).
$fanSource = Use-Mutation (Get-Content -LiteralPath (Join-Path $kmd 'fan.c') -Raw) 'binding'
if (-not $fanSource.Contains($needle)) { throw "driver\kmd\fan.c no longer includes the miniport header: the mock cannot replace it" }
[IO.File]::WriteAllText((Join-Path $Out 'fan-native.inc'), $fanSource.Replace($needle, '#include "fan_native_mock.h"'),
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out 'hwmon-fan.inc'), $source.Replace($needle, '#include "fan_native_mock.h"'),
    [Text.UTF8Encoding]::new($false))

Write-Host 'compile (binding, user mode)'
# /wd4201 the nameless struct inside D3DDDI_ESCAPEFLAGS, as d3dukmdt.h declares it; /wd4505 the model helpers
# that only one of the two tests uses.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS',
    '/wd4201', '/wd4505', "/I$Out", "/I$here", "/I$kmd") + $incUser +
    @("/Fo$objNative\", "/Fd$objNative\cl.pdb", $policy, (Join-Path $here 'hwmon_native_test.c')))

Write-Host 'compile (fan control binding, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS',
    '/wd4201', '/wd4505', "/I$Out", "/I$here", "/I$kmd") + $incUser +
    @("/Fo$objFanNative\", "/Fd$objFanNative\cl.pdb", $policy, $fan, (Join-Path $here 'fan_native_test.c')))

$link = @('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
    "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64")
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\hwmon_test.exe", "/PDB:$Out\hwmon_test.pdb") + (Get-ChildItem "$objPolicy\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\hwmon_native_test.exe", "/PDB:$Out\hwmon_native_test.pdb") + (Get-ChildItem "$objNative\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\fan_test.exe", "/PDB:$Out\fan_test.pdb") + (Get-ChildItem "$objFan\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\fan_native_test.exe", "/PDB:$Out\fan_native_test.pdb") + (Get-ChildItem "$objFanNative\*.obj").FullName)

Write-Host 'run'
$code = 0
foreach ($test in 'hwmon_test', 'hwmon_native_test', 'fan_test', 'fan_native_test') {
    & "$Out\$test.exe"
    if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE; break }
}
Write-Host "hardware monitor host tests exit code $code"
# A mutated run's build is an intermediate: its output directory is its own (the -Out above), nothing reads it
# after this line, and quick.ps1 keeps only the exit code. Six negative controls at 45 MB each is a quarter of a
# gigabyte on a disk this project may not fill, so each one takes its own away again.
if ($Mutation -and -not $PSBoundParameters.ContainsKey('Out')) {
    $env:TEMP = [IO.Path]::GetTempPath(); $env:TMP = $env:TEMP
    Remove-Item -LiteralPath $Out -Recurse -Force -ErrorAction SilentlyContinue
}
exit $code
