# The negative controls of BD-163 (the empty %ProgramData%\amdgpu-wddm\setup that a single-file setup left behind).
# The unit tests (test/UnitTests.cs) check the cleanup in folders of their own, never in %ProgramData%. This script
# shows that those checks see the defect: it compiles the unit tests three times and runs each build.
#   1. the sources as they are: every check passes.
#   2. Payload.Cleanup without its RemoveRootIfEmpty call (the code before 0fdd8179): the check "Cleanup removes the
#      setup root with the last version folder" must fail.
#   3. Access.RootRules without the entry of the creating account (the permission list before 0fdd8179): the check
#      "the root carries the account that created it" must fail.
# Nothing here touches %ProgramData% or needs administrator rights, so a leftover folder on the building PC does not
# hide a regression (test-dryrun.ps1 can only name such a folder).
#   pwsh -NoProfile -File tools\win\amdgpu_wddm_setup\test-bd163-controls.ps1 -Out <dir>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
# The same references and sources as step 1 of build.ps1.
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.IO.Compression.dll',
    'System.IO.Compression.FileSystem.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/reference:$fx\$_" }
$names = 'Strings.cs', 'Json.cs', 'EngineModel.cs', 'PlanText.cs', 'SetupArgs.cs', 'NoInternals.cs', 'Redactor.cs', 'SmallScreen.cs',
    'InstallFlow.cs', 'Payload.cs', 'HelpCards.cs', 'StartSequence.cs', 'WorkingView.cs', 'Native.cs'
$tests = 'UnitTests.cs', 'StartTests.cs', 'WorkingTests.cs' | ForEach-Object { Join-Path $here "test\$_" }
$payload = Get-Content -LiteralPath (Join-Path $here 'src\Payload.cs') -Raw

# Each mutation replaces exactly one text of Payload.cs, so a change of that code stops this script instead of
# testing nothing.
$cleanupOld = "                Remove(dir);`r`n                RemoveRootIfEmpty(root);`r`n            }`r`n            catch (Exception) { }`r`n        }`r`n`r`n        // The same removal from the release version alone"
$rootOld = '        public static DirectorySecurity RootRules()' + "`r`n" + '            {' + "`r`n" + '                var s = Rules();'
$cases = @(
    @{ Name = 'as-is'; From = $null; To = $null; Fail = $null },
    @{ Name = 'cleanup-keeps-root'; From = $cleanupOld; To = $cleanupOld.Replace("                RemoveRootIfEmpty(root);`r`n", '');
       Fail = 'single file: Cleanup removes the setup root with the last version folder' },
    # Without that entry this run may not create the version folder in the root it made (the second half of BD-163),
    # so the unit tests stop there with access denied, before they reach the check of the entry itself.
    @{ Name = 'root-without-creator'; From = $rootOld; To = $rootOld.Replace('var s = Rules();', 'var s = Rules(); return s;');
       Fail = @('single file: the root carries the account that created it', 'FAIL exception: System.UnauthorizedAccessException') }
)
# Payload.cs in the tree may have LF line ends (git autocrlf off); the anchors then use LF.
if (-not $payload.Contains("`r`n")) { foreach ($c in $cases) { if ($c.From) { $c.From = $c.From.Replace("`r`n", "`n"); $c.To = $c.To.Replace("`r`n", "`n") } } }
$failures = 0
foreach ($c in $cases) {
    $dir = Join-Path $Out $c.Name
    New-Item -ItemType Directory -Force $dir | Out-Null
    $text = $payload
    if ($c.From) {
        $count = ([regex]::Matches($payload, [regex]::Escape($c.From))).Count
        if ($count -ne 1) { throw "$($c.Name): the anchor occurs $count times in Payload.cs, not once: update this control" }
        $text = $payload.Replace($c.From, $c.To)
    }
    $mutated = Join-Path $dir 'Payload.cs'
    [IO.File]::WriteAllText($mutated, $text)
    $sources = @($names | ForEach-Object { if ($_ -eq 'Payload.cs') { $mutated } else { Join-Path $here "src\$_" } })
    # The mutation of RootRules leaves a statement after the return; csc reports it (CS0162), which is not an error here.
    & $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /warnaserror+ /nowarn:0162 /langversion:7.3 /deterministic+ `
        /define:AMDGPU_WDDM_LAB_FLAVOUR "/out:$dir\unit-tests.exe" @sources @tests | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "$($c.Name): unit test compile failed ($LASTEXITCODE)" }
    # The tests make their folders under TEMP: keep them inside the output folder of this script.
    $savedTemp = $env:TEMP, $env:TMP
    $env:TEMP = New-Item -ItemType Directory -Force (Join-Path $dir 'tmp') | ForEach-Object FullName; $env:TMP = $env:TEMP
    try { $lines = @(& "$dir\unit-tests.exe" $repo (Join-Path $here 'strings') 2>&1 | ForEach-Object { [string]$_ }); $code = $LASTEXITCODE }
    finally {
        $env:TEMP, $env:TMP = $savedTemp
        # A mutated run can leave a folder with the setup's permission list, which this account may not delete. It
        # owns the folder, so it may give itself the right first.
        $left = Join-Path $dir 'tmp'
        & icacls.exe $left /grant "*$([Security.Principal.WindowsIdentity]::GetCurrent().User.Value):(OI)(CI)F" /T /C /Q | Out-Null
        Remove-Item -LiteralPath $left -Recurse -Force -ErrorAction SilentlyContinue
    }
    $lines | Set-Content -LiteralPath (Join-Path $dir 'unit-tests.txt') -Encoding utf8
    $failLines = @($lines | Where-Object { $_ -match '^FAIL' })
    if (-not $c.Fail) {
        if ($code -eq 0 -and $failLines.Count -eq 0) { "PASS $($c.Name): every unit test passes" }
        else { "FAIL $($c.Name): exit $code, $($failLines.Count) failed checks"; $failLines | Select-Object -First 5; $failures++ }
    } else {
        $hit = @($failLines | Where-Object { $line = $_; @($c.Fail | Where-Object { $line.Contains($_) }).Count })
        if ($code -ne 0 -and $hit.Count -ge 1) { "PASS $($c.Name): the mutation fails '$($hit[0].Substring(0, [Math]::Min(110, $hit[0].Length)))' ($($failLines.Count) failed checks, exit $code)" }
        else { "FAIL $($c.Name): the mutation was not caught (exit $code, $($failLines.Count) failed checks)"; $failLines | Select-Object -First 5; $failures++ }
    }
}
if ($failures) { throw "$failures BD-163 controls failed" }
'PASS: 3 BD-163 controls'
