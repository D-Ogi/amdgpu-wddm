# Console installer regressions, under Windows PowerShell 5.1. No registry, network, UAC or real installation.
# Each child has redirected streams and a time bound. The launcher runs a synthetic engine inside WorkRoot.
param([Parameter(Mandatory)][string]$Installer, [Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $WorkRoot) { throw 'WorkRoot must be a new private directory' }
[void][IO.Directory]::CreateDirectory($WorkRoot)
$fail = 0
function Check([bool]$Ok, [string]$Text) {
    if ($Ok) { "PASS $Text" } else { "FAIL $Text"; $script:fail++ }
}
function Run-Child([string]$Exe, [string]$Arguments, [string]$InputText, [bool]$HoldInput = $false) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    try {
        $out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
        if ($InputText) { $p.StandardInput.Write($InputText); $p.StandardInput.Flush() }
        if (-not $HoldInput) { $p.StandardInput.Close() }
        $timedOut = -not $p.WaitForExit(5000)
        if ($timedOut) { $p.Kill(); $p.WaitForExit() }
        return [pscustomobject]@{ code = $p.ExitCode; timeout = $timedOut; text = $out.Result + $err.Result }
    } finally { $p.Dispose() }
}
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$reader = Join-Path $WorkRoot 'reader.ps1'
[IO.File]::WriteAllText($reader, @'
param([string]$Installer, [string]$Mode)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'components.ps1')
$script:GuiMode = ($Mode -eq 'gui')
$script:DryRunMode = ($Mode -eq 'dry')
$a = Read-YesNo -Question 'First?' -Default $false
$b = Read-YesNo -Question 'Second?' -Default $true
"ANSWERS=$([int]$a),$([int]$b)"
'@)
$argsBase = '-NoProfile -ExecutionPolicy Bypass -File "' + $reader + '" -Installer "' + $Installer + '"'
foreach ($c in @(
    @{ name = 'closed stdin uses both defaults'; text = ''; extra = ''; answers = '0,1' },
    @{ name = 'NonInteractive uses both defaults'; text = ''; extra = ''; noninteractive = $true; answers = '0,1' },
    @{ name = 'answers ignore whitespace and case'; text = " YES `r`nno`r`n"; extra = ''; answers = '1,0' },
    @{ name = 'invalid answers are bounded before default'; text = "one`r`ntwo`r`nthree`r`nno`r`n"; extra = ''; answers = '0,0' },
    @{ name = 'setup window does not consent through defaults'; text = ''; extra = ' -Mode gui'; answers = '0,0' },
    @{ name = 'dry run does not prompt'; text = ''; extra = ' -Mode dry'; answers = '0,1' }
)) {
    $a = $argsBase + $c.extra
    if ($c.noninteractive) { $a = '-NonInteractive ' + $a }
    $r = Run-Child $ps51 $a $c.text
    Check (-not $r.timeout -and $r.code -eq 0 -and $r.text.Contains('ANSWERS=' + $c.answers)) $c.name
    if ($r.code -ne 0) { $r.text }
}

$package = Join-Path $WorkRoot 'synthetic package'
[void][IO.Directory]::CreateDirectory((Join-Path $package 'installer'))
$launcher = Join-Path (Split-Path $Installer -Parent) 'install.cmd'
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) { $launcher = Join-Path $Installer 'install.cmd' }
Copy-Item -LiteralPath $launcher -Destination $package
[IO.File]::WriteAllText((Join-Path $package 'installer\install.ps1'), @'
param([int]$TestExit, [string]$Payload, [switch]$NoReboot, [switch]$Force)
$ErrorActionPreference = 'Stop'
if ($Payload -ne 'quoted path with spaces' -or -not $NoReboot -or -not $Force -or $args.Count) { exit 97 }
Write-Output 'SYNTHETIC_ENGINE_OK'
exit $TestExit
'@)
foreach ($code in @(0, 6, 10)) {
    # Keep stdin open: a stray pause must time out, rather than succeed because it sees EOF.
    $arguments = '/d /s /c ""' + (Join-Path $package 'install.cmd') + '" -TestExit ' + $code +
        ' -cOnSoLe -Payload "quoted path with spaces" -NoReboot -Force"'
    $r = Run-Child (Join-Path $env:SystemRoot 'System32\cmd.exe') $arguments '' $true
    $want = $code
    if ($code -eq 10) { $want = 0 }
    Check (-not $r.timeout -and $r.code -eq $want -and $r.text.Contains('SYNTHETIC_ENGINE_OK')) "console exits without a key, preserves arguments and maps exit $code to $want"
}
if ($fail) { throw "$fail console checks failed" }
'PASS: 9 console checks'
