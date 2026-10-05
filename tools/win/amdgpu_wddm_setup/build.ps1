# Builds the amdgpu-wddm setup window: amdgpu_wddm_setup.exe (.NET Framework 4.8, part of Windows 10/11, nothing to
# install on a tester's PC), with the string tables embedded. The compiler comes from the installed Visual Studio, as
# for the control app. Deterministic: the same sources give the same bytes (csc /deterministic).
#
#   pwsh tools\win\amdgpu_wddm_setup\build.ps1 -Out P:\BC-250\scratch\build\gui-setup\setup-app [-NoSmoke]
#
# Gates, in order, each stops the build:
#   1. unit tests (test/UnitTests.cs): G-STR and G-NOINT over the tables, the engine contract against
#      tools/release/installer (every check id, stage and message id has words), the event model and result binding
#      (A2), the A3 texts, settings-impact lines, notes, prepared folders, guide ranks, arguments, quoting, and the
#      native TOKEN_PRIVILEGES layout of the planned restart;
#   2. the compile with warnings as errors, and the .NET 4.8 target in the image;
#   3. --smoke-engine against a scripted fake engine (test/fake-engine.ps1): plan, noise, cancel, stale result, crash
#      without a result, restart, refusal; the screen in four languages without internals; the support file without
#      this PC's user or computer name; then --smoke-start-failure: plan, install and prepare with an engine that
#      cannot start end on the result screen;
#   4. --smoke-render: every setup screen from fixtures at 96, 120, 144 and 192 DPI in EN, PL, JA, KO, and at a 150 %
#      text size: no overlap, nothing outside the window, G-A11Y (names, Tab reach, Enter and Escape) and G-NOINT.
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
$pure = 'Strings.cs', 'Json.cs', 'EngineModel.cs', 'PlanText.cs', 'SetupArgs.cs', 'NoInternals.cs' | ForEach-Object { Join-Path $here "src\$_" }

# 1. Unit tests of the pure parts.
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /langversion:7.3 /deterministic+ `
    "/out:$obj\unit-tests.exe" @pure (Join-Path $here 'src\Native.cs') (Join-Path $here 'test\UnitTests.cs')
if ($LASTEXITCODE -ne 0) { throw "unit test compile failed ($LASTEXITCODE)" }
& "$obj\unit-tests.exe" $repo (Join-Path $here 'strings') | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }

# 2. The application, with the string tables embedded.
$resources = @(Get-ChildItem "$here\strings\strings.*.txt" | ForEach-Object { "/resource:$($_.FullName),$($_.Name)" })
& $csc /nologo /noconfig /nostdlib+ @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 /deterministic+ `
    "/win32manifest:$here\app.manifest" "/out:$exe" @resources (Get-ChildItem "$here\src\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
if (-not $image.Contains('.NETFramework,Version=v4.8')) { throw 'amdgpu_wddm_setup.exe does not declare TargetFramework .NETFramework,Version=v4.8' }
Write-Host '  target framework: .NETFramework,Version=v4.8 declared'

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
        plan    = @('--plan', '-Scenario', 'plan'), @('result: bound', 'outcome: planned', 'view: Plan', 'decision: install restarts 2 consents test-signing', 'settings: rows 3 lines 3', 'unknown checks: future.check', 'events: 9 ignored 0 problems 0')
        noise   = @('--plan', '-Scenario', 'noise'), @('result: bound', 'outcome: planned', 'ignored 2')
        cancel  = @('--cancel', '-Scenario', 'cancel'), @('result: bound', 'outcome: cancelled', 'cancel: requested', 'view: Cancelled result.cancelled.title nothing-changed')
        stale   = @('-Scenario', 'stale'), @('result: none (the result belongs to another run', 'view: Problem result.none.title changes-unknown offer-retry')
        crash   = @('-Scenario', 'crash'), @('exit: 6', 'result: none', 'view: Problem result.none.title changes-made changes-unknown')
        restart = @('-Scenario', 'restart'), @('outcome: restart-required', 'restart: restart.test-signing', 'view: Restart result.restart-test-signing.title changes-made offer-restart', 'restart event restart.test-signing')
        refused = @('-Scenario', 'refused'), @('outcome: refused', 'view: Problem result.preflight-refused.title nothing-changed offer-retry retry-secondary', 'No ASRock BC-250 graphics chip was found')
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
            foreach ($entry in $archive.Entries) {
                $reader = New-Object IO.StreamReader($entry.Open())
                $body = $reader.ReadToEnd(); $reader.Dispose()
                foreach ($secret in $env:USERNAME, $env:COMPUTERNAME) {
                    if ($secret.Length -ge 2 -and $body -match "(?<![A-Za-z0-9])$([regex]::Escape($secret))(?![A-Za-z0-9])") { throw "smoke-engine $($e.Key): $($entry.Name) names this PC's user or computer" }
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

    # 4. Every screen at four DPIs and four languages, and at a 150 % text size.
    $renders = @(foreach ($scale in '1', '1.25', '1.5', '2') { foreach ($lang in 'en', 'pl', 'ja', 'ko') { , @($scale, $lang, '1') } })
    $renders += , @('1', 'en', '1.5')
    $renders += , @('1', 'ja', '1.5')
    foreach ($r in $renders) {
        $dir = Join-Path $obj "render-$($r[0])-$($r[1])-text$($r[2])"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        $code = Invoke-Exe @('--smoke-render', $dir, $r[0], $r[1], '--text-scale', $r[2], '--package', $fake) 300
        $layout = Get-Content (Join-Path $dir 'layout.txt') -Raw -ErrorAction SilentlyContinue
        if ($code -ne 0 -or $layout -notmatch '^ok: ') { throw "render at scale $($r[0]), $($r[1]), text $($r[2]) (exit $code):`n$layout" }
    }
    Write-Host "  render: $(@(Get-ChildItem (Join-Path $obj 'render-1-en-text1') -Filter *.png).Count) screens x $($renders.Count) (4 DPIs x 4 languages + 150 % text), no overlap or overflow, G-A11Y and G-NOINT clean ($obj\render-*)"
}

Get-ChildItem $Out -File | ForEach-Object {
    '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8), $_.Name
}
