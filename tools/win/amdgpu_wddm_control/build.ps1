# Builds the amdgpu-wddm control application: amdgpu_wddm_control.exe (.NET Framework 4.8, part of Windows 10/11,
# nothing to install on a tester's PC) and bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c with
# BC250_CONTROL_DLL), and bc250kmd_cli.exe from the same file for the release's tools folder. csc comes from the
# installed Visual Studio. The native files are built by clang-cl and lld-link of LLVM 23.1.4 (-Compiler clang-cl, the
# default) or by cl and link of that Visual Studio (-Compiler msvc with -MsvcReason). The headers and import libraries
# come from the SDK NuGet packages under -Kits. Deterministic: the same sources give the same bytes (csc
# /deterministic, compiler and link /Brepro). A clang-cl build in another output directory differs in the per-build
# fields only (tools\build\pe_compare.py).
#
#   pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\release\control-app\build\app
#        [-StartConfirmCore <release installer's start-confirm-core.ps1>] [-Oracle <directory of the review oracles>] [-Lab]
#
# -Lab builds bc250control.dll and bc250kmd_cli.exe in the lab flavour (BD-167, AMDGPU_WDDM_LAB_FLAVOUR): the CLI
# then also has the raw diagnostic verbs, whose escapes only a lab KMD has. The application of a lab build also reads
# AMDGPU_WDDM_CONTROL_STATE, _INSTALLER, _NO_DLL and _STRINGS; a release build reads none of them. The gates run the
# application with AMDGPU_WDDM_CONTROL_STATE, so that a build records nothing in the profile of the PC that builds:
# a release build therefore runs them on a lab twin of its application (the same sources and resources with the lab
# define, in obj\gate beside copies of the files it loads from its own directory), and the unit tests are always lab.
#
# Gates, in order, each stops the build: the unit tests (test/UnitTests.cs against driver/kmd/bc250kmd_escape.h, the
# KMD's interop, guard and DPM sources, the D3D12 shell's switch names, and with -StartConfirmCore the installer's
# confirmation rule), the DLL and app compiles with warnings as errors, a smoke run of the exe with --smoke (no
# window: the pages are built and refreshed once, their text written to smoke.txt), the bug report smoke, the start
# diagnostics worker as the window starts it (no console, output redirected: BD-142), and the
# Recovery dry runs (--action X --dry-run: nothing is written, no UAC) against test/snapshot-bd059.json and this PC,
# the recovery view without bc250control.dll (--smoke-recovery), the hidden-window cost (--smoke-perf, G-PERF),
# the sign-in notice in both its cases, four languages and four scales on a small and a large screen
# (--smoke-notice: it fits the screen, overlaps nothing, names no internals, closes with Escape), the loading window
# of the start in both its states with the same checks (--smoke-loading),
# the place a rebuilt page keeps and the place of the buttons in every state of a benchmark run (--smoke-scroll), and
# the render gates (G-RENDER, G-NOINT, G-A11Y): --smoke-render of the 9 pages at 96, 120, 144 and 192 DPI in the four
# languages, at text scale 150 %, with the guide character on (the default of an installed application) and two runs
# with her off, and a language switch at run time; every page is drawn to PNG without a window and checked for
# overlap, overflow, internals in the text and accessible names. G-BACKDROP (BD-137) paints every control of the page
# alone over a colour no page uses and refuses a control that leaves any of it: a control that does not fill its own
# client rectangle shows, on the screen, the control painted into the shared double buffer before it, which a render
# into one bitmap cannot show. The guide panel must draw her picture wherever the
# side panel is in the window, and must not draw it with "Show Nagi" off.
# G-SMALL: six runs name the screen the window is drawn for (--work-area, in logical units) and
# hold the rule that a 1024x768, 800x600 or smaller screen draws no right-hand panel, and a larger one draws it.
# The smoke run passes on a PC without a BC-250 when it reports the driver as not found.
#
# The guide character (plan v7 section 6) is embedded by default from art\nagi of this directory, as the resources
# nagi.<expression>@128.png and @256.png (the map is Guide.ArtFile, the art's own PROVENANCE.md is beside it), and the
# built exe is refused unless it carries all 18. Every file is compared against the byte list of the art's own
# PROVENANCE.md (art\check-art.ps1). "Show Nagi" is on unless the user turns it off, so an installed application
# shows her without a setting change. -NagiArt <dir> embeds another directory in place of the repository's;
# -NagiArt '' embeds none, which leaves the text-only panel, and every render run of such a build uses --no-nagi,
# because a build that can draw no picture would otherwise fail its own art gate on every page.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$StartConfirmCore,
    [string]$NagiArt = (Join-Path $PSScriptRoot 'art\nagi'),   # '' embeds no art
    [string]$Oracle,
    [switch]$NoSmoke,
    # BD-166: the version resource this build writes into the two native files the release ships, as its last
    # step. -Version is the package version, which the release passes to every component build it runs, and
    # -Flavour is that release's flavour. Without -Version nothing is stamped and the build says so: a host build
    # and a quality gate are not a release, and the release reads every resource back before it ships.
    # amdgpu_wddm_control.exe is managed and carries its version from its own compile, so no stamp touches it.
    [string]$Version = '',
    [ValidateSet('release', 'lab')][string]$Flavour = 'release',
    [switch]$Lab,
    # The compiler of the two native files and of the state client objects in them. clang-cl of LLVM 23.1.4 is the
    # default (tools\build\umd-compiler.ps1), and lld-link of the same LLVM links its objects. -Compiler
    # msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy).
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = ''
)
# One flavour, two names: -Lab is BD-167's and -Flavour is BD-166's, and the release passes the one it knows.
if ($Lab) { $Flavour = 'lab' } elseif ($Flavour -eq 'lab') { $Lab = $true }

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null

$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.IO.Compression.dll', 'System.Management.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
$pure = 'KmdReply.cs', 'Profiles.cs', 'Redactor.cs', 'ManifestCheck.cs', 'Recovery.cs', 'Strings.cs', 'CuMode.cs', 'DriverCard.cs', 'UpdateCheck.cs',
    'AppSettings.cs', 'Guide.cs', 'RecentLaunches.cs', 'Sensors.cs', 'CacheInventory.cs', 'DisplayInfo.cs', 'Hints.cs', 'GameGroups.cs',
    'SettingsSearch.cs', 'HomeStatus.cs', 'PlainPlan.cs', 'HelpGuides.cs', 'CuRegistry.cs', 'Tuner.cs', 'TunerPlan.cs', 'TunerView.cs', 'FanPlan.cs', 'GraphicsSettings.cs', 'TdrSetting.cs',
    'GameLog.cs', 'HangRecord.cs',
    'LayoutRules.cs', 'UmaSetting.cs', 'BoardCapabilities.cs', 'SystemTuning.cs', 'Benchmark.cs', 'StartupDiagnosticData.cs', 'DiagnosticWorker.cs', 'DriverState.cs',
    'StartNotice.cs', 'Wallpaper.cs', 'Crash.cs', 'StartWindowHost.cs', 'BootLogoSetting.cs', 'ClosureNotice.cs' |
    ForEach-Object { Join-Path $here "src\$_" }
# The start of the window: the order of the loading window and the guard against a second window (Launch.cs).
$pure += 'SingleInstance.cs', 'LaunchSequence.cs' | ForEach-Object { Join-Path $here "src\$_" }

# Build the capability fixture from the actual provider and shipping shim tables, without adapter access.
$providerOut = Join-Path $obj 'board-provider'
& (Join-Path $repo 'driver\kmd\test\run_board_provider.ps1') -Root (Split-Path (Split-Path $Kits -Parent) -Parent) -Out $providerOut
$env:AMDGPU_WDDM_BOARD_FIXTURE = Join-Path $providerOut 'board-caps.bin'
$unknownBoardFixture = Join-Path $providerOut 'board-unknown.bin'
$unknownBoard = New-Object byte[] 768
[Array]::Copy([IO.File]::ReadAllBytes($env:AMDGPU_WDDM_BOARD_FIXTURE), $unknownBoard, 20)
[Array]::Copy([BitConverter]::GetBytes([uint32]1), 0, $unknownBoard, 28, 4)
[IO.File]::WriteAllBytes($unknownBoardFixture, $unknownBoard)

# 1. Unit tests of the pure parts. Their temporary files go below the output directory, never to the system drive's
# temp folder. With -Oracle the review oracles (oracle-cu.json, oracle-ver.json, oracle-art.json) in that directory are
# checked too (test/OracleTests.cs); the oracles are read where they are and never copied into the repository.
$testTmp = Join-Path $obj 'tmp'
New-Item -ItemType Directory -Force $testTmp | Out-Null
$env:TEMP = $testTmp; $env:TMP = $testTmp
$env:AMDGPU_WDDM_ORACLE = $(if ($Oracle) { (Resolve-Path $Oracle).Path } else { '' })
$env:AMDGPU_WDDM_SYSTEM_TUNING_FIXTURES = Join-Path $obj 'system-tuning-fixtures'
# Pure injected-system tests emit the exact backend JSON consumed by the C# contract tests. No native adapter loads.
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass `
    -File (Join-Path $repo 'tools\win\system-tuning\test-system-tuning.ps1') -Out $env:AMDGPU_WDDM_SYSTEM_TUNING_FIXTURES
if ($LASTEXITCODE -ne 0) { throw 'system tuning fake-system tests failed' }
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    /define:AMDGPU_WDDM_LAB_FLAVOUR "/out:$obj\unit-tests.exe" @pure (Get-ChildItem "$here\test\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
$unitArgs = @($repo) + @(if ($StartConfirmCore) { (Resolve-Path $StartConfirmCore).Path })
& "$obj\unit-tests.exe" @unitArgs | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }

# The actual exported native request code must reject absent/malformed board admission before mutation.
& (Join-Path $repo 'tools\win\bc250kmd_cli\test-telemetry.ps1') -Root (Split-Path (Split-Path $Kits -Parent) -Parent) -Out (Join-Path $obj 'native-controls')

# 2. bc250control.dll, the same translation unit and flags as tools\win\bc250kmd_cli\build.ps1.
$env:INCLUDE = ''; $env:LIB = ''
$sdkIncludes = @((Join-Path $msvc.FullName 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared")
$cc = Resolve-Bc250UmdCompiler (Split-Path (Split-Path $Kits -Parent) -Parent) $Compiler $MsvcReason $Out 'x64' $cl
if ($Compiler -eq 'clang-cl') {
    # clang-cl reads the CRT and SDK directories from INCLUDE as system headers, so /W4 /WX diagnose this code only.
    # INCLUDE and LIB are not empty, so clang-cl adds no headers or libraries of an installed Visual Studio or SDK.
    # The cl.exe and dumpbin.exe of the SDK boundary control come from the Visual Studio of this recipe.
    $env:INCLUDE = $sdkIncludes -join ';'
    $env:LIB = "$(Join-Path $msvc.FullName 'lib\x64');$sdkLib\ucrt\x64;$sdkLib\um\x64"
    $env:PATH = "$(Join-Path $msvc.FullName 'bin\Hostx64\x64');$env:PATH"
}
# The compile switches of one cl line in the selected compiler's form: the clang-cl form gives the CRT and SDK as
# system headers in place of /I.
function ConvertTo-ControlFlags([string[]]$Flags) {
    if ($Compiler -ne 'clang-cl') { return $Flags }
    $own = @($Flags | Where-Object { -not ($_ -like '/I*' -and $sdkIncludes -contains $_.Substring(2)) })
    # The debug information of an object records the compile directory, which is the directory the build is started
    # from. A fixed name keeps it out of the bytes.
    return @(ConvertTo-Bc250ClangFlags $own 'x64' @($repo)) + @('/clang:-ffile-compilation-dir=.', '/clang:-fuse-ld=lld')
}
# clang-cl links with lld-link of the same LLVM, named above. lld-link makes the relative source paths of the C
# runtime objects absolute in the PDB, against the current directory unless /pdbsourcepath names a base. The PDB GUID
# of a /Brepro link is a hash of the PDB, so without a fixed base the bytes depend on the directory the build is
# started from.
$clangLink = @(if ($Compiler -eq 'clang-cl') { '/pdbsourcepath:X:\bc250-build' })
if ($Compiler -eq 'clang-cl') {
    # The SDK boundary control of the shells. Its MSVC caller takes the WDK as /external:I, as the shells give it.
    $wdk = Join-Path $Kits "microsoft.windows.wdk.x64\c\Include\$KitVersion"
    $probeFlags = ConvertTo-ControlFlags @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/DNOMINMAX')
    Test-Bc250ClangSdk $cc ($probeFlags + @("/external:I$wdk\um", "/external:I$wdk\shared")) $obj
}
# /Z7 on the state client and on both products below: the debug information rides in the objects, so the two
# shipped native files get a PDB from /DEBUG at the link and no compiler database is written beside them.
# BD-167: the release flavour compiles the shipped objects with NDEBUG, so an assert() added to them later is gone
# from the release and live in the lab build.
$releaseDefine = @(if (-not $Lab) { '/DNDEBUG' })
$stateFlags = ConvertTo-ControlFlags (@('/O2','/MT','/Brepro','/Z7',"/I$($msvc.FullName)\include","/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um","/I$sdk\Include\$KitVersion\shared") + $releaseDefine)
# build-objects.ps1 runs the compiler itself: clang-cl reads CL and _CL_ as Invoke-Bc250UmdCompiler says.
$previousCl = $env:CL, $env:_CL_
if ($Compiler -eq 'clang-cl') { $env:CL = $null; $env:_CL_ = $null }
try { $stateObjects = & (Join-Path $repo 'tools\state\build-objects.ps1') -Compiler $cc -Out "$obj\state" -Flags $stateFlags -Exports }
finally { $env:CL, $env:_CL_ = $previousCl }
$flavourDefine = @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' }) + $releaseDefine
$dllFlags = ConvertTo-ControlFlags ($flavourDefine + @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/Brepro', '/Z7', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared"))
# clang-cl takes an object file name only for one input, and the state objects are inputs too, so it gets a directory.
function Get-ObjectSwitch([string]$Name) {
    if ($Compiler -ne 'clang-cl') { return "/Fo$obj\$Name.obj" }
    New-Item -ItemType Directory -Force "$obj\$Name" | Out-Null
    return "/Fo$obj\$Name\"
}
Invoke-Bc250UmdCompiler $cc @dllFlags @((Get-ObjectSwitch 'bc250control'), "/Fe$Out\bc250control.dll",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c')) @stateObjects @('/link', '/Brepro', '/DEBUG') @clangLink @(
    '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%', "/IMPLIB:$obj\bc250control.lib",
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib','onecore.lib','uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|Creating library|bc250control.lib') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250control.dll build failed ($LASTEXITCODE)" }
Remove-Item "$Out\bc250control.exp", "$Out\bc250control.lib" -ErrorAction SilentlyContinue

# The raw diagnostic verbs of the CLI against a fake adapter, in the flavour of this build (BD-167, test-verbs.ps1):
# a lab CLI must reach the escape of each handler, a release CLI must answer "unknown command" and open no adapter.
& (Join-Path $repo 'tools\win\bc250kmd_cli\test-verbs.ps1') -Kits $Kits -Out (Join-Path $obj 'verbs') -KitVersion $KitVersion `
    -Lab:$Lab | ForEach-Object { Write-Host $_ }

# 2b. bc250kmd_cli.exe from the same translation unit and the same flags, without BC250_CONTROL_DLL: the release ships
# the CLI and the DLL of one build, so the CLI has every command the DLL's source has (tester.10 shipped a CLI of the
# KMD branch without health, clock, telemetry, vram and sdmaib). Without arguments it prints its usage and exits 2.
$cliFlags = ConvertTo-ControlFlags ($flavourDefine + @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/Z7', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared"))
Invoke-Bc250UmdCompiler $cc @cliFlags @((Get-ObjectSwitch 'bc250kmd_cli'), "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c')) @stateObjects @('/link', '/Brepro', '/DEBUG') @clangLink @(
    '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib','onecore.lib','uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250kmd_cli.exe build failed ($LASTEXITCODE)" }
$usage = & "$Out\bc250kmd_cli.exe" 2>&1 | ForEach-Object { [string]$_ }
if ($LASTEXITCODE -ne 2 -or -not ($usage -match '^usage: bc250kmd_cli ')) { throw "bc250kmd_cli.exe without arguments: exit $LASTEXITCODE, no usage" }
Write-Host "  bc250kmd_cli.exe usage: $(@($usage).Count) lines"
& (Join-Path $repo 'tools\win\bc250kmd_cli\test-flavour.ps1') -Cli "$Out\bc250kmd_cli.exe" -Flavour $(if ($Lab) { 'lab' } else { 'release' }) |
    ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'bc250kmd_cli.exe is not the flavour this build asked for (test-flavour.ps1)' }

# 3. The application, with the string tables embedded.
# The lists are sorted: csc writes types and resources in input order, and a directory listing comes in the order of
# the file system, which the recipe does not own.
$resources = @(Get-ChildItem "$here\strings\strings.*.txt" | Sort-Object Name | ForEach-Object { "/resource:$($_.FullName),$($_.Name)" })
# The guide art of the repository (art\nagi) is embedded by default: an installed application shows the guide
# character without the tester finding a setting first. The names of the resources are checked in the built image
# below, so a renamed or lost file stops the build instead of shipping a text-only panel.
$artResourceNames = @()
if ($NagiArt) {
    # art\check-art.ps1 reads the expression map, refuses a missing file and compares every file against the byte
    # list of the art's own PROVENANCE.md, so a substituted or truncated picture stops the build here. The setup
    # window runs the same check on the same directory.
    foreach ($a in & (Join-Path $here 'art\check-art.ps1') -ArtDir $NagiArt -GuideSource (Join-Path $here 'src\Guide.cs')) {
        $resources += "/resource:$($a.Path),$($a.Resource)"
        $artResourceNames += $a.Resource
    }
}
# The application icon: art\icon\nagi.ico, the guide character's
# bust as a round badge, with its own PROVENANCE.md. csc takes it as /win32icon, which is what an ApplicationIcon of
# a project file compiles to. It is the icon of this window, of its task-bar button, and of the Start menu and
# desktop shortcuts the installer writes, which take the icon from this exe.
$icon = Join-Path $here 'art\icon\nagi.ico'
if (-not (Test-Path -LiteralPath $icon -PathType Leaf)) { throw "application icon: no $icon" }
# The same file as a resource as well (MainForm): the window's icon is then the authored image for the size Windows
# asks for, instead of one 32x32 image rescaled on a high-resolution screen.
$resources += "/resource:$icon,nagi.ico"
$csFlavour = @(if ($Lab) { '/define:AMDGPU_WDDM_LAB_FLAVOUR' })
# One list of sources for both compiles: this one and, in a release build, the lab twin below.
$appSources = @((Get-ChildItem "$here\src\*.cs").FullName | Sort-Object)
# /debug:portable: the application ships, so it gets a portable PDB, which the release archives beside the
# candidate and the manifest names by the GUID of the exe's own debug directory. csc has no /PDBALTPATH, and it
# records the full path of the PDB even when it is given a relative name. /pathmap replaces the output directory with
# '.' in that record: otherwise the shipped exe carries the absolute path of the build PC (tester.33 did).
Push-Location -LiteralPath $Out
try {
    & $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
        /debug:portable /pdb:amdgpu_wddm_control.pdb "/pathmap:$((Get-Location).Path.TrimEnd('\'))=." `
        @csFlavour "/win32manifest:$here\app.manifest" "/win32icon:$icon" "/out:amdgpu_wddm_control.exe" @resources @appSources
} finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$systemTuningOutput = Join-Path $Out 'system-tuning'
New-Item -ItemType Directory -Force -Path $systemTuningOutput | Out-Null
foreach ($part in 'system-tuning.ps1', 'SystemTuning.Core.psm1', 'SystemTuning.Native.psm1') {
    Copy-Item -LiteralPath (Join-Path $repo "tools\win\system-tuning\$part") -Destination (Join-Path $systemTuningOutput $part)
}
# The application the gates below run (BD-167): the built one in a lab build; in a release build its lab twin, the same
# compile with the lab define, in obj\gate beside copies of the files the application loads from its own directory.
$app = "$Out\amdgpu_wddm_control.exe"
if (-not $Lab) {
    $gateDir = Join-Path $obj 'gate'
    Remove-Item -Recurse -Force $gateDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $gateDir | Out-Null
    Get-ChildItem $Out -File | Where-Object { $_.Name -ne 'amdgpu_wddm_control.exe' } | Copy-Item -Destination $gateDir
    Copy-Item -Recurse $systemTuningOutput $gateDir
    $app = Join-Path $gateDir 'amdgpu_wddm_control.exe'
    & $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
        /define:AMDGPU_WDDM_LAB_FLAVOUR "/win32manifest:$here\app.manifest" "/win32icon:$icon" "/out:$app" @resources @appSources
    if ($LASTEXITCODE -ne 0) { throw "csc failed for the lab twin ($LASTEXITCODE)" }
}
# The application holds the switch names of its flavour only: a lab build all four, a release build none. Literals of a
# .NET image are UTF-16 in its user string heap.
$appBytes = [IO.File]::ReadAllBytes("$Out\amdgpu_wddm_control.exe")
$appImage = [Text.Encoding]::Unicode.GetString($appBytes) + [Text.Encoding]::Unicode.GetString($appBytes, 1, $appBytes.Length - 1)
$labNames = 'AMDGPU_WDDM_CONTROL_STATE', 'AMDGPU_WDDM_CONTROL_INSTALLER', 'AMDGPU_WDDM_CONTROL_NO_DLL', 'AMDGPU_WDDM_CONTROL_STRINGS'
$held = @($labNames | Where-Object { $appImage.Contains($_) })
if ($Lab -and $held.Count -ne $labNames.Count) { throw "amdgpu_wddm_control.exe (lab) lacks $(@($labNames | Where-Object { $held -notcontains $_ }) -join ', ')" }
if (-not $Lab -and $held.Count) { throw "amdgpu_wddm_control.exe (release) holds $($held -join ', ')" }
Write-Host "  application flavour: $(if ($Lab) { 'lab' } else { 'release' }), $($held.Count) of $($labNames.Count) lab variables in the image"
# The exe declares its framework (A5: SecurityProtocolType.SystemDefault means the OS's TLS choice only for a 4.7+ target).
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$Out\amdgpu_wddm_control.exe"))
if (-not $image.Contains('.NETFramework,Version=v4.8')) { throw 'amdgpu_wddm_control.exe does not declare TargetFramework .NETFramework,Version=v4.8' }
Write-Host '  target framework: .NETFramework,Version=v4.8 declared'
# Every expression the guide panel can ask for must be in the image under the name Guide.Resource builds, at both
# sizes. The names live in the assembly's string heap, so the image carries them literally.
if ($artResourceNames.Count) {
    $missing = @($artResourceNames | Where-Object { -not $image.Contains($_) })
    if ($missing.Count) { throw "guide art: the built exe has no resource $($missing -join ', ')" }
    Write-Host "  guide art: $($artResourceNames.Count) resources in the image ($(($artResourceNames | Sort-Object)[0]) .. $(($artResourceNames | Sort-Object)[-1]))"
}
if (-not $image.Contains('nagi.ico')) { throw 'application icon: the built exe has no resource nagi.ico' }
Write-Host '  application icon: nagi.ico as the Win32 icon and as a resource with every authored size'

# 4. Smoke run: no window, exits by itself. The DWM observations of the gates go to obj\state, not to this PC's profile.
$env:AMDGPU_WDDM_CONTROL_STATE = Join-Path $obj 'state'
Remove-Item -Recurse -Force $env:AMDGPU_WDDM_CONTROL_STATE -ErrorAction SilentlyContinue
if (-not $NoSmoke) {
    $smoke = Join-Path $obj 'smoke.txt'
    Remove-Item $smoke -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $app -ArgumentList '--smoke', "`"$smoke`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'smoke run did not exit within 60 s' }
    $text = if (Test-Path $smoke) { Get-Content $smoke -Raw } else { '' }
    if ($p.ExitCode -ne 0 -or $text -notmatch '(?m)^Status: ') { throw "smoke run failed (exit $($p.ExitCode)): $text" }
    ($text -split "`r?`n" | Select-Object -First 4) | ForEach-Object { Write-Host "  smoke: $_" }

    # The report path: a zip with the fixed entries and without this PC's user or computer name.
    $zip = Join-Path $obj 'report-smoke.zip'
    Remove-Item $zip, "$zip.error.txt" -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $app -ArgumentList '--smoke-report', "`"$zip`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'report smoke run did not exit within 60 s' }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $zip)) { throw "report smoke run failed (exit $($p.ExitCode))" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $names = @($archive.Entries | ForEach-Object Name)
        foreach ($want in 'driver-state.txt', 'driver-log.txt', 'installed-files.txt', 'settings.txt', 'system.txt') {
            if ($names -notcontains $want) { throw "report smoke: $want missing" }
        }
        foreach ($entry in $archive.Entries) {
            $reader = New-Object IO.StreamReader($entry.Open())
            $body = $reader.ReadToEnd(); $reader.Dispose()
            foreach ($secret in $env:USERNAME, $env:COMPUTERNAME) {
                if ($secret.Length -ge 2 -and $body -match "(?<![A-Za-z0-9])$([regex]::Escape($secret))(?![A-Za-z0-9])") {
                    throw "report smoke: $($entry.Name) names this PC's user or computer"
                }
            }
        }
        Write-Host "  report smoke: $($names.Count) files, no user or computer name"
    } finally { $archive.Dispose() }

    # The start diagnostics worker (BD-142): the built exe started as the window starts it (DiagnosticWorker.Run: no
    # shell, no window, standard output redirected), which is a Windows-subsystem process without a console. tester.29
    # ended every such worker with 0xE0434352 before its first line, and the smoke runs above never start it. The
    # text is checked here and not kept: it must run to its end with exit 0 and name neither this PC's user nor its
    # computer.
    $start = New-Object Diagnostics.ProcessStartInfo($app, '--startup-diagnostics-worker')
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
    $worker = [Diagnostics.Process]::Start($start)
    $read = $worker.StandardOutput.ReadToEndAsync()
    if (-not $worker.WaitForExit(40000)) { $worker.Kill(); throw 'startup diagnostics worker did not exit within 40 s' }
    $text = $read.Result
    if ($worker.ExitCode -ne 0) { throw "startup diagnostics worker: exit $($worker.ExitCode), $($text.Length) characters of output" }
    foreach ($want in 'Automatic start/stop diagnostics', '=== StartRecord ===', 'Collection finished.') {
        if (-not $text.Contains($want)) { throw "startup diagnostics worker: no '$want' in its output ($($text.Length) characters)" }
    }
    foreach ($secret in $env:USERNAME, $env:COMPUTERNAME) {
        if ($secret.Length -ge 2 -and $text -match "(?<![A-Za-z0-9])$([regex]::Escape($secret))(?![A-Za-z0-9])") { throw "startup diagnostics worker: its output names this PC's user or computer" }
    }
    Write-Host "  startup diagnostics worker: exit 0, $([Text.Encoding]::UTF8.GetByteCount($text)) bytes, every source reached, no user or computer name"
    $text = $null

    # The Recovery dry runs: the plans of the exe itself, from the recorded BD-059 state and from this PC.
    function Invoke-DryRun([string[]]$ArgList, [string]$Name) {
        if ($ArgList -contains '--snapshot' -and $ArgList -notcontains '--board-fixture') {
            $ArgList += @('--board-fixture', $env:AMDGPU_WDDM_BOARD_FIXTURE)
        }
        $file = Join-Path $obj "dry-$Name.txt"
        Remove-Item $file -ErrorAction SilentlyContinue
        $quoted = $ArgList | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }
        $p = Start-Process -FilePath $app -ArgumentList (@($quoted) + @('--out', "`"$file`"")) -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "dry run $Name did not exit within 60 s" }
        [pscustomobject]@{ Code = $p.ExitCode; Text = $(if (Test-Path $file) { Get-Content $file -Raw } else { '' }) }
    }
    $snapshot = Join-Path $here 'test\snapshot-bd059.json'
    $r = Invoke-DryRun @('--action', 'fan-auto', '--dry-run', '--board-fixture', $env:AMDGPU_WDDM_BOARD_FIXTURE) 'board-fixture-no-snapshot'
    if ($r.Code -ne 2) { throw "board fixture without a dry-run snapshot must be refused (exit $($r.Code))" }
    $r = Invoke-DryRun @('--action', 'fan-auto', '--snapshot', $snapshot, '--board-fixture', $env:AMDGPU_WDDM_BOARD_FIXTURE) 'board-fixture-no-dry-run'
    if ($r.Code -ne 2) { throw "board fixture without dry-run must be refused (exit $($r.Code))" }
    $r = Invoke-DryRun @('--action', 'fan-auto', '--dry-run', '--snapshot', $snapshot, '--board-fixture', $unknownBoardFixture) 'board-fixture-unknown'
    if ($r.Code -ne 3 -or -not $r.Text.Contains('dry run: nothing was written')) { throw "unknown board must refuse mutation planning (exit $($r.Code)): $($r.Text)" }
    $badBoardFixture = Join-Path $providerOut 'board-malformed.bin'
    [IO.File]::WriteAllBytes($badBoardFixture, [byte[]]@(0))
    $r = Invoke-DryRun @('--action', 'fan-auto', '--dry-run', '--snapshot', $snapshot, '--board-fixture', $badBoardFixture) 'board-fixture-malformed'
    if ($r.Code -ne 1 -or -not $r.Text.Contains('board fixture failed:')) { throw "malformed board fixture must be refused (exit $($r.Code)): $($r.Text)" }
    # RegWrite renders this legacy logical identifier in plans. RecoveryActions.Write routes it through
    # DriverStateBatch, not directly to this registry key; these assertions test preview text, not storage.
    $params = 'HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
    $expect = [ordered]@{
        'reopen-gpu-path' = @(0, "set $params EnableGpuPresentBlit = 1 (DWord)", "delete $params InteropClosedReason")
        # BD-181: the same unclean end closed the clock as well, so the one action puts both back and removes the
        # record of the close. This snapshot has no DpmClosedReason (a driver before 0.7.208.1), so the legacy
        # record DpmLastReason 4 names the closure and nothing deletes it: that record belongs to the last start.
        'reopen-closed'   = @(0, "set $params EnableGpuPresentBlit = 1 (DWord)", "set $params EnableCddDwmInterop = 1 (DWord)",
                              "set $params DpmMode = 1 (DWord)", "delete $params InteropClosedReason", 'at the next restart of Windows')
        'desktop-gpu'     = @(3, 'refused: The GPU desktop path is closed for this start')
        'desktop-cpu'     = @(3, 'refused: The desktop already runs on the CPU route')
        'confirm-start'   = @(0, 'confirm this driver start', 'undo: no')
        # The one click back from Microsoft Basic Display. This reading has a running driver, so the plan refuses it
        # and says why in the words the window shows; the lab arm exercises the state it repairs.
        'enable-driver'   = @(3, 'refused: The graphics driver is running already.')
        'enable-dpm'      = @(0, "set $params DpmMode = 1 (DWord)", "set $params DpmMaxMHz = 1700 (DWord)")
        'reset-defaults'  = @(0, 'set HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter DwmForceCpu = 1 (DWord)')
        'undo'            = @(3, 'refused: There is no action to undo')
        'set-clocks'      = @(0, "delete $params DpmMaxMHz")
        'restart-compositor' = @(3, 'refused: restart-compositor is an operator escape', '--accept-bd060')
        # The CU setter (G-PLAN, plan v7 section 7): the dry run lists the helper's steps, in the helper's order.
        'cu-mode:40'      = @(0, "S1 set $params CuMode = 40 (DWord)", "S1c set $params CuModeChosen = 0x28 (DWord)", 'S4 flush the key', 'undo: no', 'never restored')
        # The snapshot has no CuMode, so Standard (24) is a number to write, not a selection that is there already
        # (BD-077: an absent value reads to the release installer as a key it never configured).
        'cu-mode:24'      = @(0, "S1 set $params CuMode = 24 (DWord)", "S1c set $params CuModeChosen = 0x18 (DWord)", 'verify the stock selection (24)', 'never restored')
        # A count between the two: the mask of the documented default set of 30 cores is written
        # before CuMode, the record holds the whole request, and the note names the pairs in the words of the window.
        'cu-mode:30'      = @(0, "S3 set $params CuDisableWgp = 0x84218 (DWord)", "S1 set $params CuMode = 40 (DWord)",
                              "S1c set $params CuModeChosen = 0x8421828 (DWord)", 'Selects 30 of the 40 graphics cores.',
                              'group 1 pair 4', 'leaves 30 of the 40 graphics cores')
        # This snapshot carries no graphics-core reading, so a hand-picked set cannot be checked against the board
        # and is refused before anything is written.
        'cu-mode:map'     = @(3, 'refused: The graphics cores of this PC cannot be read')
        'cu-confirm'      = @(3, 'refused: No 40-core start is waiting for confirmation')
        'reset-defaults:keep' = @(0, 'note: Per-game settings are kept.')
        'game-undo'       = @(3, 'refused: There is no change of witcher3.exe to undo')
    }
    foreach ($e in $expect.GetEnumerator()) {
        $action = $e.Key.Split(':')[0]
        $extra = @(switch ($e.Key) { 'enable-dpm' { '--ceiling', '1700' } 'set-clocks' { '--mode', 'unset', '--ceiling', 'unset' }
            'cu-mode:40' { '--cu', '40' } 'cu-mode:24' { '--cu', '24' } 'cu-mode:30' { '--cu', '30' }
            'cu-mode:map' { '--cu', '30', '--cu-map', '0x84218' } 'reset-defaults:keep' { '--games', 'keep' } 'game-undo' { '--image', 'witcher3.exe' } })
        $r = Invoke-DryRun (@('--action', $action) + $extra + @('--dry-run', '--snapshot', $snapshot)) ($e.Key -replace ':', '-')
        if ($r.Code -ne $e.Value[0]) { throw "dry run $($e.Key): exit $($r.Code), $($e.Value[0]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 1) {
            if (-not $r.Text.Contains($want)) { throw "dry run $($e.Key): '$want' missing: $($r.Text)" }
        }
        if (-not $r.Text.Contains('dry run: nothing was written')) { throw "dry run $($e.Key): no closing line" }
    }
    # The tuning page (docs/design/tuner.md): a reading with the curve and the processor surface, so each plan is
    # formed in full. These actions write no registry value, so the dry run shows the request the helper would send.
    $tuned = Join-Path $here 'test\snapshot-tuner.json'
    $tune = [ordered]@{
        'tune-trial'  = @(0, 'tuning request curve-trial, curve 820,830,850,870,889,909,925,942,958,974,990', 'window 120000 ms', 'undo: no')
        'tune-keep'   = @(3, 'refused: No curve trial is running.')
        'tune-stop'   = @(3, 'refused: No curve trial is running.')
        'tune-reset'  = @(3, 'refused: The default curve is in use already.')
        'cpu-enable'  = @(3, 'refused: Processor tuning is on already.')
        'cpu-disable' = @(0, "set $params CpuTune = 0 (DWord)", 'at the next restart of Windows')
        'cpu-readback' = @(0, 'tuning request cpu-readback')
        'cpu-trial'   = @(0, 'tuning request cpu-trial', 'undervolt 8 steps', 'window 120000 ms')
        'cpu-keep'    = @(3, 'refused: No processor trial is running.')
        'cpu-reset'   = @(3, 'refused: The processor has its default settings already.')
        'core-mask'   = @(0, 'tuning request core-mask, cores 8', 'at the next restart of Windows')
        # WU-042: this saved snapshot predates StateSchema 2. Its CpuTune is already the release default 1,
        # so the legacy reset leaves it alone (a leading "!" means the text must not be there). Current
        # StateSchema 2 resets clear overrides, including an override equal to the default; test that below.
        # TunerStandardSteps and TunerResetCpuTune cover the stored tuning settings and legacy reset cases.
        'reset-defaults' = @(0, "!$params CpuTune", 'at the next restart of Windows')
        # The fan card (docs/design/fan.md Part B): the driver runs the standard curve with nothing stored.
        'fan-auto'    = @(0, 'tuning request fan-board', 'takes effect: at once', 'undo: no')
        'fan-curve'   = @(0, 'tuning request fan-curve, fan profile quiet', 'takes effect: at once')
        # The card's short test: one speed under a lease, then the standard curve in force again, nothing stored.
        'fan-test'    = @(0, 'tuning request fan-fixed, fixed 60 % for 10000 ms, lease 15000 ms, then fan-curve profile standard', 'takes effect: at once', 'undo: no')
        # The card's one registry switch (fan.md rule 10): the snapshot stores nothing, so the boost is on and
        # only switching it off has something to write.
        'fan-boost-off' = @(0, "set $params FanLoadBoost = 0 (DWord)", 'at the next restart of Windows')
        'fan-boost-on'  = @(3, 'refused: The fan runs at full speed under a heavy load already.')
    }
    foreach ($e in $tune.GetEnumerator()) {
        $extra = @(switch ($e.Key) { 'tune-trial' { '--curve', '820,830,850,870,889,909,925,942,958,974,990' }
            'reset-defaults' { '--games', 'keep' }
            'cpu-trial' { '--cpu-uv', '8' } 'core-mask' { '--cores', '8' } 'fan-curve' { '--fan-profile', 'quiet' } 'fan-test' { '--fan-test-pct', '60' } })
        $r = Invoke-DryRun (@('--action', $e.Key) + $extra + @('--dry-run', '--snapshot', $tuned)) "tune-$($e.Key)"
        if ($r.Code -ne $e.Value[0]) { throw "dry run $($e.Key): exit $($r.Code), $($e.Value[0]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 1) {
            if ($want.StartsWith('!')) {
                if ($r.Text.Contains($want.Substring(1))) { throw "dry run $($e.Key): '$($want.Substring(1))' must not be there: $($r.Text)" }
            } elseif (-not $r.Text.Contains($want)) { throw "dry run $($e.Key): '$want' missing: $($r.Text)" }
        }
    }
    # Current-state CLI controls: Disable writes zero, while Reset removes the override and exposes the
    # factory default. An absent override must stay absent even though the effective CpuTune is one.
    foreach ($cpuOverride in @('zero', 'one', 'absent')) {
        $current = Get-Content -LiteralPath $tuned -Raw | ConvertFrom-Json
        $rawCpu = if ($cpuOverride -eq 'zero') { 0 } else { 1 }
        $current.Parameters.CpuTune = $rawCpu
        $overrides = if ($cpuOverride -eq 'absent') { @{} } else { @{ CpuTune = $rawCpu } }
        $current | Add-Member -NotePropertyName StateSchema -NotePropertyValue 2 -Force
        $current | Add-Member -NotePropertyName ParameterOverrides -NotePropertyValue $overrides -Force
        $currentPath = Join-Path $obj "snapshot-tuner-state2-$cpuOverride.json"
        $current | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $currentPath -Encoding UTF8
        $r = Invoke-DryRun @('--action', 'reset-defaults', '--games', 'keep', '--dry-run', '--snapshot', $currentPath) "state2-reset-$cpuOverride"
        if ($r.Code -ne 0 -or -not $r.Text.Contains('dry run: nothing was written')) {
            throw "state v2 reset $cpuOverride failed (exit $($r.Code)): $($r.Text)"
        }
        $wantClear = $cpuOverride -ne 'absent'
        if ($r.Text.Contains("delete $params CpuTune") -ne $wantClear -or $r.Text.Contains("set $params CpuTune")) {
            throw "state v2 reset $cpuOverride must clear only an existing CpuTune override: $($r.Text)"
        }
        if ($cpuOverride -eq 'one') {
            $r = Invoke-DryRun @('--action', 'cpu-disable', '--dry-run', '--snapshot', $currentPath) 'state2-cpu-disable'
            if ($r.Code -ne 0 -or -not $r.Text.Contains("set $params CpuTune = 0 (DWord)") -or
                $r.Text.Contains("delete $params CpuTune") -or -not $r.Text.Contains('dry run: nothing was written')) {
                throw "state v2 cpu-disable must store zero (exit $($r.Code)): $($r.Text)"
            }
        }
    }
    # A curve under the driver's own floor, a core count this board does not take, and a trial without a curve: the
    # first two are refusals of the plan, the third is a usage error.
    $r = Invoke-DryRun @('--action', 'tune-trial', '--curve', '820,820,820,820,820,820,820,820,820,820,820', '--dry-run', '--snapshot', $tuned) 'tune-too-deep'
    if ($r.Code -ne 3 -or -not $r.Text.Contains('refused: The curve breaks rule Depth at 1200 MHz.')) { throw "a curve under the floor must be refused (exit $($r.Code)): $($r.Text)" }
    $r = Invoke-DryRun @('--action', 'core-mask', '--cores', '7', '--dry-run', '--snapshot', $tuned) 'tune-bad-cores'
    if ($r.Code -ne 3 -or -not $r.Text.Contains('refused: The core count must be 6 or 8.')) { throw "7 cores must be refused (exit $($r.Code)): $($r.Text)" }
    $r = Invoke-DryRun @('--action', 'fan-curve', '--fan-profile', 'custom', '--fan-curve', '40:30,30:60', '--dry-run', '--snapshot', $tuned) 'fan-falling'
    if ($r.Code -ne 3 -or -not $r.Text.Contains('refused: The fan curve breaks rule Temperature at point 2.')) { throw "a fan curve that falls back must be refused (exit $($r.Code)): $($r.Text)" }
    $r = Invoke-DryRun @('--action', 'fan-test', '--fan-test-pct', '55', '--dry-run', '--snapshot', $tuned) 'fan-test-off-list'
    if ($r.Code -ne 3 -or -not $r.Text.Contains('refused: The test speed must be one of 30, 40, 50, 60, 70, 80, 90, 100 %.')) { throw "a test speed off the list must be refused (exit $($r.Code)): $($r.Text)" }
    foreach ($bad in @(@('tune-trial'), @('tune-trial', '--curve', '820,840'), @('cpu-trial', '--cores', '8'), @('core-mask', '--curve', '820,840,860,880,899,919,935,952,968,984,1000'),
            @('fan-curve'), @('fan-auto', '--fan-profile', 'quiet'), @('fan-curve', '--fan-profile', 'quiet', '--fan-curve', '40:30,80:100'), @('fan-curve', '--fan-profile', 'custom'),
            @('fan-test'), @('fan-auto', '--fan-test-pct', '60'))) {
        $u = Invoke-DryRun (@('--action') + $bad + @('--dry-run', '--snapshot', $tuned)) ('usage-tune-' + ($bad -join '-' -replace '[^a-z0-9-]', ''))
        if ($u.Code -ne 2) { throw "--action $($bad -join ' ') must be refused as usage (exit $($u.Code))" }
    }
    Write-Host "  tuning dry runs: $($tune.Count) plans, 4 refusals, 10 usage errors"
    # The graphics settings for games (src/GraphicsSettings.cs): a setting for all games, a value stored already, a
    # value removed, one game's value removed together with its key left empty, a game value next to its switches,
    # the Vulkan rollback for all games (a stored gdi), and a reading without the keys (the BD-059 snapshot has none);
    # then the usage errors. WsiRoute takes its two presets only, and MemoryOverflow (GraphicsSettings.AwaitingIcd) is
    # not offered in this release, so --gfx refuses another route and that name as usage errors.
    $graphicsKey = 'HKLM\SOFTWARE\amdgpu-wddm\Graphics'
    $vulkanKey = 'HKLM\SOFTWARE\amdgpu-wddm\Vulkan'
    $w3Graphics = 'HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\witcher3.exe'
    $gfx = [ordered]@{
        'all-vsync'  = @(@('graphics-defaults', '--gfx', 'VSync=1,Anisotropy=16'), $tuned, 0, "set $graphicsKey VSync = 1 (DWord)", "set $graphicsKey Anisotropy = 16 (DWord)",
                         'takes effect: the next time a game starts', 'undo: yes')
        'all-stored' = @(@('graphics-defaults', '--gfx', 'FrameRateLimit=60'), $tuned, 3, 'refused: These settings for all games are stored already.')
        'all-unset'  = @(@('graphics-defaults', '--gfx', 'FrameRateLimit=unset'), $tuned, 0, "delete $graphicsKey FrameRateLimit")
        'game-unset' = @(@('game-profile', '--image', 'witcher3.exe', '--gfx', 'VSync=unset'), $tuned, 0, "delete $w3Graphics VSync", "delete key $w3Graphics (empty)",
                         'takes effect: the next time witcher3.exe starts')
        'game-both'  = @(@('game-profile', '--image', 'witcher3.exe', '--value', 'shader-model-68-off', '--gfx', 'FrameRateLimit=144'), $tuned, 0,
                         "set $w3Graphics FrameRateLimit = 144 (DWord)", 'shader-model-68-off')
        'all-vk-gdi' = @(@('graphics-defaults', '--gfx', 'WsiRoute=gdi'), $tuned, 0, "set $vulkanKey WsiRoute = `"gdi`" (String)")
        'unreadable' = @(@('graphics-defaults', '--gfx', 'VSync=1'), $snapshot, 3, 'refused: The graphics settings cannot be read')
    }
    foreach ($e in $gfx.GetEnumerator()) {
        $r = Invoke-DryRun (@('--action') + $e.Value[0] + @('--dry-run', '--snapshot', $e.Value[1])) "gfx-$($e.Key)"
        if ($r.Code -ne $e.Value[2]) { throw "dry run gfx $($e.Key): exit $($r.Code), $($e.Value[2]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 3) {
            if (-not $r.Text.Contains($want)) { throw "dry run gfx $($e.Key): '$want' missing: $($r.Text)" }
        }
    }
    foreach ($bad in @(@('graphics-defaults'), @('graphics-defaults', '--gfx', 'VSync=2'), @('graphics-defaults', '--gfx', 'Bogus=1'), @('graphics-defaults', '--gfx', 'VSync=1,VSync=0'),
            @('graphics-defaults', '--gfx', 'WsiRoute=dxgi-composition'), @('game-profile', '--image', 'witcher3.exe', '--gfx', 'MemoryOverflow=strict'),
            @('graphics-defaults', '--image', 'witcher3.exe', '--gfx', 'VSync=1'), @('reset-defaults', '--gfx', 'VSync=1'),
            @('game-profile', '--image', 'witcher3.exe'), @('game-undo', '--image', 'witcher3.exe', '--gfx', 'VSync=1'))) {
        $u = Invoke-DryRun (@('--action') + $bad + @('--dry-run', '--snapshot', $tuned)) ('usage-gfx-' + ($bad -join '-' -replace '[^a-zA-Z0-9-]', ''))
        if ($u.Code -ne 2) { throw "--action $($bad -join ' ') must be refused as usage (exit $($u.Code))" }
    }
    Write-Host "  graphics dry runs: $($gfx.Count) plans and refusals, 10 usage errors"
    # How long Windows waits for the graphics (src/TdrSetting.cs, BD-079): the one write, the range, and the
    # waiting time that is in force already. The recorded snapshots have no TdrDelay, which is the state of a
    # machine the installer never touched.
    $tdrKey = 'HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers'
    $tdr = [ordered]@{
        'set10'  = @(@('--tdr', '10'), 0, "set $tdrKey TdrDelay = 10 (DWord)", 'takes effect: at the next restart of Windows', 'undo: yes')
        'set2'   = @(@('--tdr', '2'), 0, "set $tdrKey TdrDelay = 2 (DWord)")
        'set60'  = @(@('--tdr', '60'), 0, "set $tdrKey TdrDelay = 60 (DWord)")
        'short'  = @(@('--tdr', '1'), 3, 'refused: The waiting time must be between 2 and 60 seconds.')
        'long'   = @(@('--tdr', '600'), 3, 'refused: The waiting time must be between 2 and 60 seconds.')
    }
    foreach ($e in $tdr.GetEnumerator()) {
        $r = Invoke-DryRun (@('--action', 'tdr-delay') + $e.Value[0] + @('--dry-run', '--snapshot', $snapshot)) "tdr-$($e.Key)"
        if ($r.Code -ne $e.Value[1]) { throw "dry run tdr $($e.Key): exit $($r.Code), $($e.Value[1]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 2) {
            if (-not $r.Text.Contains($want)) { throw "dry run tdr $($e.Key): '$want' missing: $($r.Text)" }
        }
    }
    # The dry run never writes, so the waiting time of this PC decides whether the same number is a change or a
    # refusal: both are a pass, a usage error is not.
    $r = Invoke-DryRun @('--action', 'tdr-delay', '--tdr', '10', '--dry-run') 'tdr-this-pc'
    if ($r.Code -ne 0 -and $r.Code -ne 3) { throw "dry run of the waiting time on this PC (exit $($r.Code)): $($r.Text)" }
    foreach ($bad in @(@('tdr-delay'), @('tdr-delay', '--tdr', 'ten'), @('tdr-delay', '--tdr', '-5'), @('reopen-gpu-path', '--tdr', '10'), @('reset-defaults', '--tdr', '10'))) {
        $u = Invoke-DryRun (@('--action') + $bad + @('--dry-run', '--snapshot', $snapshot)) ('usage-tdr-' + ($bad -join '-' -replace '[^a-zA-Z0-9-]', ''))
        if ($u.Code -ne 2) { throw "--action $($bad -join ' ') must be refused as usage (exit $($u.Code))" }
    }
    Write-Host "  graphics waiting time: $($tdr.Count) plans and refusals, 5 usage errors"
    # "Record game logs" (src/GameLog.cs): the one QWORD and the folder step, the refusals of a recording time
    # outside 1..60 minutes, and switching off with nothing recording. The recorded snapshots have no
    # GameLogUntil, which is the state of a machine where nobody ever switched recording on. The instant written
    # is this moment, so the gate checks the value name and its type and not the number.
    $diagKey = 'HKLM\SOFTWARE\amdgpu-wddm\Diagnostics'
    $gameLog = [ordered]@{
        'on10'    = @(@('game-log-on', '--minutes', '10'), 0, "set $diagKey GameLogUntil = ", '(QWord)',
                      'prepare %ProgramData%\amdgpu-wddm\logs\games', 'Users create files', 'prune that folder to 512 MiB',
                      'Records the driver''s game logs for 10 minutes', 'takes effect: the next time a game starts', 'undo: yes')
        'on1'     = @(@('game-log-on', '--minutes', '1'), 0, 'game logs for 1 minute,')
        'on60'    = @(@('game-log-on', '--minutes', '60'), 0, 'game logs for 60 minutes,')
        'on0'     = @(@('game-log-on', '--minutes', '0'), 3, 'refused: The recording time must be between 1 and 60 minutes.')
        'on61'    = @(@('game-log-on', '--minutes', '61'), 3, 'refused: The recording time must be between 1 and 60 minutes.')
        'off'     = @(@('game-log-off'), 3, 'refused: The game logs are not being recorded.')
    }
    foreach ($e in $gameLog.GetEnumerator()) {
        $r = Invoke-DryRun (@('--action') + $e.Value[0] + @('--dry-run', '--snapshot', $snapshot)) "gamelog-$($e.Key)"
        if ($r.Code -ne $e.Value[1]) { throw "dry run game log $($e.Key): exit $($r.Code), $($e.Value[1]) expected: $($r.Text)" }
        foreach ($want in $e.Value | Select-Object -Skip 2) {
            if (-not $r.Text.Contains($want)) { throw "dry run game log $($e.Key): '$want' missing: $($r.Text)" }
        }
        if (-not $r.Text.Contains('dry run: nothing was written')) { throw "dry run game log $($e.Key): no closing line" }
    }
    # A recording time belongs to game-log-on alone, and switching on without one is not a plan at all.
    foreach ($bad in @(@('game-log-on'), @('game-log-on', '--minutes', 'ten'), @('game-log-on', '--minutes', '-5'),
            @('game-log-off', '--minutes', '10'), @('tdr-delay', '--tdr', '10', '--minutes', '10'), @('reset-defaults', '--minutes', '10'))) {
        $u = Invoke-DryRun (@('--action') + $bad + @('--dry-run', '--snapshot', $snapshot)) ('usage-gamelog-' + ($bad -join '-' -replace '[^a-zA-Z0-9-]', ''))
        if ($u.Code -ne 2) { throw "--action $($bad -join ' ') must be refused as usage (exit $($u.Code))" }
    }
    # The dry run writes nothing, so the state of this PC decides whether switching off is a change or a refusal:
    # both are a pass, a usage error is not.
    $r = Invoke-DryRun @('--action', 'game-log-off', '--dry-run') 'gamelog-off-this-pc'
    if ($r.Code -ne 0 -and $r.Code -ne 3) { throw "dry run of switching the game logs off on this PC (exit $($r.Code)): $($r.Text)" }
    Write-Host "  game logs: $($gameLog.Count) plans and refusals, 6 usage errors"
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--snapshot', $snapshot) 'snapshot-without-dry-run'
    if ($r.Code -ne 2) { throw "a recorded snapshot without --dry-run must be refused (exit $($r.Code))" }
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--dry-run') 'this-pc'
    if (($r.Code -ne 0 -and $r.Code -ne 3) -or -not $r.Text.Contains('dry run: nothing was written')) { throw "dry run on this PC failed (exit $($r.Code)): $($r.Text)" }
    $u = Invoke-DryRun @('--action', 'set-clocks', '--ceiling', '1500', '--dry-run', '--snapshot', $snapshot) 'set-clocks-without-mode'
    if ($u.Code -ne 2) { throw "set-clocks without --mode must be refused as usage (exit $($u.Code))" }
    # A count outside 24 to 40 in twos, a count option on another action, and a chosen set on another action are
    # usage errors; the counts in between are valid now.
    foreach ($bad in @(@('cu-mode'), @('cu-mode', '--cu', '33'), @('cu-mode', '--cu', '48'), @('reopen-gpu-path', '--cu', '40'),
        @('reopen-gpu-path', '--cu-map', '0x10'), @('cu-mode', '--cu', '30', '--cu-map', 'zz'), @('game-profile', '--image', 'C:\x.exe', '--value', ''))) {
        $u = Invoke-DryRun (@('--action') + $bad + @('--dry-run', '--snapshot', $snapshot)) ('usage-' + ($bad -join '-' -replace '[^a-z0-9-]', ''))
        if ($u.Code -ne 2) { throw "--action $($bad -join ' ') must be refused as usage (exit $($u.Code))" }
    }
    foreach ($a in 'desktop-gpu', 'desktop-cpu', 'undo') {
        $u = Invoke-DryRun @('--action', $a, '--dry-run', '--snapshot', $snapshot) "no-dwm-$a"
        if ($u.Text -match 'restart DWM|DWM restart|watchdog') { throw "dry run $a still names a DWM restart: $($u.Text)" }
    }
    $u = Invoke-DryRun @('--action', 'restart-compositor', '--accept-bd060', '--dry-run', '--snapshot', $snapshot) 'escape-accepted'
    if ($u.Code -ne 0 -or -not $u.Text.Contains('stop DWM in the active session') -or -not $u.Text.Contains('BD-060')) { throw "dry run of the accepted escape (exit $($u.Code)): $($u.Text)" }
    # --status records the session's DWM (the smoke runs above already did, into obj\state); a later reading keeps the
    # first observation, and one instance is unknown history, never "observed" without a replacement.
    $since = @()
    foreach ($n in 1, 2) {
        $st = Invoke-DryRun @('--status') "status-$n"
        if ($st.Code -ne 0 -or $st.Text -notmatch '(?m)^dwm-restart: (observed|unknown-history|unknown) \(boot') { throw "--status failed (exit $($st.Code)): $($st.Text)" }
        $statusLine = (($st.Text -split "`r?`n") | Where-Object { $_ -like 'dwm-restart:*' } | Select-Object -First 1)
        $since += if ($statusLine -match 'watched since (\S+ by \w+)\)') { $Matches[1] } else { '-' }
    }
    if ($statusLine -notlike 'dwm-restart: unknown *') {
        $rec = Join-Path $env:AMDGPU_WDDM_CONTROL_STATE 'dwm-observations.json'
        if (-not (Test-Path $rec)) { throw "--status did not record the session's DWM in $rec" }
        if ($since[0] -eq '-' -or $since[0] -ne $since[1]) { throw "--status did not keep the first observation: $($since -join ' / ')" }
        # The installer's record, written as tools\release\installer\dwm-session.ps1 writes it (boot time from
        # Win32_OperatingSystem, logon time from WTSSessionInfo, the DWM from Win32_Process), must be read as this
        # session's earliest observation: the app's own epoch readings agree with the installer's.
        if (-not ('BuildGate.Wts' -as [type])) {
            Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
namespace BuildGate { public static class Wts {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct I { public int a, b, c, d, e, f, g, h;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string w; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 17)] public string x;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 21)] public string y; public long t0, t1, t2, LogonTime, t4; }
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode)] static extern bool WTSQuerySessionInformationW(IntPtr s, int id, int c, out IntPtr b, out int n);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr m);
    public static long LogonTime(int id) { IntPtr b; int n; if (!WTSQuerySessionInformationW(IntPtr.Zero, id, 24, out b, out n)) return -1;
        try { return ((I)Marshal.PtrToStructure(b, typeof(I))).LogonTime; } finally { WTSFreeMemory(b); } } } }
'@
        }
        $session = (Get-Process -Id $PID).SessionId
        $logon = [BuildGate.Wts]::LogonTime($session)
        $dwm = @(Get-CimInstance Win32_Process -Filter "Name='dwm.exe'" | Where-Object { $_.SessionId -eq $session } | Sort-Object CreationDate | Select-Object -Last 1)
        if ($session -ne 0 -and $logon -gt 0 -and $dwm.Count -eq 1) {
            $created = $dwm[0].CreationDate.ToUniversalTime()
            $rec = [ordered]@{ boot_utc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'); session = $session
                logon_utc = [DateTime]::FromFileTimeUtc($logon).ToString('o'); dwm_pid = [int]$dwm[0].ProcessId; dwm_created_utc = $created.ToString('o')
                recorded_utc = $created.AddSeconds(1).ToString('o'); recorded_by = 'start-confirm' }
            $baseline = Join-Path $env:AMDGPU_WDDM_CONTROL_STATE 'dwm-baseline.json'
            [IO.File]::WriteAllText($baseline, ([ordered]@{ schema = 1; records = @($rec) } | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
            $st = Invoke-DryRun @('--status') 'status-installer-record'
            $line = (($st.Text -split "`r?`n") | Where-Object { $_ -like 'dwm-restart:*' } | Select-Object -First 1)
            if ($line -notlike 'dwm-restart: unknown-history *' -or $line -notmatch 'instances seen 1, watched since \S+ by start-confirm\)') { throw "--status did not read the installer's record of this session: $line" }
            Remove-Item $baseline
            Write-Host "  installer record: read as this session's first observation"
        }
        # Eight observers at once on an empty record: the lock and the atomic replace leave one valid file with this
        # session's DWM and no temporary files (reviewer 911).
        $observations = Join-Path $env:AMDGPU_WDDM_CONTROL_STATE 'dwm-observations.json'
        Remove-Item $observations
        $racers = 1..8 | ForEach-Object {
            Start-Process -FilePath $app -ArgumentList '--status', '--out', "`"$(Join-Path $obj "status-race-$_.txt")`"" -PassThru -WindowStyle Hidden
        }
        foreach ($p in $racers) { if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'a concurrent --status did not exit within 60 s' } if ($p.ExitCode -ne 0) { throw "a concurrent --status failed (exit $($p.ExitCode))" } }
        $file = Get-Content $observations -Raw | ConvertFrom-Json
        $leftover = @(Get-ChildItem $env:AMDGPU_WDDM_CONTROL_STATE -Filter '*.tmp')
        if ($file.Schema -ne 2 -or @($file.Records).Count -ne 1 -or @($file.Records[0].Instances).Count -ne 1 -or $leftover.Count) {
            throw "concurrent observers left a bad record ($(@($file.Records).Count) records, $($leftover.Count) temporary files): $(Get-Content $observations -Raw)"
        }
        Write-Host "  observers: 8 concurrent --status, one valid record, no temporary files"
        # A damaged old epoch (a null instance) is dropped and this session's observation is still saved (reviewer 914).
        $damaged = '{"Schema":2,"Records":[{"BootId":1,"Session":1,"SessionStartUtc":"2026-01-01T00:00:00.000Z","Instances":[null]},' +
            '{"BootId":2,"Session":0,"SessionStartUtc":"bad","Instances":[{"Pid":0,"CreatedUtc":"bad","FirstSeenUtc":null,"Observer":"x"}]}]}'
        [IO.File]::WriteAllText($observations, $damaged, (New-Object Text.UTF8Encoding $false))
        $st = Invoke-DryRun @('--status') 'status-damaged-history'
        $file = Get-Content $observations -Raw | ConvertFrom-Json
        if ($st.Code -ne 0 -or @($file.Records).Count -ne 1 -or $file.Records[0].BootId -le 2 -or @($file.Records[0].Instances).Count -ne 1 -or $null -eq $file.Records[0].Instances[0]) {
            throw "--status did not replace a damaged history with this session's record (exit $($st.Code)): $(Get-Content $observations -Raw)"
        }
        Write-Host "  damaged history: dropped, this session's observation saved"
    }
    # A real run writes --out too. Only where it cannot change anything: without administrator it stops at once.
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $u = Invoke-DryRun @('--action', 'reopen-gpu-path') 'real-run-not-elevated'
        if ($u.Code -ne 5 -or -not $u.Text.Contains('needs administrator') -or -not $u.Text.Contains('exit 5')) { throw "a real run must write --out (exit $($u.Code)): $($u.Text)" }
    }
    Write-Host "  status of this PC: $statusLine"
    Write-Host "  dry runs: $($expect.Count) actions planned from the BD-059 snapshot as expected; this PC: $((($r.Text -split "`r?`n") | Where-Object { $_ -match '^\s+(refused|change):' } | Select-Object -First 1).Trim())"
}

if (-not $NoSmoke) {
    # The recovery view never loads bc250control.dll (WU-068); a hidden window costs nothing (G-PERF).
    $file = Join-Path $obj 'smoke-recovery.txt'
    $p = Start-Process -FilePath $app -ArgumentList '--smoke-recovery', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'recovery smoke did not exit within 60 s' }
    if ($p.ExitCode -ne 0) { throw "recovery smoke (exit $($p.ExitCode)): $(Get-Content $file -Raw -ErrorAction SilentlyContinue)" }
    Write-Host "  recovery view: built, bc250control.dll not loaded"
    $file = Join-Path $obj 'smoke-perf.txt'
    $p = Start-Process -FilePath $app -ArgumentList '--smoke-perf', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'perf smoke did not exit within 60 s' }
    $perf = Get-Content $file -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "perf smoke (exit $($p.ExitCode)): $perf" }
    Write-Host "  hidden window: $((($perf -split "`r?`n") | Where-Object { $_ }) -join ', ')"

    # The sign-in notice (NoticeForm.cs): both cases, the four languages, 96/120/144/192 DPI, a 1024x768 screen and
    # a large one, built without being shown. It must fit the screen, overlap nothing, overflow nothing, name no
    # driver internals and close with Escape (BD-137 found overlapping texts on Microsoft Basic Display).
    $file = Join-Path $obj 'smoke-notice.txt'
    $p = Start-Process -FilePath $app -ArgumentList '--smoke-notice', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'notice smoke did not exit within 120 s' }
    $notice = Get-Content $file -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "notice smoke (exit $($p.ExitCode)): $notice" }
    Write-Host "  sign-in notice: $((($notice -split "`r?`n") | Where-Object { $_ } | Select-Object -First 1))"

    # The loading window of the start (Launch.cs): both states, the same matrix as the notice, built without being
    # shown. The same checks, and the guide's picture must be there when the exe carries it.
    $file = Join-Path $obj 'smoke-loading.txt'
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-loading', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'loading smoke did not exit within 120 s' }
    $loading = Get-Content $file -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "loading smoke (exit $($p.ExitCode)): $loading" }
    Write-Host "  loading window: $((($loading -split "`r?`n") | Where-Object { $_ }) -join ', ')"

    # A rebuilt page keeps the place the person scrolled to (F1 of the b23 lab round) and the card the person reads
    # keeps its place while a card above it grows (F7 of the second round): the Performance page of the recorded
    # fixture, scrolled, then rebuilt as a progress report rebuilds it, and every state of a benchmark run with the
    # buttons measured against the view.
    $file = Join-Path $obj 'smoke-scroll.txt'
    $p = Start-Process -FilePath $app -ArgumentList '--smoke-scroll', "`"$file`"", '--fixture', "`"$(Join-Path $here 'test\snapshot-bd059.json')`"", '--board-fixture', "`"$env:AMDGPU_WDDM_BOARD_FIXTURE`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'scroll smoke did not exit within 60 s' }
    if ($p.ExitCode -ne 0) { throw "scroll smoke (exit $($p.ExitCode)): $(Get-Content $file -Raw -ErrorAction SilentlyContinue)" }
    Write-Host '  rebuilt page: the place is kept, and the buttons of a run stay where they are'

    # G-RENDER matrix: this PC's state and the recorded fixture (warnings, games, the CU section).
    $fixture = Join-Path $here 'test\snapshot-bd059.json'
    $runs = @()
    foreach ($lang in 'en', 'pl', 'ja', 'ko') {
        foreach ($scale in '1', '1.25', '1.5', '2') { $runs += , @("$lang-$scale", $scale, '--lang', $lang, '--fixture', "`"$fixture`"") }
        $runs += , @("$lang-1-unknown-board", '1', '--lang', $lang, '--fixture', "`"$fixture`"", '--board-fixture', "`"$unknownBoardFixture`"")
    }
    $runs += , @('en-1-pc', '1')
    $runs += , @('pl-1-text150', '1', '--lang', 'pl', '--text-scale', '1.5', '--fixture', "`"$fixture`"")
    $runs += , @('ja-1.5-text150', '1.5', '--lang', 'ja', '--text-scale', '1.5', '--fixture', "`"$fixture`"")
    # Every run above draws the guide character, because she is on by default; these two draw the panel with her off.
    $runs += , @('en-1-no-nagi', '1', '--no-nagi', '--fixture', "`"$fixture`"")
    $runs += , @('ko-2-no-nagi', '2', '--lang', 'ko', '--no-nagi', '--fixture', "`"$fixture`"")
    $runs += , @('en-1-switch-ja', '1', '--switch-to', 'ja', '--fixture', "`"$fixture`"")
    $runs += , @('ja-1-switch-pl', '1', '--lang', 'ja', '--switch-to', 'pl', '--fixture', "`"$fixture`"")
    # The tuning cards in their other states (docs/design/tuner.md): the BD-059 runs above draw a curve test and saved
    # processor settings; these draw the standard curve with a readback behind it, and the card that waits for a restart.
    $tuner = Join-Path $here 'test\snapshot-tuner.json'
    $tunerOff = Join-Path $here 'test\snapshot-tuner-off.json'
    $runs += , @('en-1-tuner', '1', '--fixture', "`"$tuner`"")
    $runs += , @('pl-1.25-tuner', '1.25', '--lang', 'pl', '--fixture', "`"$tuner`"")
    $runs += , @('ja-1.5-tuner', '1.5', '--lang', 'ja', '--fixture', "`"$tuner`"")
    $runs += , @('ko-2-tuner', '2', '--lang', 'ko', '--fixture', "`"$tuner`"")
    $runs += , @('en-1-tuner-off', '1', '--fixture', "`"$tunerOff`"")
    $runs += , @('pl-1-tuner-off', '1', '--lang', 'pl', '--fixture', "`"$tunerOff`"")
    # BD-181: the notice card of Home, in the four languages. This fixture is the state unit A was left in after the
    # 0x116 of 2026-10-15 01:05Z: the driver closed the GPU desktop path and the automatic clock at the 01:12:22Z
    # start and left the durable record of each (InteropClosedReason 4, DpmClosedReason 4), so later starts read the
    # zeros as "not requested" and the desktop stayed on the processor. The BD-059 runs above draw the same card from
    # the legacy record (DpmLastReason), which is how a driver before 0.7.208.1 leaves the clock.
    $closed = Join-Path $here 'test\snapshot-bd181.json'
    foreach ($lang in 'en', 'pl', 'ja', 'ko') {
        $runs += , @("$lang-1-closed", '1', '--lang', $lang, '--fixture', "`"$closed`"")
    }
    $runs += , @('ja-1.5-closed-text150', '1.5', '--lang', 'ja', '--text-scale', '1.5', '--fixture', "`"$closed`"")
    $runs += , @('en-1-closed-work800x600', '1', '--work-area', '800x600', '--fixture', "`"$closed`"")
    # G-SMALL, the rule: the five working areas of a tester's screen, in logical units. The
    # four small ones must draw no right-hand panel and the 1920x1080 one must draw it; overlap, overflow and the
    # page width are checked in every one of them, which is what an 800x600 screen needed (GitHub #38).
    foreach ($work in '800x600', '1024x768', '1280x720', '1366x768', '1920x1080') {
        $runs += , @("en-1-work$work", '1', '--work-area', $work, '--fixture', "`"$fixture`"")
    }
    $runs += , @('pl-1.5-work1920x1080', '1.5', '--lang', 'pl', '--work-area', '1920x1080', '--fixture', "`"$fixture`"")
    $failed = @()
    foreach ($r in $runs) {
        $dir = Join-Path $obj "render\$($r[0])"
        $renderArgs = @('--smoke-render', "`"$dir`"") + $r[1..($r.Count - 1)]
        if ($renderArgs -contains '--fixture' -and $renderArgs -notcontains '--board-fixture') {
            $renderArgs += @('--board-fixture', "`"$env:AMDGPU_WDDM_BOARD_FIXTURE`"")
        }
        # A build without art (-NagiArt '') cannot draw her, so every run of it renders the text-only panel: the
        # guide character is on by default, and a run with her on and no picture is a finding, by design.
        if (-not $artResourceNames.Count -and $renderArgs -notcontains '--no-nagi') { $renderArgs += '--no-nagi' }
        $p = Start-Process -FilePath $app -ArgumentList $renderArgs -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(120000)) { $p.Kill(); throw "render $($r[0]) did not exit within 120 s" }
        if ($p.ExitCode -ne 0) { $failed += "$($r[0]): $(Get-Content (Join-Path $dir 'layout.txt') -Raw -ErrorAction SilentlyContinue)" }
    }
    if ($failed.Count) { throw "render gates:`n$($failed -join "`n")" }
    Write-Host "  render: $($runs.Count) runs (9 pages, 96-192 DPI, 4 languages, text 150 %, Nagi on by default and two runs with her off, language switch, tuning states, 6 runs over five screen sizes from 800x600 up): no findings ($obj\render)"
}
# BD-166: the two native files of this build that the release ships carry their version resource from here. The
# stamp is the last step, after every gate above has run against the images this build linked.
& pwsh -NoProfile -File (Join-Path $repo 'tools\build\version-stamp.ps1') -Version $Version -Flavour $Flavour `
    "$Out\bc250control.dll" "$Out\bc250kmd_cli.exe" "$Out\amdgpu_wddm_control.exe"
if ($LASTEXITCODE -ne 0) { throw 'the version stamp of the control application files failed' }

Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
