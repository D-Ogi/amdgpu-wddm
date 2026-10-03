# Host test of the firmware download, under Windows PowerShell 5.1 like the installer (called by test-dryrun.ps1).
# Uses the package's own common.ps1 and manifest.json; writes only under -WorkRoot, installs nothing.
#   powershell -File tools\release\test-firmware.ps1 -Installer <package>\installer -Manifest <package>\manifest.json -WorkRoot <dir>
# Cases: both download hosts answer; a real download of every file with its SHA256 checked independently; the same
# from a folder (-FirmwareDir); a pinned SHA256 that does not match refuses (download and folder), staging nothing.
# Leaves <WorkRoot>\download with the downloaded files for the -FirmwareDir dry runs of test-dryrun.ps1.
param([Parameter(Mandatory)][string]$Installer, [Parameter(Mandatory)][string]$Manifest, [Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Copy-Firmware($Fw) { return ($Fw | ConvertTo-Json -Depth 6 | ConvertFrom-Json) }
$fw = (Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json).firmware
[void][IO.Directory]::CreateDirectory($WorkRoot)
$n = @($fw.files).Count
Check ($n -eq 9 -and $fw.commit -match '^[0-9a-f]{40}$') "manifest firmware: $n files at commit $($fw.commit)"

$h = Test-FirmwareHosts -Firmware $fw -Rounds 1
Check ($h.ok.Count -eq 2) "both download hosts answer a HEAD request: $($h.ok -join ', ') $($h.bad -join '; ')"

'real download (TLS 1.2, Invoke-WebRequest -UseBasicParsing)'
$dl = Join-Path $WorkRoot 'download'
$t0 = [DateTime]::UtcNow
$staged = @(Get-FirmwareStaged -Firmware $fw -Staging $dl)
Check ($staged.Count -eq $n) "$($staged.Count) files staged in $([int]([DateTime]::UtcNow - $t0).TotalSeconds) s"
foreach ($f in @($fw.files)) {
    $p = Join-Path $dl $f.name
    $sha = if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { 'missing' }
    Check ($sha -eq ([string]$f.sha256).ToUpperInvariant() -and (Get-Item -LiteralPath $p).Length -eq [long]$f.size) "$($f.name): Get-FileHash $sha, $((Get-Item -LiteralPath $p).Length) bytes"
}
Check (@(Get-ChildItem -LiteralPath $dl -File).Count -eq $n) 'nothing else in the staging folder'

'fallback: the first address fails, the GitLab mirror serves the file'
$fb = Copy-Firmware $fw
$fb.files = @($fb.files | Where-Object { $_.name -eq 'LICENSE.amdgpu' })
$fb.url_templates = @(([string]$fw.url_templates[0]).Replace('{path}', '{path}.does-not-exist'), [string]$fw.url_templates[1])
$fbDir = Join-Path $WorkRoot 'fallback'
$staged = @(Get-FirmwareStaged -Firmware $fb -Staging $fbDir -Tries 1)
Check ($staged.Count -eq 1 -and (Get-FileHash -LiteralPath $staged[0]).Hash -eq ([string]$fb.files[0].sha256).ToUpperInvariant()) 'LICENSE.amdgpu from the second address after the first failed'

'from a folder (-FirmwareDir)'
$off = Join-Path $WorkRoot 'offline'
$staged = @(Get-FirmwareStaged -Firmware $fw -Staging $off -FromDir $dl)
Check ($staged.Count -eq $n) "$($staged.Count) files staged from $dl"

'a pinned SHA256 that does not match refuses'
$bad = Copy-Firmware $fw
$bad.files = @($bad.files | Where-Object { $_.name -eq 'LICENSE.amdgpu' })
$right = $bad.files[0].sha256
$bad.files[0].sha256 = $right.Substring(0, 63) + $(if ($right[63] -eq '0') { '1' } else { '0' })
$wrong = Join-Path $WorkRoot 'wrong-download'
$msg = $null
try { [void](Get-FirmwareStaged -Firmware $bad -Staging $wrong -Tries 1) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'LICENSE\.amdgpu could not be downloaded with the pinned SHA256') 'download with another SHA256 than pinned: refused'
Check ($msg -match 'git\.kernel\.org: SHA256 ' -and $msg -match 'gitlab\.com: SHA256 ') 'both hosts were tried and both answers rejected'
Check (-not (Test-Path -LiteralPath (Join-Path $wrong 'LICENSE.amdgpu'))) 'the rejected file is not left in staging'

$corrupt = Join-Path $WorkRoot 'corrupt'
[void][IO.Directory]::CreateDirectory($corrupt)
foreach ($f in Get-ChildItem -LiteralPath $dl -File) { Copy-Item -LiteralPath $f.FullName -Destination $corrupt }
[IO.File]::AppendAllText((Join-Path $corrupt 'cyan_skillfish2_rlc.bin'), 'x')
$msg = $null
try { [void](Get-FirmwareStaged -Firmware $fw -Staging (Join-Path $WorkRoot 'wrong-offline') -FromDir $corrupt) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'cyan_skillfish2_rlc\.bin has SHA256 [0-9A-F]{64}, this release pins') 'a changed file in the -FirmwareDir folder: refused'
Remove-Item -LiteralPath $corrupt, $off, $wrong, $fbDir, (Join-Path $WorkRoot 'wrong-offline') -Recurse -Force -ErrorAction SilentlyContinue

if ($fail) { "$fail firmware check(s) failed"; exit 1 }
'firmware checks passed'
exit 0
