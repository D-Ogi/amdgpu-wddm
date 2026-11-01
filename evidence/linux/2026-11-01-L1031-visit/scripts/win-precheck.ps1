# Read only, unit A Windows, before the L1031 Linux visit: reference root image, models, stick.
$ref = 'C:\BC250\m12\linux-reference001\build-root.ext4'
"ref root: " + $(if (Test-Path $ref) { "{0} GB" -f [math]::Round((Get-Item $ref).Length/1GB,1) } else { 'absent' })
Get-ChildItem C:\BC250\m12 -Directory -ErrorAction SilentlyContinue | ForEach-Object { "  m12: $($_.Name)" }
Get-ChildItem C:\BC250 -Recurse -Include *.gguf -ErrorAction SilentlyContinue | Select-Object -First 12 |
  ForEach-Object { "gguf: $($_.FullName) {0} GB" -f [math]::Round($_.Length/1GB,1) }
$v = Get-Volume | Where-Object FileSystemLabel -eq 'BC250DIAG'
"stick: $($v.DriveLetter): $($v.FileSystem) " + ((Get-ChildItem "$($v.DriveLetter):\efi\boot" | ForEach-Object Name) -join ',')
