# E09 on the target, one phase per call, everything logged under C:\BC250\e09\out. Runs elevated (SSH session).
#
#   -Phase install                  new package, gates closed by the INF
#   -Phase gate -On 0|1             EnableMmio, EnableVram, EnableGart all to that value; restart the device; confirm
#   -Phase sweep -Tag x             GC and MMHUB register sweeps through bc250rd (the independent witness)
#   -Phase plan|enable|restore      the GART command of bc250kmd, output kept
param(
    [Parameter(Mandatory)][ValidateSet('install', 'gate', 'sweep', 'plan', 'enable', 'restore')][string]$Phase,
    [string]$Tag = 'x',
    [int]$On = 0,
    [string]$Package = 'C:\BC250\e09'
)

$ErrorActionPreference = 'Continue'
$out = Join-Path $Package 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = (Get-Date).ToString('HHmmss')
$log = Join-Path $out "$Phase-$Tag-$stamp.txt"
$cli = Join-Path $Package 'bc250kmd_cli.exe'
$rd = 'C:\BC250\bc250rd\bc250rd_cli.exe'
$reglist = 'C:\BC250\bc250rd\reglist.txt'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$hwid = 'PCI\VEN_1002&DEV_13FE'

function Say([string]$text) { $text | Tee-Object -FilePath $log -Append }
function Wait-Stage61 {
    foreach ($i in 1..20) {
        $p = Get-ItemProperty $params -ErrorAction SilentlyContinue
        if ($p.LastStage -eq 61) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}
function State {
    $gpu = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
    $p = Get-ItemProperty $params -ErrorAction SilentlyContinue
    Say ("device   {0}   status {1}   problem {2}" -f $gpu.FriendlyName, $gpu.Status, $gpu.Problem)
    Say ("driver   {0}" -f (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data)
    Say ("stages   last {0}   history {1}   unconfirmed {2}" -f $p.LastStage, $p.StageHistory, $p.UnconfirmedStarts)
    Say ("gates    EnableMmio {0}  EnableMmioWrite {1}  EnableVram {2}  EnableVramWrite {3}  EnableGart {4}" -f `
        $p.EnableMmio, $p.EnableMmioWrite, $p.EnableVram, $p.EnableVramWrite, $p.EnableGart)
}
function Alive { & $cli info 2>&1 | Select-String 'version|last stage|presents' | ForEach-Object { Say "$_" } }

Say ("time " + (Get-Date).ToString('s') + "  phase $Phase $Tag")
switch ($Phase) {
    'install' {
        pnputil /add-driver (Join-Path $Package 'bc250kmd.inf') /install 2>&1 | ForEach-Object { Say "$_" }
        if (Wait-Stage61) { & $cli confirm | ForEach-Object { Say "$_" } } else { Say 'stage 61 NOT reached' }
        State
    }
    'gate' {
        Set-ItemProperty $params -Name EnableMmio -Value $On -Type DWord
        Set-ItemProperty $params -Name EnableMmioWrite -Value 0 -Type DWord
        Set-ItemProperty $params -Name EnableVram -Value $On -Type DWord
        Set-ItemProperty $params -Name EnableVramWrite -Value 0 -Type DWord
        Set-ItemProperty $params -Name EnableGart -Value $On -Type DWord
        $gpu = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
        Say ("disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        Start-Sleep -Seconds 3
        Say ("enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        if (Wait-Stage61) { & $cli confirm | ForEach-Object { Say "$_" } } else { Say 'stage 61 NOT reached' }
        State
    }
    'sweep' {
        foreach ($ip in 'GC', 'MMHUB') {
            $file = Join-Path $out "sweep-$ip-$Tag-$stamp.log"
            & $rd sweep $reglist "$ip." > $file 2>&1
            Say ("sweep $ip exit $LASTEXITCODE, $((Get-Content $file | Measure-Object -Line).Lines) lines -> $file")
        }
        Alive
    }
    default {
        & $cli gart $Phase 2>&1 | ForEach-Object { Say "$_" }
        Say "exit code $LASTEXITCODE"
        Alive
    }
}
