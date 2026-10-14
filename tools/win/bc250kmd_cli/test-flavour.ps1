# Gate: a built bc250kmd_cli.exe is the flavour its build asked for (BD-167). The raw diagnostic verbs exist only in a
# lab build (bc250kmd_cli.c, AMDGPU_WDDM_LAB_FLAVOUR): a release CLI names none of them in its usage text, and a lab
# CLI names every one. Both name every form that a release install calls (tools/release/cli-commands.json). The CLI
# runs once, without arguments, which opens no adapter. No raw verb runs here: a CLI built in the wrong flavour would
# act on it, against the PC that builds (review r1 R4). verbs_test.c (test-verbs.ps1) runs every raw verb with a
# valid argument shape against a fake adapter instead, in the flavour of the same build: a release build must answer
# each with "unknown command". Windows PowerShell 5.1 and PowerShell 7; no window, nothing written.
#   pwsh -File tools\win\bc250kmd_cli\test-flavour.ps1 -Cli <dir>\bc250kmd_cli.exe -Flavour release|lab
param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][ValidateSet('release', 'lab')][string]$Flavour)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$labVerbs = @('read', 'write', 'memory', 'vread', 'vwrite', 'vcompare', 'vtable', 'gart', 'psp', 'gfx', 'ih', 'fence',
    'ib', 'dcn', 'dcnflip', 'sdmacopy', 'sdmaib', 'fbdump')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Run([string[]]$Arguments) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Cli; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.Arguments = ($Arguments -join ' ')
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardInput = $true
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(10000)) { $p.Kill(); throw "$Cli $($Arguments -join ' ') did not exit within 10 s" }
    return @{ Code = $p.ExitCode; Text = $out.Result + $err.Result }
}
function Offers([string]$Usage, [string]$Form) { return $Usage -match "(bc250kmd_cli |\| )$([regex]::Escape($Form))(?![A-Za-z0-9_-])" }

$usage = Run @()
Check ($usage.Code -eq 2 -and $usage.Text -match '(?m)^usage: bc250kmd_cli ') "usage without arguments, exit $($usage.Code)"
$forms = @((Get-Content -LiteralPath (Join-Path $repo 'tools\release\cli-commands.json') -Raw | ConvertFrom-Json).forms.PSObject.Properties | ForEach-Object Name)
$missing = @($forms | Where-Object { -not (Offers $usage.Text $_) })
Check ($missing.Count -eq 0) "every release form of cli-commands.json ($($forms.Count))$(if ($missing.Count) { ': missing ' + ($missing -join ', ') })"
$offered = @($labVerbs | Where-Object { Offers $usage.Text $_ })
if ($Flavour -eq 'lab') {
    Check ($offered.Count -eq $labVerbs.Count) "lab: every raw diagnostic verb in the usage ($($offered.Count) of $($labVerbs.Count))"
} else {
    Check ($offered.Count -eq 0) "release: no raw diagnostic verb in the usage$(if ($offered.Count) { ': ' + ($offered -join ', ') })"
}
if ($fail) { "FAIL: $fail check(s), $Cli is not a $Flavour CLI"; exit 1 }
"PASS: $Cli is a $Flavour CLI"
exit 0
