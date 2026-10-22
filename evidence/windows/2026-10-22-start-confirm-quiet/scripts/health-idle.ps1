param([int]$Seconds = 150, [int]$Every = 2)
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$end = (Get-Date).AddSeconds($Seconds)
$prev = $null
while ((Get-Date) -lt $end) {
    $line = ((& $cli health read 2>&1) | Select-String '^health abi' | Select-Object -First 1).Line
    if ($line -match 'completed=(\d+) age_ms=(\d+) ready_ms=(\d+)') {
        $c = [uint64]$Matches[1]; $a = [uint64]$Matches[2]
        if ($prev -eq $null -or $c -ne $prev) { '{0:HH:mm:ss} completed={1} age_ms={2}' -f [DateTime]::UtcNow, $c, $a }
        $prev = $c
    } else { "{0:HH:mm:ss} unread: $line" -f [DateTime]::UtcNow }
    Start-Sleep -Seconds $Every
}
'{0:HH:mm:ss} end completed={1}' -f [DateTime]::UtcNow, $prev
