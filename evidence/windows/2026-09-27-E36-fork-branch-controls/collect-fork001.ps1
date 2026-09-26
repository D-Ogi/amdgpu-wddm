# Zip the E36 fork001 evidence on the target (everything except the DLLs and the cache) for one pull.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m13\fork-consolidated001'
$zip = 'C:\BC250\tmp\fork001-evidence.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
$files = Get-ChildItem $dir -File | Where-Object { $_.Extension -ne '.dll' }
$files += Get-ChildItem "$dir\smoke001" -File -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath ($files | ForEach-Object { $_.FullName }) -DestinationPath $zip
"$((Get-Item $zip).Length) bytes, $($files.Count) files"
