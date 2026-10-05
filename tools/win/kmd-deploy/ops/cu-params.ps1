$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
(Get-Item $k).Property | Where-Object { $_ -match '^(Cu|Dpm)' } | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" }
