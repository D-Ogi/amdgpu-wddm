# The two controls of the review of bde0ff88 (make.py next to this script), built the way that
# review built them, against the frozen sources of one commit of this repository: MSVC /W4 /WX /O2
# /MT /Brepro and the SDK headers and libraries of the NuGet packages under -Kits. review_copy_lost
# is the loss-query control, review_hip_routes the additional host-processor routes, run with
# batching off and on. The exit code is 0 when every control passes and 1 when one fails.
#
#   pwsh compute\hip\tests\host\review-controls\run.ps1 -Commit <sha> [-FixedCount]
#
# -FixedCount expects one late retirement in the control's loss block instead of the review's two:
# the review counted the retirement that bde0ff88's read made on the lost device. Expected results:
# the fix (ffceaf51 and later) passes with -FixedCount and fails only the count line without it;
# bde0ff88 fails both ways.

# $Root is the workspace that holds toolchain\nuget and scratch, found as compute\hip\build.ps1
# finds it.
param(
    [string]$Commit = 'HEAD',
    [switch]$FixedCount,
    [string]$Root = $(
        if ($env:BC250_ROOT) { $env:BC250_ROOT } else {
            $probe = $PSScriptRoot
            while ($probe -and -not (Test-Path (Join-Path $probe 'toolchain\nuget'))) {
                $probe = Split-Path -Parent $probe
            }
            if ($probe) { $probe } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path }
        }
    ),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (& git -C $here rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0) { throw 'this script is not inside a git checkout' }
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\review-controls' }
$full = (& git -C $repo rev-parse --verify "$Commit^{commit}").Trim()
if ($LASTEXITCODE -ne 0) { throw "commit $Commit not found" }
$Out = Join-Path $Out ($full.Substring(0, 8) + $(if ($FixedCount) { '-fixed-count' } else { '' }))
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force (Join-Path $Out 'src') | Out-Null
$tarFile = Join-Path $Out 'src.tar'
& git -C $repo archive --format=tar -o $tarFile $full -- compute/hip driver/contract driver/shim/include docs/design/m16-hip-route-b.md
if ($LASTEXITCODE -ne 0) { throw "git archive failed ($LASTEXITCODE)" }
& tar -xf $tarFile -C (Join-Path $Out 'src')
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
Remove-Item -Force $tarFile
$src = Join-Path $Out 'src\compute\hip'
# make.py comes from this checkout, the sources from the commit under test.
$makeArgs = @('-I', (Join-Path $here 'make.py'), (Join-Path $src 'tests\host'))
if ($FixedCount) { $makeArgs += '--fixed-count' }
& python @makeArgs
if ($LASTEXITCODE -ne 0) { throw "make.py failed ($LASTEXITCODE)" }

# Compiler temporaries stay in the output directory, off drive C: of the development PC.
$env:TEMP = $Out; $env:TMP = $Out
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'no Visual Studio toolset found (vswhere returned nothing)' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$flags = @('/nologo','/W4','/WX','/O2','/MT','/Brepro','/D_CRT_SECURE_NO_WARNINGS','/DBC250_HIP_BUILD_DLL',
    "/I$($msvc.FullName)\include", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um",
    "/I$src\include", "/I$src\bc250hsa", "/I$Out\src\driver\contract",
    "/I$src\runtime", "/I$src\tests\host")
$link = @('/Brepro', "/LIBPATH:$($msvc.FullName)\lib\x64",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")
$script:failed = 0
function Compile([string[]]$Arguments) {
    & $cl @flags @Arguments | Out-File -Append -Encoding utf8 (Join-Path $Out 'build.log')
    if ($LASTEXITCODE) { throw "compiler exit $LASTEXITCODE, see build.log" }
}
function Run([string]$Name, [string]$Exe, [string[]]$Arguments = @()) {
    $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    if ($code -ne 0) { $script:failed++ }
    $lines | Set-Content -Encoding utf8 (Join-Path $Out "$Name.log")
    $lines | Where-Object { $_ -match '^REVIEW|checks, ' }
    "$Name exit $code, sha256 $((Get-FileHash $Exe).Hash)"
}
$hostObjects = @()
foreach ($name in 'status','co_msgpack','co_metadata','co_loader','kernarg','pm4_dispatch') {
    Compile @('/std:c11','/c',"/Fo$Out\$name.obj","$src\bc250hsa\$name.c")
    $hostObjects += "$Out\$name.obj"
}
Compile (@('/std:c11',"/Fo$Out\review_copy_lost.obj","/Fe$Out\review_copy_lost.exe","$src\tests\host\review_copy_lost.c") + $hostObjects + @('/link') + $link)
"commit $full"
Run 'review_copy_lost' "$Out\review_copy_lost.exe"
$runtimeObjects = @()
foreach ($name in 'hip_device','hip_error','hip_event','hip_launch','hip_journal','hip_log','hip_memory','hip_module','hip_perf','hip_stream') {
    Compile @('/std:c++17','/EHsc','/c',"/Fo$Out\$name.obj","$src\runtime\$name.cpp")
    $runtimeObjects += "$Out\$name.obj"
}
Compile @('/std:c11','/c',"/Fo$Out\hipmock_backend.obj","$src\tests\host\hipmock_backend.c")
Compile (@('/std:c++17','/EHsc',"/Fo$Out\review_hip_routes.obj","/Fe$Out\review_hip_routes.exe","$src\tests\host\review_hip_routes.cpp","$Out\hipmock_backend.obj") + $runtimeObjects + @('/link') + $link)
foreach ($batch in '0', '1') {
    $env:BC250_HIP_BATCH = $batch
    Run "review_hip_routes-batch$batch" "$Out\review_hip_routes.exe" @("$src\tests\data\hip_test_kernels.gfx1013.fatbin")
}
Remove-Item Env:BC250_HIP_BATCH
"controls failed: $script:failed of 3"
exit $(if ($script:failed) { 1 } else { 0 })
