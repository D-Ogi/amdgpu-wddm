# Stamps NEW translations with the source hash of the English text they were made from (src/Strings.cs, A6).
#
#   pwsh tools\win\amdgpu_wddm_control\strings\stamp.ps1 [-Language pl,ja,ko]
#
# Only a line whose hash field is "?" is stamped: the translator writes "?" when the translation is new or was redone
# for the current English text. A stale hash is never refreshed here: retranslate the line and set "?" again. This
# keeps the rule that a build never blesses stale text. Prints the lines it stamped and the stale or missing ids.

param([string[]]$Language = @('pl', 'ja', 'ko'))

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$utf8 = New-Object Text.UTF8Encoding $false

function Get-Hash8([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { (($sha.ComputeHash($utf8.GetBytes($Text)) | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 8) } finally { $sha.Dispose() }
}

function Read-Table([string]$Path) {
    $map = @{}
    foreach ($l in [IO.File]::ReadAllLines($Path, $utf8)) {
        if ($l -match '^\s*$' -or $l.StartsWith('#')) { continue }
        $p = $l.Split('|', 4)
        if ($p.Count -eq 4) { $map[$p[0]] = $p[3].Replace('\n', "`n") }
    }
    $map
}

$en = Read-Table (Join-Path $here 'strings.en.txt')
foreach ($lang in $Language) {
    $path = Join-Path $here "strings.$lang.txt"
    $out = New-Object Collections.Generic.List[string]
    $stamped = 0; $seen = @{}
    foreach ($l in [IO.File]::ReadAllLines($path, $utf8)) {
        $p = $l.Split('|', 4)
        if ($l -match '^\s*$' -or $l.StartsWith('#') -or $p.Count -ne 4) { $out.Add($l); continue }
        $seen[$p[0]] = $true
        if ($p[2] -eq '?' -and $en.ContainsKey($p[0])) {
            $p[2] = Get-Hash8 $en[$p[0]]
            $stamped++
        } elseif ($en.ContainsKey($p[0]) -and $p[2] -ne (Get-Hash8 $en[$p[0]])) {
            Write-Host "  $lang stale: $($p[0])"
        }
        $out.Add($p -join '|')
    }
    foreach ($id in $en.Keys | Sort-Object) { if (-not $seen.ContainsKey($id)) { Write-Host "  $lang missing: $id" } }
    [IO.File]::WriteAllText($path, (($out -join "`n") + "`n"), $utf8)
    Write-Host "$lang`: $stamped stamped"
}
