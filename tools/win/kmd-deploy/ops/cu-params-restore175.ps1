# Restore the kmd175-deploy003 captured CU parameters after the to40-0 trial (receipt cu-trial\receipts\to40-0.json):
# capture had CuModeLastReason=0 and no CuMode value. Effective mode is unchanged (24 either way).
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty -Path $k -Name CuModeLastReason -Value 0 -Type DWord
Remove-ItemProperty -Path $k -Name CuMode -ErrorAction SilentlyContinue
(Get-Item $k).Property | Where-Object { $_ -match '^(Cu|Dpm)' } | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" }
