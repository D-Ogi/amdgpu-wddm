# LAB read-only: shutdown/boot events of the last 2 hours (clean shutdown 6006/1074, unexpected 6008/41, boot 6005/12),
# to tell a hang in the Windows shutdown from a hang after it (firmware POST or early boot).
$since = (Get-Date).AddHours(-2)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 12, 13, 41, 1074, 6005, 6006, 6008, 109 } -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated | ForEach-Object {
        $m = ($_.Message -split "`n")[0]
        if ($m.Length -gt 110) { $m = $m.Substring(0, 110) }
        "$($_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss'))Z $($_.Id) $($_.ProviderName) $m"
    }
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; ProviderName = 'bc250kmd' } -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated | Select-Object -Last 10 | ForEach-Object {
        $m = ($_.Message -split "`n")[0]
        if ($m.Length -gt 110) { $m = $m.Substring(0, 110) }
        "$($_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss'))Z kmd $($_.Id) $m"
    }
