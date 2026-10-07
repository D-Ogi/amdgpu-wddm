# LAB: BD-090 trial: one live device restart (pnputil /restart-device) of the GPU, then the checks of LAB-PLAN:
# device status, guard lines in the kept stop log, health within 60 s, CreateDevice for DWM's PID. Bounded ~100 s.
param([string]$Tag = 't1')
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\dpaudio\bc250kmd_cli.exe'
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$id = (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }).InstanceId
$dwm0 = (Get-Process dwm -ErrorAction SilentlyContinue).Id
$logs0 = @(Get-ChildItem C:\BC250\kmdlog -Filter 'ring-*.log' -ErrorAction SilentlyContinue | ForEach-Object Name)
"[$Tag] before: dwm $dwm0 UnconfirmedStarts $((Get-ItemProperty $par).UnconfirmedStarts) GuardBoot $((Get-ItemProperty "$par\GuardBoot" -ErrorAction SilentlyContinue).Confirmed) rings $($logs0.Count)"
$t0 = Get-Date
& pnputil.exe /restart-device "$id" 2>&1 | ForEach-Object { "  pnputil: $_" }
"[$Tag] pnputil returned after $([int]((Get-Date) - $t0).TotalSeconds) s"
Start-Sleep 15
$d = Get-PnpDevice -InstanceId $id
"[$Tag] +15s device $($d.Status) $($d.Problem) UnconfirmedStarts $((Get-ItemProperty $par).UnconfirmedStarts) StageHistory $((Get-ItemProperty $par).StageHistory)"
$ok = $false
for ($i = 0; $i -lt 12 -and -not $ok; $i++) {
    $h = (& $cli health 2>&1 | Out-String).Trim()
    if ($h -match 'flags=(\d+).*completed=(\d+)' -and ([int]$Matches[1] -band 3) -eq 3 -and [int]$Matches[2] -gt 0) { $ok = $true }
    else { Start-Sleep 5 }
}
"[$Tag] health ok=$ok : $h"
$dwm1 = Get-Process dwm -ErrorAction SilentlyContinue
"[$Tag] dwm now $($dwm1.Id) (was $dwm0) start $($dwm1.StartTime.ToUniversalTime().ToString('o'))"
if ($dwm1) { "[$Tag] dwm threads: " + (($dwm1.Threads | Group-Object { "$($_.ThreadState)/$($_.WaitReason)" } | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ' ') }
$new = Get-ChildItem C:\BC250\kmdlog -Filter 'ring-*.log' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin $logs0 } | Sort-Object Name
foreach ($f in $new) {
    "[$Tag] kept stop log $($f.Name) $($f.Length) bytes:"
    Select-String -Path $f.FullName -Pattern 'guard:|CreateDevice .*pid|black' | Select-Object -Last 12 | ForEach-Object { '    ' + $_.Line.Trim() }
}
'--- live ring (guard / CreateDevice)'
& $cli log 2>&1 | Select-String -Pattern 'guard:|CreateDevice' | Select-Object -Last 20 | ForEach-Object { '    ' + $_.Line.Trim() }
'--- System events since the restart'
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$t0} -ErrorAction SilentlyContinue | Select-Object -First 10 | ForEach-Object { '{0:o} {1} {2} {3}' -f $_.TimeCreated.ToUniversalTime(), $_.ProviderName, $_.Id, ($_.Message -split "`n")[0] }
