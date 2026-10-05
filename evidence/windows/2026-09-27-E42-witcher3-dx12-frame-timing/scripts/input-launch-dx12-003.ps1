# Run one input action through the interactive-session task BC250-Witcher-Input and print the input log tail.
param([string]$Action = 'Enter', [int]$X = 0, [int]$Y = 0, [int]$Vk = 0, [int]$Ms = 1000)
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\witcher3-dx12'
if (-not (Get-Process witcher3 -ErrorAction SilentlyContinue)) { throw 'Game not running' }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
$a = New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $dir\input-dx12-003.ps1 -Action $Action -X $X -Y $Y -Vk $Vk -Ms $Ms"
$p = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName BC250-Witcher-Input -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask BC250-Witcher-Input
$deadline = (Get-Date).AddSeconds(15)
while ((Get-ScheduledTask -TaskName BC250-Witcher-Input).State -eq 'Running' -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
"task_result=" + (Get-ScheduledTaskInfo BC250-Witcher-Input).LastTaskResult
if (Test-Path "$dir\input-003.log") { Get-Content "$dir\input-003.log" -Tail 1 }
