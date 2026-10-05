# Gate: the release's bc250kmd_cli.exe answers every form in cli-commands.json. Runs the CLI without arguments (it
# prints its usage and exits 2 before it opens any adapter) and requires each form after "bc250kmd_cli " or "| " in
# the usage text. With -Sources, the CLI and both copies of bc250control.dll must come from one build folder, so the
# CLI and the DLL of a release are one source (tester.10 paired a DLL of the control-app branch with a CLI of the KMD
# branch, without health and clock). Windows PowerShell 5.1 and PowerShell 7; no window, nothing written.
#   pwsh -File tools\release\test-cli-commands.ps1 -Cli <package>\payload\tools\bc250kmd_cli.exe [-Sources release-sources.json]
param([Parameter(Mandatory)][string]$Cli, [string]$Commands, [string]$Sources)
$ErrorActionPreference = 'Stop'
if (-not $Commands) { $Commands = Join-Path $PSScriptRoot 'cli-commands.json' }  # 5.1 has no $PSScriptRoot in param defaults
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

$forms = @((Get-Content -LiteralPath $Commands -Raw | ConvertFrom-Json).forms.PSObject.Properties | ForEach-Object Name)
Check ($forms.Count -gt 0) "$($forms.Count) forms in $(Split-Path $Commands -Leaf)"
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $Cli; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardInput = $true
$p = [Diagnostics.Process]::Start($psi)
$p.StandardInput.Close()
$out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(10000)) { $p.Kill(); throw "$Cli did not exit within 10 s" }
$usage = $out.Result + $err.Result
Check ($p.ExitCode -eq 2 -and $usage -match '(?m)^usage: bc250kmd_cli ') "usage without arguments, exit $($p.ExitCode) ($((Get-FileHash -LiteralPath $Cli).Hash.Substring(0, 8)))"
foreach ($form in $forms) {
    Check ($usage -match "(bc250kmd_cli |\| )$([regex]::Escape($form))(?![A-Za-z0-9_-])") "form '$form'"
}

if ($Sources) {
    $files = @((Get-Content -LiteralPath $Sources -Raw | ConvertFrom-Json).files)
    $folder = { param($p) $f = @($files | Where-Object { $_.path -eq $p }); if ($f.Count -ne 1) { return "<$($f.Count) entries for $p>" }; return ($f[0].source -replace '/[^/]+$', '') }
    $cliDir = & $folder 'payload/tools/bc250kmd_cli.exe'
    $dllDirs = @('payload/tools/bc250control.dll', 'payload/control/bc250control.dll' | ForEach-Object { & $folder $_ })
    Check (($dllDirs | Where-Object { $_ -ne $cliDir }).Count -eq 0) "CLI and control DLLs from one build folder ($cliDir)"
}

if ($fail) { "FAIL: $fail check(s)"; exit 1 }
'PASS: bc250kmd_cli.exe answers every listed form'
