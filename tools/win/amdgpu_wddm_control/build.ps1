# Builds the amdgpu-wddm control application: amdgpu_wddm_control.exe (.NET Framework 4.8, part of Windows 10/11,
# nothing to install on a tester's PC) and bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c with
# BC250_CONTROL_DLL), and bc250kmd_cli.exe from the same file for the release's tools folder. Compilers from the installed Visual Studio, headers and import libraries from the SDK NuGet
# packages under -Kits. Deterministic: the same sources give the same bytes (csc /deterministic, cl/link /Brepro).
#
#   pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\release\control-app\build\app
#        [-StartConfirmCore <release installer's start-confirm-core.ps1>] [-Oracle <directory of the review oracles>]
#
# Gates, in order, each stops the build: the unit tests (test/UnitTests.cs against driver/kmd/bc250kmd_escape.h, the
# KMD's interop, guard and DPM sources, the D3D12 shell's switch names, and with -StartConfirmCore the installer's
# confirmation rule), the DLL and app compiles with warnings as errors, a smoke run of the exe with --smoke (no
# window: the pages are built and refreshed once, their text written to smoke.txt), the bug report smoke, and the
# Recovery dry runs (--action X --dry-run: nothing is written, no UAC) against test/snapshot-bd059.json and this PC,
# the recovery view without bc250control.dll (--smoke-recovery), the hidden-window cost (--smoke-perf, G-PERF), and
# the render gates (G-RENDER, G-NOINT, G-A11Y): --smoke-render of the 8 pages at 96, 120, 144 and 192 DPI in the four
# languages, at text scale 150 %, with Nagi shown and hidden, and a language switch at run time; every page is drawn to
# PNG without a window and checked for overlap, overflow, internals in the text and accessible names.
# The smoke run passes on a PC without a BC-250 when it reports the driver as not found.
#
# -NagiArt <dir>: embeds the guide character (plan v7 section 6) from the owner's art directory as resources
# nagi.<expression>@128.png and @256.png (the map is Guide.ArtFile). The art is never committed; without -NagiArt the
# guide panel is text only, which is also what "Show Nagi" unchecked (the default) shows.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$StartConfirmCore,
    [string]$NagiArt,
    [string]$Oracle,
    [switch]$NoSmoke
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
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
    'LayoutRules.cs', 'UmaSetting.cs' |
    ForEach-Object { Join-Path $here "src\$_" }

# 1. Unit tests of the pure parts. Their temporary files go below the output directory, never to the system drive's
# temp folder. With -Oracle the review oracles (oracle-cu.json, oracle-ver.json, oracle-art.json) in that directory are
# checked too (test/OracleTests.cs); the oracles are read where they are and never copied into the repository.
$testTmp = Join-Path $obj 'tmp'
New-Item -ItemType Directory -Force $testTmp | Out-Null
$env:TEMP = $testTmp; $env:TMP = $testTmp
$env:AMDGPU_WDDM_ORACLE = $(if ($Oracle) { (Resolve-Path $Oracle).Path } else { '' })
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    "/out:$obj\unit-tests.exe" @pure (Get-ChildItem "$here\test\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
$unitArgs = @($repo) + @(if ($StartConfirmCore) { (Resolve-Path $StartConfirmCore).Path })
& "$obj\unit-tests.exe" @unitArgs | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }

# 2. bc250control.dll, the same translation unit and flags as tools\win\bc250kmd_cli\build.ps1.
$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/Brepro', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$obj\bc250control.obj", "/Fe$Out\bc250control.dll",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c'), '/link', '/Brepro', "/IMPLIB:$obj\bc250control.lib",
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|Creating library|bc250control.lib') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250control.dll build failed ($LASTEXITCODE)" }
Remove-Item "$Out\bc250control.exp", "$Out\bc250control.lib" -ErrorAction SilentlyContinue

# 2b. bc250kmd_cli.exe from the same translation unit and the same flags, without BC250_CONTROL_DLL: the release ships
# the CLI and the DLL of one build, so the CLI has every command the DLL's source has (tester.10 shipped a CLI of the
# KMD branch without health, clock, telemetry, vram and sdmaib). Without arguments it prints its usage and exits 2.
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$obj\bc250kmd_cli.obj", "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $repo 'tools\win\bc250kmd_cli\bc250kmd_cli.c'), '/link', '/Brepro',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250kmd_cli.exe build failed ($LASTEXITCODE)" }
$usage = & "$Out\bc250kmd_cli.exe" 2>&1 | ForEach-Object { [string]$_ }
if ($LASTEXITCODE -ne 2 -or -not ($usage -match '^usage: bc250kmd_cli ')) { throw "bc250kmd_cli.exe without arguments: exit $LASTEXITCODE, no usage" }
Write-Host "  bc250kmd_cli.exe usage: $(@($usage).Count) lines"

# 3. The application, with the string tables embedded.
# The lists are sorted: csc writes types and resources in input order, and a directory listing comes in the order of
# the file system, which the recipe does not own.
$resources = @(Get-ChildItem "$here\strings\strings.*.txt" | Sort-Object Name | ForEach-Object { "/resource:$($_.FullName),$($_.Name)" })
if ($NagiArt) {
    $guide = Get-Content (Join-Path $here 'src\Guide.cs') -Raw
    $map = [regex]::Matches($guide, '\{ "(?<expr>\d\d-[a-z-]+)", "(?<file>[a-z-]+)" \}')
    if ($map.Count -ne 9) { throw "Guide.ArtFile: $($map.Count) expressions found, 9 expected" }
    foreach ($m in $map) {
        foreach ($size in 128, 256) {
            $file = Join-Path $NagiArt "$($m.Groups['file'].Value)@$size.png"
            if (-not (Test-Path $file)) { throw "-NagiArt: $file missing" }
            $resources += "/resource:$file,nagi.$($m.Groups['expr'].Value)@$size.png"
        }
    }
    Write-Host "  guide art: 9 expressions x 2 sizes embedded from -NagiArt"
}
& $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
    "/win32manifest:$here\app.manifest" "/out:$Out\amdgpu_wddm_control.exe" @resources @((Get-ChildItem "$here\src\*.cs").FullName | Sort-Object)
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
# The exe declares its framework (A5: SecurityProtocolType.SystemDefault means the OS's TLS choice only for a 4.7+ target).
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$Out\amdgpu_wddm_control.exe"))
if (-not $image.Contains('.NETFramework,Version=v4.8')) { throw 'amdgpu_wddm_control.exe does not declare TargetFramework .NETFramework,Version=v4.8' }
Write-Host '  target framework: .NETFramework,Version=v4.8 declared'

# 4. Smoke run: no window, exits by itself. The DWM observations of the gates go to obj\state, not to this PC's profile.
$env:AMDGPU_WDDM_CONTROL_STATE = Join-Path $obj 'state'
Remove-Item -Recurse -Force $env:AMDGPU_WDDM_CONTROL_STATE -ErrorAction SilentlyContinue
if (-not $NoSmoke) {
    $smoke = Join-Path $obj 'smoke.txt'
    Remove-Item $smoke -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke', "`"$smoke`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'smoke run did not exit within 60 s' }
    $text = if (Test-Path $smoke) { Get-Content $smoke -Raw } else { '' }
    if ($p.ExitCode -ne 0 -or $text -notmatch '(?m)^Status: ') { throw "smoke run failed (exit $($p.ExitCode)): $text" }
    ($text -split "`r?`n" | Select-Object -First 4) | ForEach-Object { Write-Host "  smoke: $_" }

    # The report path: a zip with the fixed entries and without this PC's user or computer name.
    $zip = Join-Path $obj 'report-smoke.zip'
    Remove-Item $zip, "$zip.error.txt" -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-report', "`"$zip`"" -PassThru -WindowStyle Hidden
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

    # The Recovery dry runs: the plans of the exe itself, from the recorded BD-059 state and from this PC.
    function Invoke-DryRun([string[]]$ArgList, [string]$Name) {
        $file = Join-Path $obj "dry-$Name.txt"
        Remove-Item $file -ErrorAction SilentlyContinue
        $quoted = $ArgList | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }
        $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList (@($quoted) + @('--out', "`"$file`"")) -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "dry run $Name did not exit within 60 s" }
        [pscustomobject]@{ Code = $p.ExitCode; Text = $(if (Test-Path $file) { Get-Content $file -Raw } else { '' }) }
    }
    $snapshot = Join-Path $here 'test\snapshot-bd059.json'
    $params = 'HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
    $expect = [ordered]@{
        'reopen-gpu-path' = @(0, "set $params EnableGpuPresentBlit = 1 (DWord)", "delete $params InteropClosedReason")
        'desktop-gpu'     = @(3, 'refused: The GPU desktop path is closed for this start')
        'desktop-cpu'     = @(3, 'refused: The desktop already runs on the CPU route')
        'confirm-start'   = @(0, 'confirm this driver start', 'undo: no')
        'enable-dpm'      = @(0, "set $params DpmMode = 1 (DWord)", "set $params DpmMaxMHz = 1700 (DWord)")
        'reset-defaults'  = @(0, 'set HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter DwmForceCpu = 1 (DWord)')
        'undo'            = @(3, 'refused: There is no action to undo')
        'set-clocks'      = @(0, "delete $params DpmMaxMHz")
        'restart-compositor' = @(3, 'refused: restart-compositor is an operator escape', '--accept-bd060')
        # The CU setter (G-PLAN, plan v7 section 7): the dry run lists the helper's steps, in the helper's order.
        'cu-mode:40'      = @(0, "S1 set $params CuMode = 40 (DWord)", 'S4 flush the key', 'undo: no', 'never restored')
        'cu-mode:24'      = @(3, 'refused: Standard (24) is selected already')
        'cu-confirm'      = @(3, 'refused: No 40-core start is waiting for confirmation')
        'reset-defaults:keep' = @(0, 'note: Per-game settings are kept.')
        'game-undo'       = @(3, 'refused: There is no change of witcher3.exe to undo')
    }
    foreach ($e in $expect.GetEnumerator()) {
        $action = $e.Key.Split(':')[0]
        $extra = @(switch ($e.Key) { 'enable-dpm' { '--ceiling', '1700' } 'set-clocks' { '--mode', 'unset', '--ceiling', 'unset' }
            'cu-mode:40' { '--cu', '40' } 'cu-mode:24' { '--cu', '24' } 'reset-defaults:keep' { '--games', 'keep' } 'game-undo' { '--image', 'witcher3.exe' } })
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
        'cpu-disable' = @(0, "delete $params CpuTune", 'at the next restart of Windows')
        'cpu-readback' = @(0, 'tuning request cpu-readback')
        'cpu-trial'   = @(0, 'tuning request cpu-trial', 'undervolt 8 steps', 'window 120000 ms')
        'cpu-keep'    = @(3, 'refused: No processor trial is running.')
        'cpu-reset'   = @(3, 'refused: The processor has its default settings already.')
        'core-mask'   = @(0, 'tuning request core-mask, cores 8', 'at the next restart of Windows')
        # WU-042: "Reset driver settings" is the one control that puts the standard settings back, so with the
        # processor surface open it also turns processor tuning off. The tuning steps of a machine that has
        # settings stored are gated by the host tests (TunerStandardSteps); this snapshot has none stored.
        'reset-defaults' = @(0, "delete $params CpuTune", 'at the next restart of Windows')
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
            if (-not $r.Text.Contains($want)) { throw "dry run $($e.Key): '$want' missing: $($r.Text)" }
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
    # and a reading without the keys (the BD-059 snapshot has none); then the usage errors. The Vulkan names
    # (GraphicsSettings.AwaitingIcd) are not offered in this release, so --gfx refuses them as usage errors.
    $graphicsKey = 'HKLM\SOFTWARE\amdgpu-wddm\Graphics'
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
            @('graphics-defaults', '--gfx', 'WsiRoute=gdi'), @('game-profile', '--image', 'witcher3.exe', '--gfx', 'MemoryOverflow=strict'),
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
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--snapshot', $snapshot) 'snapshot-without-dry-run'
    if ($r.Code -ne 2) { throw "a recorded snapshot without --dry-run must be refused (exit $($r.Code))" }
    $r = Invoke-DryRun @('--action', 'reopen-gpu-path', '--dry-run') 'this-pc'
    if (($r.Code -ne 0 -and $r.Code -ne 3) -or -not $r.Text.Contains('dry run: nothing was written')) { throw "dry run on this PC failed (exit $($r.Code)): $($r.Text)" }
    $u = Invoke-DryRun @('--action', 'set-clocks', '--ceiling', '1500', '--dry-run', '--snapshot', $snapshot) 'set-clocks-without-mode'
    if ($u.Code -ne 2) { throw "set-clocks without --mode must be refused as usage (exit $($u.Code))" }
    foreach ($bad in @(@('cu-mode'), @('cu-mode', '--cu', '32'), @('reopen-gpu-path', '--cu', '40'), @('game-profile', '--image', 'C:\x.exe', '--value', ''))) {
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
            Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--status', '--out', "`"$(Join-Path $obj "status-race-$_.txt")`"" -PassThru -WindowStyle Hidden
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
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-recovery', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'recovery smoke did not exit within 60 s' }
    if ($p.ExitCode -ne 0) { throw "recovery smoke (exit $($p.ExitCode)): $(Get-Content $file -Raw -ErrorAction SilentlyContinue)" }
    Write-Host "  recovery view: built, bc250control.dll not loaded"
    $file = Join-Path $obj 'smoke-perf.txt'
    $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList '--smoke-perf', "`"$file`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill(); throw 'perf smoke did not exit within 60 s' }
    $perf = Get-Content $file -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "perf smoke (exit $($p.ExitCode)): $perf" }
    Write-Host "  hidden window: $((($perf -split "`r?`n") | Where-Object { $_ }) -join ', ')"

    # G-RENDER matrix: this PC's state and the recorded fixture (warnings, games, the CU section).
    $fixture = Join-Path $here 'test\snapshot-bd059.json'
    $runs = @()
    foreach ($lang in 'en', 'pl', 'ja', 'ko') {
        foreach ($scale in '1', '1.25', '1.5', '2') { $runs += , @("$lang-$scale", $scale, '--lang', $lang, '--fixture', "`"$fixture`"") }
    }
    $runs += , @('en-1-pc', '1')
    $runs += , @('pl-1-text150', '1', '--lang', 'pl', '--text-scale', '1.5', '--fixture', "`"$fixture`"")
    $runs += , @('ja-1.5-text150', '1.5', '--lang', 'ja', '--text-scale', '1.5', '--fixture', "`"$fixture`"")
    $runs += , @('en-1-nagi', '1', '--nagi', '--fixture', "`"$fixture`"")
    $runs += , @('ko-2-nagi', '2', '--lang', 'ko', '--nagi', '--fixture', "`"$fixture`"")
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
    $failed = @()
    foreach ($r in $runs) {
        $dir = Join-Path $obj "render\$($r[0])"
        $p = Start-Process -FilePath "$Out\amdgpu_wddm_control.exe" -ArgumentList (@('--smoke-render', "`"$dir`"") + $r[1..($r.Count - 1)]) -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(120000)) { $p.Kill(); throw "render $($r[0]) did not exit within 120 s" }
        if ($p.ExitCode -ne 0) { $failed += "$($r[0]): $(Get-Content (Join-Path $dir 'layout.txt') -Raw -ErrorAction SilentlyContinue)" }
    }
    if ($failed.Count) { throw "render gates:`n$($failed -join "`n")" }
    Write-Host "  render: $($runs.Count) runs (8 pages, 96-192 DPI, 4 languages, text 150 %, Nagi on/off, language switch, tuning states): no findings ($obj\render)"
}
Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
