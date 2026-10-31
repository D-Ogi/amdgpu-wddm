# One more HIP runtime folder for lab-arm.ps1, all files from its inbox: rt-<Name> = every file of
# C:\BC250\tmp\t35hip\inbox-rt-<Name> (pushed before), with no file of the installed package. Prints the hashes.
param([Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+$')][string]$Name)
$k = 'C:\BC250\tmp\t35hip'
$in = Join-Path $k "inbox-rt-$Name"
foreach ($f in 'amdhip64.dll', 'bc250hipblas.dll') { if (-not (Test-Path (Join-Path $in $f))) { "missing: $in\$f"; exit 2 } }
$d = Join-Path $k "rt-$Name"
if (Test-Path $d) { Remove-Item -LiteralPath $d -Recurse -Force }
New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item (Join-Path $in '*') $d -Force
Get-ChildItem $d -File | ForEach-Object { "rt-$Name $($_.Name) $((Get-FileHash $_.FullName).Hash)" }
