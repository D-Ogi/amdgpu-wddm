# X3: clear the GRUB start counter on the diagnostic stick after a good Windows start. Run by the SYSTEM task
# "Lab boot counter reset" at startup with a 3-minute delay. Same stick identity check as the emergency listener
# (label, FAT32, USB bus, exact size). Writes a fresh 1024-byte GRUB environment block (no variables).
$ErrorActionPreference = 'Stop'
$log = 'C:\BC250\x3\x3-clear.log'
function L($m) { Add-Content -LiteralPath $log -Value ('{0} {1}' -f (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'), $m) }
try {
    $v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
    if ($v.Count -ne 1) { L "skip: $($v.Count) volumes labelled BC250DIAG"; exit 0 }
    $disk = Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk
    if ($v[0].FileSystem -ne 'FAT32' -or $disk.BusType -ne 'USB' -or $disk.Size -ne 64160400896) { L 'skip: identity mismatch'; exit 0 }
    $root = "$($v[0].DriveLetter):\"
    $envf = Join-Path $root 'boot\grub\grubenv'
    if (-not (Test-Path -LiteralPath (Join-Path $root 'boot\grub\bc250diag.id')) -or -not (Test-Path -LiteralPath $envf)) {
        L 'skip: marker or grubenv missing'; exit 0
    }
    $old = [IO.File]::ReadAllText($envf)
    $head = "# GRUB Environment Block`n"
    $block = $head + ('#' * (1024 - $head.Length))
    if ($old -ceq $block) { L 'already clear'; exit 0 }
    $try = [regex]::Match($old, 'bc250_try=(\S*)').Groups[1].Value
    [IO.File]::WriteAllBytes($envf, [Text.Encoding]::ASCII.GetBytes($block))
    L "cleared bc250_try=[$try]"
} catch {
    L "ERROR $($_.Exception.Message)"
}
