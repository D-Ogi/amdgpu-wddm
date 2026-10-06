# Tuner lab step: open the CPU surface gate (CpuTune=1, read once at driver start) for the readback; restart separate.
param([int]$Value = 1)
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
New-ItemProperty -Path $k -Name CpuTune -PropertyType DWord -Value $Value -Force | Out-Null
'CpuTune = {0}' -f (Get-ItemProperty $k -Name CpuTune).CpuTune
