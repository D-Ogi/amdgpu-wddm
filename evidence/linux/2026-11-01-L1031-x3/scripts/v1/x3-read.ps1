# X3: read the GRUB start counter on the diagnostic stick from Windows (no write). Same identity check as x3-clear.ps1.
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
if ($v.Count -ne 1) { "volumes labelled BC250DIAG: $($v.Count)"; exit 1 }
$disk = Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk
'{0}: fs={1} bus={2} size={3}' -f $v[0].DriveLetter, $v[0].FileSystem, $disk.BusType, $disk.Size
$root = "$($v[0].DriveLetter):\"
$envf = Join-Path $root 'boot\grub\grubenv'
$b = [IO.File]::ReadAllBytes($envf)
"grubenv bytes=$($b.Length)"
$t = [Text.Encoding]::ASCII.GetString($b).TrimEnd('#')
$t -split "`n" | Where-Object { $_ -and $_ -notmatch '^#+$' }
(Select-String -LiteralPath (Join-Path $root 'boot\grub\grub.cfg') -Pattern '^set default=').Line
Get-ChildItem (Join-Path $root 'efi\boot') | ForEach-Object { '{0} {1}' -f $_.Name, $_.Length }
$bi = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
"windows boot $bi"
