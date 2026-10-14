# Builds the amdgpu-wddm setup window: amdgpu_wddm_setup.exe (.NET Framework 4.8, part of Windows 10/11, nothing to
# install on a tester's PC), with the string tables embedded. The compiler comes from the installed Visual Studio, as
# for the control app. Deterministic: the same sources give the same bytes (csc /deterministic).
#
#   pwsh tools\win\amdgpu_wddm_setup\build.ps1 -Out P:\BC-250\scratch\build\gui-setup\setup-app [-NoSmoke]
#
# Gates, in order, each stops the build:
#   1. unit tests (test/UnitTests.cs, test/StartTests.cs: the order of the start, test/WorkingTests.cs: what the
#      progress screen shows and which events change it): G-STR and G-NOINT over the tables, the engine contract against
#      tools/release/installer (every check id, stage and message id has words), the event model and result binding
#      (A2), the A3 texts, settings-impact lines, notes, prepared folders, guide ranks, arguments, quoting, and the
#      native TOKEN_PRIVILEGES layout of the planned restart;
#   2. the compile with warnings as errors, the .NET 4.8 target in the image, and the guide character's art: the
#      expressions this window's causes reach (SetupGuide.ArtExpressions, printed by the tests of step 1) at @256
#      (SetupGuide.ArtPixels; the smallest slot of this window is larger than the @128 picture) from
#      tools\win\amdgpu_wddm_control\art\nagi, under the names that application's Guide.ArtFile map gives them,
#      with the bytes its PROVENANCE.md records, every one of them in the built image;
#   3. --smoke-engine against a scripted fake engine (test/fake-engine.ps1): plan, noise, cancel, stale result, crash
#      without a result, restart, refusal, and a verify that found a driver which did not start (its diagnostics
#      file is named in the result and travels in the support file); the screen in four languages without internals;
#      the support file without this PC's user or computer name; then --smoke-start-failure: plan, install and
#      prepare with an engine that cannot start end on the result screen;
#   4. --smoke-render: every setup screen from fixtures at 96, 120, 144 and 192 DPI in EN, PL, JA, KO, at a 150 %
#      text size, without the guide character, with the high-contrast palette, and for six screen sizes from
#      800x600 up (--work-area, in logical units): no overlap, nothing outside the window, G-BACKDROP (BD-137: every
#      control that paints itself fills its whole client rectangle, which the picture cannot show because the parent
#      drew the pixels under it first, and clears itself in the colour the window really paints under it, which the
#      picture cannot show either because one flat colour over another leaves no overlap and no overflow),
#      G-A11Y (names, Tab reach,
#      tab order, Enter and Escape), G-CHROME, G-NOINT, G-SMALL, the owner's rule of 2026-10-12 that a 1024x768,
#      800x600 or smaller screen draws no right-hand panel and a larger one draws it, and G-CONSENT, the owner's
#      instruction of the same day that no screen asks the user for a permission: the plan screen carries no check
#      box or option button of its own and its Install button is never off. G-SHORTCUT holds the two shortcut
#      options of the same instruction: the plan screen offers the Start menu and the desktop, and both are on when
#      it opens.
#   4b. --smoke-loading: the loading window of the start in each of its stages, with the same layout and G-NOINT
#      checks, at four DPIs, four languages, three text sizes and two screens;
#   4c. --smoke-progress: an install run of 30 s and a plan run of 10 s played against the setup window: the progress
#      strip stays one control with one window handle, no engine event builds the screen again, and the strip is
#      drawn again only when its value changes or its band moves;
#   5. tools\release\test-setup-exe.ps1: the exe is packed around a small package of its own (the single-file
#      installer the release ships, owner 2026-10-12) and reads it back through --smoke-payload, and nine damaged, doctored
#      or empty exes are offered to the reader and every one of them is refused.
# None of them shows a window; nothing is installed and nothing outside -Out is written (the runs' temporary files are
# under -Out\obj).

param(
    [Parameter(Mandatory)][string]$Out,
    [switch]$NoSmoke
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
$exe = Join-Path $Out 'amdgpu_wddm_setup.exe'

$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.IO.Compression.dll',
    'System.IO.Compression.FileSystem.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/reference:$fx\$_" }
$pure = 'Strings.cs', 'Json.cs', 'EngineModel.cs', 'PlanText.cs', 'SetupArgs.cs', 'NoInternals.cs', 'Redactor.cs', 'SmallScreen.cs', 'InstallFlow.cs', 'Payload.cs' | ForEach-Object { Join-Path $here "src\$_" }
# The order of the start: the loading window before every slow step (LoadingWindow.cs).
$pure += 'StartSequence.cs' | ForEach-Object { Join-Path $here "src\$_" }
# What the progress screen shows, and which engine events change it in place (WorkingView.cs).
$pure += 'WorkingView.cs' | ForEach-Object { Join-Path $here "src\$_" }

# 1. Unit tests of the pure parts.
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    "/out:$obj\unit-tests.exe" @pure (Join-Path $here 'src\Native.cs') (Join-Path $here 'test\UnitTests.cs') (Join-Path $here 'test\StartTests.cs') (Join-Path $here 'test\WorkingTests.cs')
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
$unit = @(& "$obj\unit-tests.exe" $repo (Join-Path $here 'strings') | ForEach-Object { [string]$_ })
$unitFailed = $LASTEXITCODE -ne 0
$unit | ForEach-Object { Write-Host "  $_" }
if ($unitFailed) { throw 'unit tests failed' }
# The expressions the window can ask for, from the program's own code: SetupGuide.ArtExpressions, printed by the
# tests that also check it against every SetupCause. The build embeds these and no more.
$expressionLine = @($unit | Where-Object { $_ -match '^guide art expressions: ' })
if ($expressionLine.Count -ne 1) { throw 'the unit tests did not print the one line with the guide art expressions' }
$setupExpressions = @(($expressionLine[0] -replace '^guide art expressions: ', '').Trim() -split '\s+' | Where-Object { $_ })
if (-not $setupExpressions.Count) { throw 'the guide art expressions line is empty' }

# 2. The application, with the string tables embedded.
# Both lists are sorted: csc writes types and resources in input order, and a directory listing comes in the order of
# the file system, which the recipe does not own.
$resources = @(Get-ChildItem "$here\strings\strings.*.txt" | Sort-Object Name | ForEach-Object { "/resource:$($_.FullName),$($_.Name)" })
# The guide character: the same art directory, the same expression map and the same byte check as the control
# application (its art\check-art.ps1), so one map serves both programs and the setup window shows her on its own
# first screen. Only the expressions this window's causes reach are embedded; the built image is refused below
# unless it carries every one of them.
$artDir = Join-Path $repo 'tools\win\amdgpu_wddm_control\art\nagi'
$artResourceNames = @()
foreach ($a in & (Join-Path $repo 'tools\win\amdgpu_wddm_control\art\check-art.ps1') -ArtDir $artDir `
        -GuideSource (Join-Path $repo 'tools\win\amdgpu_wddm_control\src\Guide.cs') -Expressions $setupExpressions -Sizes 256) {
    $resources += "/resource:$($a.Path),$($a.Resource)"
    $artResourceNames += $a.Resource
}
Write-Host "  guide art: $($setupExpressions.Count) expressions ($($setupExpressions -join ', ')) at @256 embedded from $artDir"
# The application icon (owner, 2026-10-12: "ikonke zrob z biustu Nagi"): the guide character's bust as a round badge,
# art\icon\nagi.ico with its PROVENANCE.md, the same file the control application uses. It is the window's icon, the
# icon of the single-file installer that carries this exe, and the icon the Start menu and desktop shortcuts show.
$icon = Join-Path $repo 'tools\win\amdgpu_wddm_control\art\icon\nagi.ico'
if (-not (Test-Path -LiteralPath $icon -PathType Leaf)) { throw "application icon: no $icon" }
# The same file as a resource as well (SetWindowIcon): the window's icon is then the authored 16, 20, 24, ... 256
# image for the size Windows asks for, instead of one 32x32 image rescaled on a high-resolution screen.
$resources += "/resource:$icon,nagi.ico"
& $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
    "/win32manifest:$here\app.manifest" "/win32icon:$icon" "/out:$exe" @resources @((Get-ChildItem "$here\src\*.cs").FullName | Sort-Object)
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
if (-not $image.Contains('.NETFramework,Version=v4.8')) { throw 'amdgpu_wddm_setup.exe does not declare TargetFramework .NETFramework,Version=v4.8' }
Write-Host '  target framework: .NETFramework,Version=v4.8 declared'
$missingArt = @($artResourceNames | Where-Object { -not $image.Contains($_) })
if ($missingArt.Count) { throw "guide art: the built exe has no resource $($missingArt -join ', ')" }
if (-not $image.Contains('nagi.ico')) { throw 'application icon: the built exe has no resource nagi.ico' }
Write-Host '  application icon: nagi.ico as the Win32 icon and as a resource with every authored size'
Write-Host "  guide art: $($artResourceNames.Count) resources in the image"

if (-not $NoSmoke) {
    # A package folder for the headless runs: the fake engine as installer\install.ps1, a manifest and the newest notes.
    $fake = Join-Path $obj 'fake-package'
    Remove-Item -Recurse -Force $fake -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $fake 'installer') | Out-Null
    Copy-Item (Join-Path $here 'test\fake-engine.ps1') (Join-Path $fake 'installer\install.ps1')
    [IO.File]::WriteAllText((Join-Path $fake 'manifest.json'), '{"name":"amdgpu-wddm-tester-9.9.9-fake","version":"9.9.9-fake"}')
    $notes = Get-ChildItem (Join-Path $repo 'docs\testing\release-notes') -Filter '*.md' | Sort-Object Name | Select-Object -Last 1
    Copy-Item $notes.FullName (Join-Path $fake 'RELEASE-NOTES.md')

    function Invoke-Exe([string[]]$ArgList, [int]$Seconds = 120) {
        $quoted = $ArgList | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ } }
        $p = Start-Process -FilePath $exe -ArgumentList $quoted -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); throw "amdgpu_wddm_setup.exe $($ArgList[0]) did not exit within $Seconds s" }
        $p.ExitCode
    }

    # 3. The engine client against the scripted engine.
    $expect = [ordered]@{
        # The fake engine still reports a consent in its decision, because the model has to go on reading the
        # field of the engine contract. The window shows no control for it any more (G-CONSENT): what the install
        # changes is stated on the plan screen, and pressing Install is the consent (owner, 2026-10-12).
        plan    = @('--plan', '-Scenario', 'plan'), @('result: bound', 'outcome: planned', 'view: Plan', 'decision: install restarts 2 consents test-signing', 'settings: rows 3 lines 3', 'unknown checks: future.check', 'events: 9 ignored 0 problems 0')
        noise   = @('--plan', '-Scenario', 'noise'), @('result: bound', 'outcome: planned', 'ignored 2')
        cancel  = @('--cancel', '-Scenario', 'cancel'), @('result: bound', 'outcome: cancelled', 'cancel: requested', 'view: Cancelled result.cancelled.title nothing-changed')
        stale   = @('-Scenario', 'stale'), @('result: none (the result belongs to another run', 'view: Problem result.none.title changes-unknown offer-retry')
        crash   = @('-Scenario', 'crash'), @('exit: 6', 'result: none', 'view: Problem result.none.title changes-made changes-unknown')
        restart = @('-Scenario', 'restart'), @('outcome: restart-required', 'restart: restart.test-signing', 'view: Restart result.restart-test-signing.title changes-made offer-restart', 'restart event restart.test-signing')
        refused = @('-Scenario', 'refused'), @('outcome: refused', 'view: Problem result.preflight-refused.title nothing-changed offer-retry retry-secondary', 'No ASRock BC-250 graphics chip was found')
        # A verify that found a driver which did not start: the engine's own diagnostics file is named in the
        # result, the screen says the details were collected, and the support file carries them (below).
        diagnostics = @('-Scenario', 'verify-failed'), @('outcome: verify-failed', 'view: Problem result.verify-failed.title nothing-changed', 'The details of the graphics chip that did not start were collected for you')
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($e in $expect.GetEnumerator()) {
        $dir = Join-Path $obj "engine-$($e.Key)"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        $flags = @($e.Value[0] | Where-Object { $_ -like '--*' })
        $engineArgs = @($e.Value[0] | Where-Object { $_ -notlike '--*' })
        $code = Invoke-Exe (@('--smoke-engine', $fake, $dir) + $flags + @('--') + $engineArgs)
        $summary = Get-Content (Join-Path $dir 'summary.txt') -Raw -ErrorAction SilentlyContinue
        if ($code -ne 0) { throw "smoke-engine $($e.Key): exit $code`n$summary" }
        foreach ($want in $e.Value[1]) { if (-not $summary.Contains($want)) { throw "smoke-engine $($e.Key): '$want' missing:`n$summary" } }
        foreach ($lang in 'en', 'pl', 'ja', 'ko') {
            if ($summary -notmatch "(?m)^screen ${lang}: \w+; internals none; missing strings none") { throw "smoke-engine $($e.Key): the $lang screen has internals or missing strings:`n$summary" }
        }
        $zip = Join-Path $dir 'support.zip'
        if (-not (Test-Path $zip)) { throw "smoke-engine $($e.Key): no support file" }
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $names = @($archive.Entries | ForEach-Object Name)
            foreach ($want in 'setup.txt', 'run-1-command.txt', 'run-1-engine-output.txt') { if ($names -notcontains $want) { throw "smoke-engine $($e.Key): support file without $want" } }
            # The engine's automatic diagnostics of a failed start travel in the support file as run-N-diagnostics.txt,
            # and only for a run that named one.
            if ($e.Key -eq 'diagnostics' -and $names -notcontains 'run-1-diagnostics.txt') { throw "smoke-engine $($e.Key): support file without run-1-diagnostics.txt" }
            if ($e.Key -ne 'diagnostics' -and $names -contains 'run-1-diagnostics.txt') { throw "smoke-engine $($e.Key): a run that named no diagnostics file has one in the support file" }
            foreach ($entry in $archive.Entries) {
                $reader = New-Object IO.StreamReader($entry.Open())
                $body = $reader.ReadToEnd(); $reader.Dispose()
                foreach ($secret in $env:USERNAME, $env:COMPUTERNAME) {
                    if ($secret.Length -ge 2 -and $body -match "(?<![A-Za-z0-9])$([regex]::Escape($secret))(?![A-Za-z0-9])") { throw "smoke-engine $($e.Key): $($entry.Name) names this PC's user or computer" }
                }
            }
            # The new entry, against the promise of docs/testing/INSTALL.md: no serial number and no machine id
            # of a line that names one, whatever prefix that line carries, and the hardware id and the decision
            # still there, because a report without them is useless.
            if ($e.Key -eq 'diagnostics') {
                $entry = @($archive.Entries | Where-Object { $_.Name -eq 'run-1-diagnostics.txt' })[0]
                $reader = New-Object IO.StreamReader($entry.Open())
                $body = $reader.ReadToEnd(); $reader.Dispose()
                foreach ($secret in 'SN-ABC123456789', '11111111-2222-3333-4444-555555555555',
                    'otherperson', 'S-1-5-21-3623811015-3361044348-30300820-1013', 'SOMEBODY-ELSE-PC') {
                    if ($body.Contains($secret)) { throw "smoke-engine diagnostics: run-1-diagnostics.txt still carries $secret" }
                }
                foreach ($keep in 'Device serial: <redacted>', 'MachineGuid: <redacted>', 'PCI\VEN_1002&DEV_13FE', 'problem code 43',
                    'C:\Users\<user>\Downloads', '<sid>', '<Computer><computer></Computer>') {
                    if (-not $body.Contains($keep)) { throw "smoke-engine diagnostics: run-1-diagnostics.txt without '$keep'" }
                }
            }
        } finally { $archive.Dispose() }
        Write-Host "  smoke-engine $($e.Key): $((($summary -split "`r?`n") | Where-Object { $_ -like 'view:*' }) -join '')"
    }

    # 3b. An engine that cannot start (run folder, package folder) for plan, install and prepare: the result screen stays.
    $dir = Join-Path $obj 'start-failure'
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $code = Invoke-Exe @('--smoke-start-failure', $dir)
    $summary = Get-Content (Join-Path $dir 'summary.txt') -Raw -ErrorAction SilentlyContinue
    if ($code -ne 0 -or $summary -notmatch '(?m)^ok\r?$' -or ([regex]::Matches($summary, '(?m)^\w+ [\w-]+: Result Problem\r?$')).Count -ne 6) { throw "smoke-start-failure (exit $code):`n$summary" }
    Write-Host '  start failure: plan, install and prepare with a run folder or a package folder that cannot be used end on the result screen (6 cases)'

    # 4. Every screen at four DPIs and four languages, and at a 150 % text size. Every run draws the guide character,
    # because she is on unless the control app's setting says 0; the two last runs draw the panel without her.
    $renders = @(foreach ($scale in '1', '1.25', '1.5', '2') { foreach ($lang in 'en', 'pl', 'ja', 'ko') { , @($scale, $lang, '1', '') } })
    $renders += , @('1', 'en', '1.5', '')
    $renders += , @('1', 'ja', '1.5', '')
    $renders += , @('1', 'en', '1', '--no-nagi')
    $renders += , @('2', 'ko', '1', '--no-nagi')
    # High contrast: this PC does not have it on, so the window is told to use the system palette. Without this run
    # nothing would see a tint drawn on a tint or text that the high-contrast colours make unreadable.
    $renders += , @('1', 'en', '1', '--high-contrast')
    $renders += , @('1.5', 'pl', '1', '--high-contrast')
    # G-SMALL, the owner's rule of 2026-10-12: five working areas of a tester's screen, in logical units. The four
    # small ones must draw no right-hand panel and the 1920x1080 one must draw it; overlap, overflow and the page
    # column are checked in every one of them, which is what the tester's 800x600 screen needed (GitHub #38).
    foreach ($work in '800x600', '1024x768', '1280x720', '1366x768', '1920x1080') {
        $renders += , @('1', 'en', '1', '--work-area', $work)
    }
    $renders += , @('1.5', 'pl', '1', '--work-area', '1920x1080')
    foreach ($r in $renders) {
        $extra = @($r[3..($r.Count - 1)] | Where-Object { $_ })
        $dir = Join-Path $obj "render-$($r[0])-$($r[1])-text$($r[2])$(if ($extra.Count) { '-' + (($extra -replace '^--', '') -join '-') })"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        $renderArgs = @('--smoke-render', $dir, $r[0], $r[1], '--text-scale', $r[2]) + $extra + @('--package', $fake)
        $code = Invoke-Exe $renderArgs 300
        $layout = Get-Content (Join-Path $dir 'layout.txt') -Raw -ErrorAction SilentlyContinue
        if ($code -ne 0 -or $layout -notmatch '^ok: ') { throw "render at scale $($r[0]), $($r[1]), text $($r[2]) $($extra -join ' ') (exit $code):`n$layout" }
    }
    Write-Host "  render: $(@(Get-ChildItem (Join-Path $obj 'render-1-en-text1') -Filter *.png).Count) screens x $($renders.Count) (4 DPIs x 4 languages + 150 % text + 2 without the guide character + 2 in high contrast + 6 runs over five screen sizes from 800x600 up), no overlap or overflow, the guide art and the right-hand panel where they belong, G-A11Y, G-CHROME, G-SMALL, G-CONSENT, G-SHORTCUT and G-NOINT clean ($obj\render-*)"

    # 4b. The loading window of the start (LoadingWindow.cs) in each of its stages, four languages, four scales, three
    #     text sizes and a 1024x768 and a large screen: it fits or scrolls, nothing overlaps or overflows, no internals,
    #     every text in the tables, the guide's picture where the exe carries it.
    $dir = Join-Path $obj 'loading'
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $code = Invoke-Exe @('--smoke-loading', $dir) 300
    $loading = Get-Content (Join-Path $dir 'loading.txt') -Raw -ErrorAction SilentlyContinue
    if ($code -ne 0 -or $loading -notmatch '^ok: ') { throw "smoke-loading (exit $code):`n$loading" }
    Write-Host "  loading window: $((($loading -split "`r?`n") | Where-Object { $_ }) -join ', ')"

    # 4c. The progress screen under engine events (SetupForm.ProgressGate): the strip is never made again, never
    #     gets a second window handle, and is drawn again only when its value changes or its band moves.
    $dir = Join-Path $obj 'progress'
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $code = Invoke-Exe @('--smoke-progress', $dir) 300
    $progress = Get-Content (Join-Path $dir 'progress.txt') -Raw -ErrorAction SilentlyContinue
    if ($code -ne 0 -or $progress -notmatch '(?m)^ok\r?$') { throw "smoke-progress (exit $code):`n$progress" }
    (($progress -split "`r?`n") | Where-Object { $_ -and $_ -ne 'ok' }) | ForEach-Object { Write-Host "  progress: $_" }

    # 5. The single-file installer (Payload.cs): the exe just built is packed around a small package of its own and
    #    read back through its --smoke-payload mode, then nine damaged, doctored or empty exes are offered to it and every one
    #    of them must be refused. This runs here, with the exe in hand, so a reader that would unpack a doctored
    #    package cannot reach a release.
    $one = @(& (Join-Path $repo 'tools\release\test-setup-exe.ps1') -Setup $exe -Out (Join-Path $obj 'setup-exe') |
        ForEach-Object { [string]$_ })
    $oneOk = @($one | Where-Object { $_ -match '^single-file installer: \d+ checks passed, 0 failed$' })
    if ($oneOk.Count -ne 1) { throw "the single-file installer test did not pass:`n$($one -join "`n")" }
    Write-Host "  $($oneOk[0]) (the payload read, checked and unpacked; 9 damaged, doctored or empty exes refused)"
}

Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
