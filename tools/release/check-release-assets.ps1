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
  -Update <file>      also checks the self-update assets (docs/design/self-update.md) of the release flavour: the
                      manifest nagi-update.json that the file is, its signature nagi-update.json.p7s and each
                      component asset that the manifest names. Before the upload they must be in -LocalDir with the
                      manifest's size and SHA256. After the upload each one must answer 200 at -ReleaseBase.
                      -Download also compares the downloaded manifest with the file, byte for byte.
  -PinsFrom <file>    with -Update: the nagi-update.ps1 of the release published before this one (git show
                      v<previous>:tools/release/installer/nagi-update.ps1). The installed release checks the signature
                      of the next manifest with its own list of pinned signers, so the signature in -LocalDir must pass
                      Test-NagiManifestSignature of that file, with that file's list. A signer that the previous
                      release does not pin strands every installed copy.
  -FirstUpdate        with -Update and no -PinsFrom: no earlier release has the self-update (tester.37 only). The
                      signature is checked with the package's own nagi-update.ps1.

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
    [int]$TimeoutSec = 60,
    [string]$Update = '',
    [string]$PinsFrom = '',
    [switch]$FirstUpdate,
    # The download folder of the release: SelfUpdate.cs DownloadUrl, with the tag of the release (v<release>).
    [string]$ReleaseBase = 'https://github.com/D-Ogi/amdgpu-wddm/releases/download/v{release}'
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
if ($Update) {
    if (-not (Test-Path -LiteralPath $Update -PathType Leaf)) { Write-Output "UPDATE FAIL: no file $Update"; $fails++ }
    else {
        $um = Get-Content -LiteralPath $Update -Raw | ConvertFrom-Json
        if ([string]$um.release -ne [string]$manifest.release) {
            Write-Output "UPDATE FAIL: $Update is of release $([string]$um.release), the package of $([string]$manifest.release)"; $fails++
        }
        $base = $ReleaseBase.Replace('{release}', [string]$manifest.release).TrimEnd('/')
        $updateDir = Split-Path -Parent ([IO.Path]::GetFullPath($Update))
        # Each asset of the update: the manifest and its signature (no pin inside the manifest: the manifest file
        # itself is the reference), then each component with the manifest's size and SHA256.
        $items = @(
            [pscustomobject]@{ id = 'manifest'; name = 'nagi-update.json'; size = (Get-Item -LiteralPath $Update).Length; sha = (Get-FileSha $Update) }
            [pscustomobject]@{ id = 'signature'; name = 'nagi-update.json.p7s'; size = -1; sha = '' }
        ) + @(foreach ($c in @($um.components)) {
            [pscustomobject]@{ id = [string]$c.name; name = [string]$c.asset; size = [long]$c.size; sha = ([string]$c.sha256).ToUpperInvariant() }
        })
        foreach ($it in $items) {
            if ($LocalDir) {
                $f = Join-Path $LocalDir $it.name
                if ($it.id -eq 'manifest' -or $it.id -eq 'signature') { if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { $f = Join-Path $updateDir $it.name } }
                if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Write-Output "UPDATE $($it.id) $($it.name) FAIL: not in $LocalDir"; $fails++; continue }
                $len = (Get-Item -LiteralPath $f).Length
                if ($it.size -lt 0) { $ok = $len -gt 0; Write-Output "UPDATE $($it.id) $($it.name) local size $len $(if ($ok) { 'ok' } else { 'FAIL' })" }
                else {
                    $h = Get-FileSha $f; $ok = ($len -eq $it.size -and $h -eq $it.sha)
                    Write-Output "UPDATE $($it.id) $($it.name) local size $len (manifest $($it.size)) sha256 $($h.Substring(0, 8)) (manifest $($it.sha.Substring(0, 8))) $(if ($ok) { 'ok' } else { 'FAIL' })"
                }
                if (-not $ok) { $fails++ }
                continue
            }
            $u = "$base/$($it.name)"
            $hd = Get-Head $u
            $ok = ($hd.code -eq 200 -and ($it.size -lt 0 -or $hd.size -eq $it.size -or $hd.size -eq 0))
            $sz = if ($hd.size -gt 0) { "size $($hd.size)$(if ($it.size -ge 0) { " (manifest $($it.size))" })" } else { 'size not given' }
            Write-Output "UPDATE $($it.id) $u HTTP $($hd.code) $sz $(if ($ok) { 'ok' } else { 'FAIL' })$(if ($hd.why) { ': ' + $hd.why })"
            if (-not $ok) { $fails++; continue }
            if ($Download -and $it.sha) {
                New-Item -ItemType Directory -Force $Scratch | Out-Null
                $dst = Join-Path $Scratch $it.name
                try {
                    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
                    Invoke-WebRequest -Uri $u -OutFile $dst -UseBasicParsing -TimeoutSec ([Math]::Max($TimeoutSec, 600)) -UserAgent $ua
                    $ProgressPreference = $old
                    $len = (Get-Item -LiteralPath $dst).Length; $h = Get-FileSha $dst
                    $ok2 = ($len -eq $it.size -and $h -eq $it.sha)
                    Write-Output "UPDATE $($it.id) downloaded size $len sha256 $($h.Substring(0, 8)) (manifest $($it.sha.Substring(0, 8))) $(if ($ok2) { 'ok' } else { 'FAIL' })"
                    if (-not $ok2) { $fails++ }
                } catch { Write-Output "UPDATE $($it.id) download FAIL: $($_.Exception.Message)"; $fails++ }
                finally { Remove-Item -LiteralPath $dst -ErrorAction SilentlyContinue }
            }
        }
        # The signer, with the list of the release that will check it (the previous one). The signature is read from
        # -LocalDir, or beside the manifest file: these are the bytes that go up with the release.
        $sigFile = $null
        foreach ($d in @($LocalDir, $updateDir) | Where-Object { $_ }) {
            $c = Join-Path $d 'nagi-update.json.p7s'
            if (Test-Path -LiteralPath $c -PathType Leaf) { $sigFile = $c; break }
        }
        $checker = $null
        if ($PinsFrom) { $checker = $PinsFrom }
        elseif ($FirstUpdate) { $checker = Join-Path $Package 'files\installer\nagi-update.ps1' }
        if (-not $checker) {
            Write-Output 'UPDATE signer FAIL: name the nagi-update.ps1 of the previous release (-PinsFrom), or -FirstUpdate for the first release with the self-update'
            $fails++
        } elseif (-not (Test-Path -LiteralPath $checker -PathType Leaf)) {
            Write-Output "UPDATE signer FAIL: no file $checker"; $fails++
        } elseif (-not $sigFile) {
            Write-Output 'UPDATE signer FAIL: no nagi-update.json.p7s in -LocalDir or beside the manifest'; $fails++
        } else {
            # The checker's own function with the checker's own default list, as the installed release runs it.
            $v = & {
                param($Script, $Manifest, $Signature)
                . $Script
                $r = Test-NagiManifestSignature -ManifestPath $Manifest -SignaturePath $Signature
                [pscustomobject]@{ ok = $r.ok; reason = $r.reason; thumbprint = $r.thumbprint; pinned = @($script:NagiPinnedSigners) }
            } $checker ([IO.Path]::GetFullPath($Update)) $sigFile
            $pins = (@($v.pinned) | ForEach-Object { ([string]$_).Substring(0, 8) }) -join ','
            if ($v.ok) { Write-Output "UPDATE signer $($v.thumbprint) is pinned by $checker (pins $pins) ok" }
            else { Write-Output "UPDATE signer FAIL: $($v.reason) (checker $checker, pins $pins)"; $fails++ }
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
