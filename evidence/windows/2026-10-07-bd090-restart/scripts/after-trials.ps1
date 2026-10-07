# LAB: after the BD-090 trials: KeepLog back to 0, and how much the kept stop logs take.
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty $par -Name KeepLog -Value 0 -Type DWord
$f = Get-ChildItem C:\BC250\kmdlog -Filter 'ring-*.log' -ErrorAction SilentlyContinue
"KeepLog $((Get-ItemProperty $par).KeepLog); kmdlog rings $($f.Count), $([int](($f | Measure-Object Length -Sum).Sum / 1MB)) MB, oldest $(($f | Sort-Object Name | Select-Object -First 1).Name)"
"rings today: $(@($f | Where-Object Name -like 'ring-20261007-*').Count)"
