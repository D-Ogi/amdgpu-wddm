# The KMD rewrites CuModeLastReason at every start (a telemetry value). kmd178-deploy001's push captured 6 (the
# cold-boot refusal of 40 CU on 0.7.176.1); the 178 start applied 24 with reason 0, so postflight saw a difference.
# Put the captured value back so the frozen postflight compares equal; the next start overwrites it anyway.
param([int]$Value = 6)
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty -Path $k -Name CuModeLastReason -Value $Value -Type DWord
(Get-Item $k).Property | Where-Object { $_ -match '^(Cu|Dpm)' } | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" }
