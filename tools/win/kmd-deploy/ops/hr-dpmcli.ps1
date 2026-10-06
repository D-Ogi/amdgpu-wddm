& 'C:\BC250\dpm\bc250kmd_cli.exe' health read; "exit $LASTEXITCODE"
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' health read; "exit $LASTEXITCODE"
Get-Item 'C:\BC250\dpm\bc250kmd_cli.exe','C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' | ForEach-Object { "$($_.FullName) $($_.Length) $($_.LastWriteTimeUtc)" }
