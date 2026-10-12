# Read-only: the pushed package zip on the lab, its size and SHA-256 against the development PC's hash.
# Generic: both the path and the wanted hash are arguments, so one copy serves every train.
param([Parameter(Mandatory = $true)][string]$Zip,
      [Parameter(Mandatory = $true)][string]$Want)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path -LiteralPath $Zip)) { "ABSENT $Zip"; exit 2 }
$i = Get-Item -LiteralPath $Zip
$h = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash
"path  : $Zip"
"bytes : $($i.Length)"
"sha256: $h"
if ($h -eq $Want) { 'zip OK (matches the development PC)' } else { "zip MISMATCH, expected $Want"; exit 3 }
