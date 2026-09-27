$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07165'
& "$out\dispatch165.ps1" -Mode Cleanup
$files=@(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.txt','.err','.json','.log' })
$zip=Join-Path $out 'transition-receipts.zip'
if(Test-Path $zip){throw 'Archive exists'}
Compress-Archive -LiteralPath $files.FullName -DestinationPath $zip
