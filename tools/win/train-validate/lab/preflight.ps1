# Read-only preflight of unit A before the arms of a train validation (Windows PowerShell 5.1, SSH session).
# Generic: it names no train and no package.
#
#   preflight.ps1 [-Cu 40] [-TdrDelay 10] [-MaxTctl 80]
#
# What it refuses on: a CU mode other than 40 confirmed (the lab lost 16 CU once and a whole round of numbers
# with it), a DPM that is switched off (a 0x116 leaves DpmMode 0 behind), a TdrDelay other than 10 (the GUI
# default since 2026-10-08), a temperature at or above -MaxTctl, and another trial already running on the
# machine. The last one is why this exists: two agents on one lab produce two sets of numbers and no evidence.
# The resident tasks of the lab are not competing trials and are never counted as one: the overlay, the net
# watchdog and the emergency channel run at every boot and must keep running. -ResidentTasks names them.
param([int]$Cu = 40, [int]$TdrDelay = 10, [double]$MaxTctl = 80,
    [string[]]$ResidentTasks = @('BC250 monitor overlay', 'BC250 net watchdog', 'Lab emergency channel'))
$ErrorActionPreference = 'Continue'
$bad = @()
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
"utc $([DateTime]::UtcNow.ToString('o')) boot $($boot.ToUniversalTime().ToString('o')) uptime_s $([Math]::Round(([DateTime]::Now - $boot).TotalSeconds, 1))"
"installroot $inst"
$health = (& $cli health read 2>&1) -join ' '
"health: $health"
if ($health -notmatch 'flags=15') { $bad += 'start health is not flags=15' }
$p = Get-ItemProperty $par -ErrorAction SilentlyContinue
"params: CuMode=$($p.CuMode) CuModeLastApplied=$($p.CuModeLastApplied) CuModeConfirmed=$($p.CuModeConfirmed) DpmMode=$($p.DpmMode) DpmMaxMHz=$($p.DpmMaxMHz) DpmIdleMHz=$($p.DpmIdleMHz) HangRecoveryMode=$($p.HangRecoveryMode)"
if ([int]$p.CuModeConfirmed -ne $Cu) { $bad += "CuModeConfirmed is $($p.CuModeConfirmed), wanted $Cu" }
if ($null -ne $p.DpmMode -and [int]$p.DpmMode -eq 0) { $bad += 'DpmMode is 0: the clock control is switched off (a bugcheck leaves it so)' }
$dpm = (((& $cli dpm read 2>&1) -join ' | ') -split '\|' | Select-Object -Last 1).Trim()
"dpm: $dpm"
$fan = ((& $cli fan 2>&1) -join ' ').Trim()
"fan: $fan"
$clock = (& $cli clock read 2>&1) -join ' '
$tctl = -1
if ($clock -match 'temperature_mc=(\d+)') { $tctl = [double]$Matches[1] / 1000 }
"tctl: $tctl"
if ($tctl -lt 0) { $bad += 'the temperature is unreadable' } elseif ($tctl -ge $MaxTctl) { $bad += "tctl $tctl C is at or above $MaxTctl C" }
$gd = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction SilentlyContinue
"tdr: TdrDelay=$($gd.TdrDelay) TdrDdiDelay=$($gd.TdrDdiDelay) TdrLevel=$($gd.TdrLevel)"
if ([int]$gd.TdrDelay -ne $TdrDelay) { $bad += "TdrDelay is $($gd.TdrDelay), wanted $TdrDelay" }
# A competing trial: a running task of another kit, or a game or path tracing client already up.
$running = @(Get-ScheduledTask -ErrorAction SilentlyContinue |
    Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI|pathtrace|trial' -and $_.TaskName -notmatch 'emergency' })
$tasks = @($running | Where-Object { $ResidentTasks -notcontains $_.TaskName })
"tasks_resident: $(($running | Where-Object { $ResidentTasks -contains $_.TaskName } | ForEach-Object { $_.TaskName }) -join ', ')"
"tasks_running: $($tasks.Count) $(($tasks | ForEach-Object { $_.TaskName }) -join ', ')"
if ($tasks.Count) { $bad += "another trial task runs: $(($tasks | ForEach-Object { $_.TaskName }) -join ', ')" }
$procs = @(Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match 'q2rtx|ROTTR|witcher3|Factorio|dxrpt|vkfillcheck|llama|3DMark|SOTTR|AscentRT' })
"procs_running: $($procs.Count) $(($procs | ForEach-Object { $_.ProcessName }) -join ', ')"
if ($procs.Count) { $bad += "a client of another trial runs: $(($procs | ForEach-Object { $_.ProcessName }) -join ', ')" }
$mon = @(Get-Process -Name python, pythonw -ErrorAction SilentlyContinue)
"bc250mon_candidates: $($mon.Count)"
if ($bad.Count) { 'preflight: REFUSED ' + ($bad -join '; '); exit 3 }
'preflight: ok'
