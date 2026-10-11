# The guide character's art, checked and listed for a build. One copy of this check serves the control application
# and the setup window, so the two programs embed the same bytes under the same resource names.
#
# What it does, in this order:
#   1. reads the expression-to-file map from src\Guide.cs (the one copy of that map) and refuses a map that lost an
#      expression;
#   2. keeps the expressions the caller asked for (-Expressions; the setup window can draw only the expressions its
#      screens reach, so it embeds those and no more);
#   3. refuses a file that is absent, and, where the art directory has a PROVENANCE.md with a byte list, refuses a
#      file whose size or SHA-256 is not the one recorded there. An art directory passed with -ArtDir that has no
#      such list (another set, for a trial build) is listed without that comparison and the caller is told.
#
# It writes one line about what it found to the host and returns one object per file:
#   Expression, File, Size (128 or 256), Path (the file on disk), Resource (the name in the exe).
# The order is the order of the map and then the size, so a build embeds the resources in a deterministic order.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtDir,          # the directory with <file>@128.png and <file>@256.png
    [Parameter(Mandatory)][string]$GuideSource,     # src\Guide.cs of the control application: the expression map
    [string[]]$Expressions,                         # the expression ids to take; all nine when absent
    [int[]]$Sizes = @(128, 256)
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ArtDir)) { throw "guide art: no directory $ArtDir" }
if (-not (Test-Path -LiteralPath $GuideSource)) { throw "guide art: no expression map at $GuideSource" }

$map = [regex]::Matches((Get-Content -LiteralPath $GuideSource -Raw), '\{ "(?<expr>\d\d-[a-z-]+)", "(?<file>[a-z-]+)" \}')
if ($map.Count -ne 9) { throw "Guide.ArtFile: $($map.Count) expressions found, 9 expected" }
$wanted = @(foreach ($m in $map) { if (-not $Expressions -or $Expressions -contains $m.Groups['expr'].Value) { $m } })
if ($Expressions) {
    $unknown = @($Expressions | Where-Object { $e = $_; -not ($map | Where-Object { $_.Groups['expr'].Value -eq $e }) })
    if ($unknown.Count) { throw "guide art: Guide.ArtFile has no expression $($unknown -join ', ')" }
}
if (-not $wanted.Count) { throw 'guide art: no expression asked for' }

# The byte list of PROVENANCE.md: | `<file>@<size>.png` | <bytes> | `<sha-256>` |
$recorded = @{}
$provenance = Join-Path $ArtDir 'PROVENANCE.md'
if (Test-Path -LiteralPath $provenance) {
    foreach ($m in [regex]::Matches((Get-Content -LiteralPath $provenance -Raw),
            '(?m)^\|\s*`(?<file>[a-z0-9@.-]+\.png)`\s*\|\s*(?<bytes>\d+)\s*\|\s*`(?<sha>[0-9A-F]{64})`\s*\|')) {
        $recorded[$m.Groups['file'].Value] = @{ bytes = [int]$m.Groups['bytes'].Value; sha = $m.Groups['sha'].Value }
    }
}

$out = @()
$checked = 0
foreach ($m in $wanted) {
    foreach ($size in $Sizes) {
        $name = "$($m.Groups['file'].Value)@$size.png"
        $path = Join-Path $ArtDir $name
        if (-not (Test-Path -LiteralPath $path)) { throw "guide art: $path missing" }
        if ($recorded.Count) {
            if (-not $recorded.ContainsKey($name)) { throw "guide art: $name is not in the byte list of $provenance" }
            $want = $recorded[$name]
            $file = Get-Item -LiteralPath $path
            if ($file.Length -ne $want.bytes) { throw "guide art: $name is $($file.Length) bytes, $provenance records $($want.bytes)" }
            $sha = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($sha -ne $want.sha) { throw "guide art: $name hashes $sha, $provenance records $($want.sha)" }
            $checked++
        }
        $out += [pscustomobject]@{
            Expression = $m.Groups['expr'].Value
            File       = $m.Groups['file'].Value
            Size       = $size
            Path       = $path
            Resource   = "nagi.$($m.Groups['expr'].Value)@$size.png"
        }
    }
}

if ($checked -eq $out.Count) {
    Write-Host "  guide art: $($out.Count) files from $ArtDir, every one of them the bytes PROVENANCE.md records"
} else {
    Write-Host "  guide art: $($out.Count) files from $ArtDir, no byte list in that directory: the bytes are not compared"
}
$out
