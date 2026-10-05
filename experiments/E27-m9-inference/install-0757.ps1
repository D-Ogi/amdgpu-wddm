$ErrorActionPreference='Stop'
$cli='C:\BC250\m8\bc250kmd_cli.exe'
$package='C:\BC250\m9\kmd0757'
& $cli info
$hash=(Get-FileHash "$package\bc250kmd.sys" -Algorithm SHA256).Hash
if ($hash -ne '16FFDAA230424DC12F027890F73032EB0D06C6EF570D3A332B4EB0703618B96D') { throw 'Package hash mismatch' }
pnputil /add-driver "$package\bc250kmd.inf" /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "Driver install failed: $LASTEXITCODE" }
Start-Sleep -Seconds 8
& $cli info
& 'C:\BC250\tmp\e19_target.ps1' -Phase confirm -Package C:\BC250\m8 -Tag m9-kmd0757
