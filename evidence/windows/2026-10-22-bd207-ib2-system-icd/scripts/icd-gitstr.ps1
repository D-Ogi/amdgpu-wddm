# Lab side (PowerShell 5.1), read only: print the SHA256 and the "(git-<hash>)" strings of one or more Vulkan ICD
# DLLs, so that a signed package copy can be mapped to its source commit.
#   icd-gitstr.ps1 -Path <dll>[,<dll>...]
param([Parameter(Mandatory)][string[]]$Path)
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o'))"
# target.py passes a comma list as one string: split it here.
foreach ($p in ($Path | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
    if (-not (Test-Path -LiteralPath $p)) { "missing $p"; continue }
    $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
    $bytes = [IO.File]::ReadAllBytes($p)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    $hits = [regex]::Matches($text, 'git-[0-9a-f]{10}') | ForEach-Object { $_.Value } | Sort-Object -Unique
    "dll $p $h $($bytes.Length) bytes"
    "  strings: $($hits -join ' ')"
}
