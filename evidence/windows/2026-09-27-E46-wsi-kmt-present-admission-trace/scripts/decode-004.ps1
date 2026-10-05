# Decode run004\dxgkrnl.etl on the development PC (inbox Microsoft-Windows-DxgKrnl manifest).
# Event ids from ref\presentmon-etw\PresentData\ETW\Microsoft_Windows_DxgKrnl.h:
#   Blit_Info 0xa6 (166), PresentHistory_Start 0xab (171), PresentHistory_Info 0xac (172),
#   PresentHistoryDetailed_Start 0xd7 (215), Present_Start 0xb8 (184), BlitCancel_Info 0x2c (44).
param([string]$Run = 'P:\bc-250\scratch\wsi-kmt-2026-09-27\run004')
$etl = Join-Path $Run 'dxgkrnl.etl'
if (-not (Test-Path -LiteralPath $etl)) { throw "no $etl" }
"etl bytes " + (Get-Item -LiteralPath $etl).Length
$ids = 166, 171, 172, 215, 184, 44
$events = Get-WinEvent -Path $etl -Oldest -FilterXPath ("*[System[(EventID=" + ($ids -join ' or EventID=') + ")]]") -ErrorAction SilentlyContinue
"events of interest: " + @($events).Count
$events | Group-Object Id | ForEach-Object { "id $($_.Name): $($_.Count)" }
$blits = $events | Where-Object Id -eq 166
"blit events by process: " + (($blits | Group-Object ProcessId | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ', ')
$rows = foreach ($e in $blits) {
  $p = $e.Properties | ForEach-Object { $_.Value }
  [pscustomobject]@{ time = $e.TimeCreated.ToString('HH:mm:ss.fff'); pid = $e.ProcessId; props = ($p -join ' ') ; msg = ($e.Message -replace '\s+', ' ') }
}
$rows | Select-Object -First 5 | Format-List | Out-String -Width 300
$rows | Group-Object props | Sort-Object Count -Descending | Select-Object -First 12 | ForEach-Object { "$($_.Count) x [$($_.Name)]" }
$rows | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $Run 'blit-info.csv')
$ph = $events | Where-Object { $_.Id -in 171, 172, 215 }
"present history by process: " + (($ph | Group-Object ProcessId | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ', ')
$ph | Select-Object -First 3 | ForEach-Object { "ph id $($_.Id) pid $($_.ProcessId): " + (($_.Properties | ForEach-Object { $_.Value }) -join ' ') }
