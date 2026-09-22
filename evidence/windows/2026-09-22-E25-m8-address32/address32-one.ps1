$ErrorActionPreference = 'Continue'
$exDeadline = (Get-Date).AddMinutes(3)
$explorer = $null
do {
    $explorer = Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($explorer) { break }
    Start-Sleep -Seconds 3
} while ((Get-Date) -lt $exDeadline)
if (-not $explorer) { "NO EXPLORER"; exit 4 }
"explorer $($explorer.Id)"

$cmd = @'
@echo off
set MESA_SHADER_CACHE_DISABLE=true
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --only fill_g1 --runs 1 > C:\BC250\m8\out\compute.txt 2> C:\BC250\m8\out\compute.err
echo compute_exit %ERRORLEVEL%>> C:\BC250\m8\out\compute.txt
'@
Set-Content -Path 'C:\BC250\m8\run-full.cmd' -Value $cmd -Encoding ASCII
$action = New-ScheduledTaskAction -Execute 'C:\BC250\m8\run-full.cmd' -WorkingDirectory 'C:\BC250\m8'
$principal = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'bc250-m8-full' -Action $action -Principal $principal -Force | Out-Null
Remove-Item 'C:\BC250\m8\out\compute.txt','C:\BC250\m8\out\compute.err' -ErrorAction SilentlyContinue
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 2>&1 | Out-String
"temp $($raw.Trim())"
if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85) { "VERDICT HOT"; exit 4 }
Start-ScheduledTask -TaskName 'bc250-m8-full'
$deadline = (Get-Date).AddMinutes(3)
do {
    Start-Sleep -Seconds 2
    $t = Get-ScheduledTask -TaskName 'bc250-m8-full' -ErrorAction SilentlyContinue
    if (-not $t -or $t.State -ne 'Running') { break }
} while ((Get-Date) -lt $deadline)
"state $($t.State)"
if (Test-Path 'C:\BC250\m8\out\compute.err') {
    Select-String -Path 'C:\BC250\m8\out\compute.err' -Pattern 'clamped|SubmitCommand|preamble' | ForEach-Object { $_.Line }
}
$us = $null
if (Test-Path 'C:\BC250\m8\out\compute.txt') {
    $line = Select-String -Path 'C:\BC250\m8\out\compute.txt' -Pattern 'fill_g1|device ' | Select-Object -Last 2
    $line | ForEach-Object { $_.Line }
    $fill = Select-String -Path 'C:\BC250\m8\out\compute.txt' -Pattern 'fill_g1' | Select-Object -Last 1
    if ($fill -and $fill.Line -match 't_submit_to_idle_us=\s*([0-9.]+)') { $us = [double]$Matches[1] }
} else { 'compute.txt missing' }
$log = 'C:\BC250\m8\out\kmd-log.txt'
& 'C:\BC250\m8\bc250kmd_cli.exe' log > $log 2>&1
Select-String -Path $log -Pattern 'umd ib |HARDWARE FENCE|umd submit fence' | Select-Object -Last 12 | ForEach-Object { $_.Line }
if ($null -eq $us) { "VERDICT NORESULT"; exit 3 }
if ($us -gt 100000) { "VERDICT TIMEOUT us $us"; exit 2 }
"VERDICT FENCE us $us"
exit 0

