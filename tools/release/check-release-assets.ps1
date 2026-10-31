<#
.SYNOPSIS
Checks the addresses that the installer of a package downloads from (BD-278).

.DESCRIPTION
The installer gets each optional component from the url templates of the package's installer\components.json, with
{release} already expanded by build-release.ps1 and {name} expanded at install time (Get-ComponentAssetUrls of
installer\components.ps1, which this script uses). tester.31 to tester.34 pinned an asset that their releases did not
carry, and every llama.cpp download got HTTP 404. This script finds that before a tester does:

  before the upload   -LocalDir <folder>: every pinned asset is in the folder with the pinned size and SHA256.
  after the upload    (no -LocalDir): every address of every pinned asset answers 200 with the pinned size;
                      -Download also downloads each asset and checks its SHA256.
  -Models             also checks the model addresses (HEAD and size only, the files are several GB).

A component without a pin (no 64-digit SHA256) is listed and not checked: the installer refuses it and downloads
nothing. One line per check, "ASSET ..." or "MODEL ...", then "check-release-assets: ok" or "... FAIL (<n>)".
Exit codes: 0 ok, 1 a check failed, 2 usage.

.EXAMPLE
pwsh -File tools\release\check-release-assets.ps1 -Package <set>\package\release\amdgpu-wddm-tester-<ver> -LocalDir <dist>
pwsh -File tools\release\check-release-assets.ps1 -Package <set>\package\release\amdgpu-wddm-tester-<ver> -Download
#>
param(
    [Parameter(Mandatory)][string]$Package,
    [string]$LocalDir = '',
    [switch]$Download,
    [switch]$Models,
    [string]$Scratch = '',
    [int]$TimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\components.ps1')

$json = $Package
if (Test-Path -LiteralPath $Package -PathType Container) {
    $json = @(
        (Join-Path $Package 'files\installer\components.json'),
        (Join-Path $Package 'installer\components.json')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $json -or -not (Test-Path -LiteralPath $json -PathType Leaf)) {
    Write-Output "check-release-assets: no installer\components.json in $Package"; exit 2
}
$manifest = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
if (([string]$manifest.release) -match '[{}]') {
    Write-Output "check-release-assets: $json still holds {release}: give the package's file, not tools\release\components.json"; exit 2
}
$problems = @(Test-ComponentsManifest $manifest)
foreach ($p in $problems) { Write-Output "MANIFEST $p" }
$fails = $problems.Count
$ua = 'amdgpu-wddm-release-check'
if ($Download -and -not $Scratch) { $Scratch = Join-Path ([IO.Path]::GetTempPath()) ("check-release-assets-" + [Guid]::NewGuid().ToString('N').Substring(0, 8)) }

function Get-Head([string]$Url) {
    try {
        $r = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing -TimeoutSec $TimeoutSec -UserAgent $ua -MaximumRedirection 8
        $len = $r.Headers['Content-Length']
        if ($len -is [array]) { $len = $len[0] }
        return [pscustomobject]@{ code = [int]$r.StatusCode; size = [long]("0$len"); why = '' }
    } catch {
        $code = 0
        if ($_.Exception.Response) { try { $code = [int]$_.Exception.Response.StatusCode } catch { } }
        return [pscustomobject]@{ code = $code; size = -1; why = $_.Exception.Message }
    }
}
function Get-FileSha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }

Write-Output "release $([string]$manifest.release) from $json"
foreach ($c in @($manifest.components)) {
    $id = [string]$c.id; $name = [string]$c.asset.name; $want = ([string]$c.asset.sha256).ToUpperInvariant(); $size = [long]$c.asset.size
    if ($want -notmatch '^[0-9A-F]{64}$') { Write-Output "ASSET $id $name unpinned: the installer refuses it, nothing to check"; continue }
    if ($LocalDir) {
        $f = Join-Path $LocalDir $name
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Write-Output "ASSET $id $name FAIL: not in $LocalDir"; $fails++; continue }
        $len = (Get-Item -LiteralPath $f).Length; $h = Get-FileSha $f
        $ok = ($len -eq $size -and $h -eq $want)
        Write-Output "ASSET $id $name local size $len (pin $size) sha256 $($h.Substring(0, 8)) (pin $($want.Substring(0, 8))) $(if ($ok) { 'ok' } else { 'FAIL' })"
        if (-not $ok) { $fails++ }
        continue
    }
    foreach ($u in @(Get-ComponentAssetUrls $manifest $c)) {
        $hd = Get-Head $u
        $ok = ($hd.code -eq 200 -and ($hd.size -eq $size -or $hd.size -eq 0))
        $sz = if ($hd.size -gt 0) { "size $($hd.size) (pin $size)" } else { 'size not given' }
        Write-Output "ASSET $id $u HTTP $($hd.code) $sz $(if ($ok) { 'ok' } else { 'FAIL' })$(if ($hd.why) { ': ' + $hd.why })"
        if (-not $ok) { $fails++; continue }
        if ($Download) {
            New-Item -ItemType Directory -Force $Scratch | Out-Null
            $dst = Join-Path $Scratch $name
            try {
                $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
                Invoke-WebRequest -Uri $u -OutFile $dst -UseBasicParsing -TimeoutSec ([Math]::Max($TimeoutSec, 600)) -UserAgent $ua
                $ProgressPreference = $old
                $len = (Get-Item -LiteralPath $dst).Length; $h = Get-FileSha $dst
                $ok2 = ($len -eq $size -and $h -eq $want)
                Write-Output "ASSET $id downloaded size $len sha256 $($h.Substring(0, 8)) (pin $($want.Substring(0, 8))) $(if ($ok2) { 'ok' } else { 'FAIL' })"
                if (-not $ok2) { $fails++ }
            } catch { Write-Output "ASSET $id download FAIL: $($_.Exception.Message)"; $fails++ }
            finally { Remove-Item -LiteralPath $dst -ErrorAction SilentlyContinue }
        }
    }
}
if ($Models) {
    foreach ($m in @($manifest.models)) {
        if (-not [string]$m.url) { continue }
        $hd = Get-Head ([string]$m.url)
        $ok = ($hd.code -eq 200 -and ($hd.size -eq [long]$m.size -or $hd.size -eq 0))
        $sz = if ($hd.size -gt 0) { "size $($hd.size) (pin $([long]$m.size))" } else { 'size not given' }
        Write-Output "MODEL $([string]$m.id) HTTP $($hd.code) $sz $(if ($ok) { 'ok' } else { 'FAIL' })$(if ($hd.why) { ': ' + $hd.why })"
        if (-not $ok) { $fails++ }
    }
}
if ($Download -and (Test-Path -LiteralPath $Scratch)) { Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue }
if ($fails) { Write-Output "check-release-assets: FAIL ($fails)"; exit 1 }
Write-Output 'check-release-assets: ok'
exit 0
