<#
Packs <Root>\results\<RunId> into <Root>\results-<RunId>.tar (Windows' own tar.exe) so that
target.py pull can bring it back as one file. Windows PowerShell 5.1.
    target.py ps pack-results.ps1 -RunId icd85077e29
    target.py pull C:\BC250\cts\results-icd85077e29.tar P:\bc-250\scratch\cts\lab-results\results-icd85077e29.tar
#>
param([Parameter(Mandatory = $true)][string]$RunId, [string]$Root = 'C:\BC250\cts')
$ErrorActionPreference = 'Stop'
$results = Join-Path $Root 'results'
if (-not (Test-Path -LiteralPath (Join-Path $results $RunId))) { Write-Output "no $results\$RunId"; exit 1 }
$tar = Join-Path $Root "results-$RunId.tar"
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = "$env:windir\System32\tar.exe"
$psi.Arguments = "-cf `"$tar`" -C `"$results`" `"$RunId`""
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$p = [Diagnostics.Process]::Start($psi)
$o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {}; Write-Output 'tar timeout'; exit 1 }
if ($p.ExitCode -ne 0) { Write-Output ("tar exit $($p.ExitCode): " + $e.Result); exit 1 }
$h = (Get-FileHash -LiteralPath $tar -Algorithm SHA256).Hash
Write-Output ("{0} {1} bytes sha256 {2}" -f $tar, (Get-Item -LiteralPath $tar).Length, $h)
