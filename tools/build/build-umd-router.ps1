# Builds bc250d3d_router.dll (driver/umd/router: the desktop route, the AppRouter application policy and the
# M15.14 D3D11_1 front) and its host-test binaries: five UMD doubles (fake-*.dll) and test-router.exe.
# test-umd-router.ps1 runs the host gate on the output.
#
# The front (front-adapter.cpp, front-device.cpp, front-dxgi.cpp) is compiled into the DLL and into
# test-router.exe, so the host gate drives the production table fills and the production DirectFlip rule and not
# a copy of them. The DLL half is built without /EHsc and without /std:c++17, as the router always was: the front
# must stay C++14-safe and must not need exceptions.
#
#   pwsh tools\build\build-umd-router.ps1 [-OutputDir <dir>] [-VsInstall <dir>] [-Arch x64|x86]
#
# -Arch x86 builds the 32-bit router for UserModeDriverNameWow (BD-064). It links /MT, not /MD, so the 32-bit UMD
# set needs no x86 Visual C++ redistributable; the shells, engines, ICD and CPU UMD are /MT already. The recipe
# then checks that the image is an x86 one.
#
# The recipe is the one that built the registered router 674AD261 (2026-10-01, driver/umd/router/README.md):
# VS 2022 vcvars64 (MSVC 14.44.35207, cl 19.44.35221) with the WDK 10.0.26100.0 um and shared headers in front of
# INCLUDE; /O2 /MD /W4 /WX /Zi, linked /DEBUG /OPT:REF /OPT:ICF with /PDBALTPATH:%_PDB%, so the DLL names its PDB
# by file name only and carries no build path. Since build/release-from-branches the recipe also gives cl and link
# /Brepro and gives cl /FC with /d1trimfile:<repo>, so two builds of one commit in two directories give the same
# bytes (docs/design/reproducible-builds.md). The registered 674AD261 and the routers of b21 came before that: a
# rebuild differs from them in the PE and debug-directory timestamps and the PDB signature (pe_compare.py).
param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      # BD-166: the version resource this build writes into the file it ships, as its last step. -Version is the
      # package version (default: kmd_version of tools\release\release-sources.json), -Flavour the flavour of the
      # release this build is for. Without -Version nothing is stamped and the
      # build says so; the release reads every resource back before it ships.
      [string]$Version = '', [ValidateSet('release', 'lab')][string]$Flavour = 'release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir = Join-Path $root "scratch\build\umd-router$(if ($Arch -eq 'x86') { '-x86' })" }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir.Contains(' ')) { throw "OutputDir must not contain spaces (cl /Fo takes it with a trailing backslash): $OutputDir" }
$src = Join-Path $repo 'driver\umd\router'
$contract = Join-Path $repo 'driver\contract'
$wdk = Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0'
if (-not (Test-Path -LiteralPath "$wdk\um\d3d10umddi.h")) { throw "WDK headers not found under $wdk" }
$obj = Join-Path $OutputDir 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
# The host gate's objects go in their own directory: the front sources are compiled twice, once for the DLL and
# once for the exe, with different switches, and one /Fo directory for both would mix the two.
New-Item -ItemType Directory -Force (Join-Path $obj 'host') | Out-Null

# The per-application settings reader that ForwardApp compiles in (RenderOnCpu), with the router's flags.
& "$PSScriptRoot\test-umd-app-settings.ps1" -OutputDir (Join-Path $OutputDir 'quality\app-settings') -VsInstall $VsInstall -Arch $Arch

$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir; $env:TMP = $OutputDir
    $install = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $env:INCLUDE = "$wdk\um;$wdk\shared;$env:INCLUDE"
    Write-Host "Visual Studio: $install, cl $(Get-ClVersion), target $env:VSCMD_ARG_TGT_ARCH"
    $crt = if ($Arch -eq 'x86') { '/MT' } else { '/MD' }
    # /Brepro: a content hash where cl and link write a time. /FC with /d1trimfile: __FILE__ and the name that MSVC
    # gives an anonymous namespace (a hash of the source path) see only the path below the repository.
    $repro = @('/Brepro', '/FC', "/d1trimfile:$repo")
    # /GL with /LTCG at link and /arch:AVX2 on the router and its host gate: design decision of 2026-10-10 (b29).
    # BD-167: the release flavour compiles with NDEBUG, so an assert() added later is gone from the release and
    # live in the lab build.
    $opt = @('/O2', '/GL', '/arch:AVX2') + @(if ($Flavour -ne 'lab') { '/DNDEBUG' })
    Push-Location $OutputDir
    try {
        $stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler 'cl.exe' -Out "$obj\state" -Flags ($opt + @($crt) + $repro))
        $front = @("$src\front-adapter.cpp", "$src\front-device.cpp", "$src\front-dxgi.cpp")
        & cl.exe /nologo @opt $crt /W4 /WX /Zi /LD @repro "/Fo$obj\" "/Fd$obj\router-vc.pdb" "$src\router.cpp" @front @stateObjects "/Fe:$OutputDir\bc250d3d_router.dll" /link /Brepro /LTCG /DEBUG /OPT:REF /OPT:ICF "/PDB:$OutputDir\bc250d3d_router.pdb" '/PDBALTPATH:%_PDB%' onecore.lib uuid.lib setupapi.lib advapi32.lib
        if ($LASTEXITCODE -ne 0) { throw 'router build failed' }
        # The existing private-hive gate needs a state boundary double: SCM state
        # is not redirected by RegOverridePredefKey. Never ship this test DLL.
        $fixtureObj = Join-Path $obj 'fixture'
        New-Item -ItemType Directory -Force $fixtureObj | Out-Null
        & cl.exe /nologo @opt $crt /W4 /WX /LD @repro /DBC250_ROUTER_STATE_FIXTURE /DBC250_ROUTER_TEST_LOADER_OVERRIDES "/Fo$fixtureObj\" "$src\router.cpp" @front "$src\tests\state-fixture.cpp" "/Fe:$OutputDir\bc250d3d_router-test.dll" /link /Brepro /LTCG advapi32.lib
        if ($LASTEXITCODE -ne 0) { throw 'router test fixture build failed' }
        & cl.exe /nologo @opt $crt /W4 /WX /LD @repro /DBC250_ROUTER_STATE_FIXTURE "/Fo$fixtureObj\" "$src\router.cpp" @front "$src\tests\state-fixture.cpp" "/Fe:$OutputDir\bc250d3d_router-package-test.dll" /link /Brepro /LTCG advapi32.lib
        if ($LASTEXITCODE -ne 0) { throw 'router package-path fixture build failed' }

        & cl.exe /nologo /O2 $crt /W4 /WX @repro "/Fo$obj\host\state-test.obj" "$src\tests\test-state.cpp" "/Fe:$OutputDir\test-state.exe" /link /Brepro
        if ($LASTEXITCODE -ne 0) { throw 'router state test build failed' }
        & "$OutputDir\test-state.exe"
        if ($LASTEXITCODE -ne 0) { throw 'router state test failed' }
        # The doubles: one source, a tag per role (cpu, hosted, app), one without OpenAdapter10_2, one that fails.
        $doubles = @(
            @('fake-cpu', @('/DFAKE_TAG=0xC0')),
            @('fake-hosted', @('/DFAKE_TAG=0x60')),
            @('fake-app', @('/DFAKE_TAG=0xA0')),
            @('fake-nooa102', @('/DFAKE_TAG=0xB0', '/DFAKE_NO_OA102')),
            @('fake-fail', @('/DFAKE_TAG=0xFA', '/DFAKE_FAIL')))
        foreach ($d in $doubles) {
            $name = $d[0]; $defines = $d[1]
            & cl.exe /nologo /O2 $crt /W4 /WX /LD @repro @defines "/Fo$obj\$name.obj" "$src\tests\fake-umd.cpp" "/Fe:$OutputDir\$name.dll" /link /Brepro
            if ($LASTEXITCODE -ne 0) { throw "$name build failed" }
        }
        & cl.exe /nologo @opt $crt /W4 /WX /EHsc /std:c++17 @repro "/I$contract" "/Fo$obj\host\" "$src\tests\test-router.cpp" @front "/Fe:$OutputDir\test-router.exe" /link /Brepro /LTCG
        if ($LASTEXITCODE -ne 0) { throw 'test-router build failed' }
        # These cases use only UMD doubles and a private hive. None runs as dwm.exe,
        # publishes a desktop-route record, opens an adapter or starts a window.
        $fixture = Join-Path $OutputDir ('quality\state-fixture-' + [Guid]::NewGuid().ToString('N'))
        foreach ($dir in 'router','cpu','hives','routelogs','relocated\desktop','relocated\d3d11') {
            New-Item -ItemType Directory -Force (Join-Path $fixture $dir) | Out-Null
        }
        Copy-Item -LiteralPath "$OutputDir\bc250d3d_router-test.dll" -Destination "$fixture\router\bc250d3d_router.dll"
        $suffix = if ($Arch -eq 'x86') { '32' } else { '' }
        Copy-Item -LiteralPath "$OutputDir\fake-hosted.dll" -Destination "$fixture\router\bc250d3d_zink$suffix.dll"
        Copy-Item -LiteralPath "$OutputDir\fake-cpu.dll" -Destination "$fixture\cpu\bc250d3d.dll"
        Copy-Item -LiteralPath "$OutputDir\bc250d3d_router-package-test.dll" -Destination "$fixture\relocated\desktop\bc250d3d_router.dll"
        Copy-Item -LiteralPath "$OutputDir\fake-cpu.dll" -Destination "$fixture\relocated\desktop\bc250d3d$suffix.dll"
        Copy-Item -LiteralPath "$OutputDir\fake-app.dll" -Destination "$fixture\relocated\d3d11\amdgpu_wddm_d3d11$suffix.dll"
        foreach ($case in 'route-kill-switch','route-switches-off','route-switches-absent',
            'route-latched-on','route-latched-closed','route-latched-partial','route-latched-wrong-type',
            'route-switches-not-required','route-require-wrong-type','route-default-cpu-path',
            'route-relocated-cpu','route-relocated-gpu','route-package-cpu-override','route-package-gpu-override') {
            & "$OutputDir\test-router.exe" child $case $fixture
            if ($LASTEXITCODE -ne 0) { throw "router state fixture failed: $case" }
        }
        $machine = (& dumpbin.exe /nologo /headers bc250d3d_router.dll | Select-String 'machine \(') -join ' '
        $want = if ($Arch -eq 'x86') { '14C machine (x86)' } else { '8664 machine (x64)' }
        Write-Host "header: $machine"
        if (-not $machine.Contains($want)) { throw "router image is not $Arch" }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
Set-Bc250VersionStamp -Repo $repo -Files @((Join-Path $OutputDir 'bc250d3d_router.dll')) -Names @('bc250d3d_router.dll') `
    -Version $Version -Flavour $Flavour
'{0}  bc250d3d_router.dll' -f (Get-FileHash -LiteralPath (Join-Path $OutputDir 'bc250d3d_router.dll')).Hash
