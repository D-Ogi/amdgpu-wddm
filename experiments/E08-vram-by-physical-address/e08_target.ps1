# E08 on the target, one phase per call, everything logged under C:\BC250\e08\out. Runs elevated (SSH session).
# (The helper is Run-Cli because "cli" is a built-in PowerShell alias of Clear-Item and aliases win over functions.)
# No GPU register is written by this experiment: the CLI's memory commands read and write VRAM words only.
#
#   -Phase install                                   new package, gates closed by the INF
#   -Phase gate -Mmio 0|1 -Vram 0|1 -VramWrite 0|1   set the gates, restart the device, wait for stage 61, confirm
#   -Phase probe -Tag x                              memory layout, same-memory comparison, refusals
#   -Phase write                                     cross-path write test in the driver's test page, then restore
param(
    [Parameter(Mandatory)][ValidateSet('install', 'gate', 'probe', 'write')][string]$Phase,
    [string]$Tag = 'x',
    [int]$Mmio = 0,
    [int]$Vram = 0,
    [int]$VramWrite = 0,
    [string]$Package = 'C:\BC250\e08'
)

$ErrorActionPreference = 'Continue'
$out = Join-Path $Package 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = (Get-Date).ToString('HHmmss')
$log = Join-Path $out "$Phase-$Tag-$stamp.txt"
$cli = Join-Path $Package 'bc250kmd_cli.exe'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$hwid = 'PCI\VEN_1002&DEV_13FE'

function Say([string]$text) { $text | Tee-Object -FilePath $log -Append }
function Run-Cli { Say ("> bc250kmd_cli " + ($args -join ' ')); & $cli @args 2>&1 | ForEach-Object { Say "  $_" } }
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
    Say ("gates    EnableMmio {0}  EnableMmioWrite {1}  EnableVram {2}  EnableVramWrite {3}" -f $p.EnableMmio, $p.EnableMmioWrite, $p.EnableVram, $p.EnableVramWrite)
}
function Word([string]$path, [string]$offset) {
    # one VRAM word as text, or the refusal line
    $line = (& $cli vread $path $offset 2>&1) -join ' '
    if ($line -match '^read \S+ \S+ ([0-9A-F]{8})$') { $Matches[1] } else { "[$line]" }
}

Say ("time " + (Get-Date).ToString('s') + "  phase $Phase $Tag")
switch ($Phase) {
    'install' {
        pnputil /add-driver (Join-Path $Package 'bc250kmd.inf') /install 2>&1 | ForEach-Object { Say "$_" }
        if (Wait-Stage61) { & $cli confirm | ForEach-Object { Say "$_" } } else { Say 'stage 61 NOT reached' }
        State
    }
    'gate' {
        Set-ItemProperty $params -Name EnableMmio -Value $Mmio -Type DWord
        Set-ItemProperty $params -Name EnableMmioWrite -Value 0 -Type DWord
        Set-ItemProperty $params -Name EnableVram -Value $Vram -Type DWord
        Set-ItemProperty $params -Name EnableVramWrite -Value $VramWrite -Type DWord
        $gpu = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
        Say ("disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        Start-Sleep -Seconds 3
        Say ("enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        if (Wait-Stage61) { & $cli confirm | ForEach-Object { Say "$_" } } else { Say 'stage 61 NOT reached' }
        State
    }
    'probe' {
        Run-Cli memory
        Say '--- the same memory through both paths? three places inside the visible framebuffer, 64 words each'
        Run-Cli vcompare 0 64
        Run-Cli vcompare 400000 64
        Run-Cli vcompare 800000 64
        Say '--- the test page, both paths'
        Run-Cli vcompare F000000 16
        Say '--- top 2 MB of VRAM (physical path only; BAR0 does not reach it)'
        foreach ($o in '1FFE00000', '1FFE00004', '1FFF00000', '1FFFFFFFC') { Say ("phys 0x$o  " + (Word phys $o)) }
        Say '--- refusals: a read between the windows, a read beyond BAR0 through bar0, a write (test page and elsewhere)'
        Run-Cli vread phys 20000000
        Run-Cli vread bar0 10000000
        Run-Cli vwrite phys F000000 0
        Run-Cli vwrite phys 1000 0
        & $cli info 2>&1 | Select-String 'version|last stage|presents' | ForEach-Object { Say "$_" }
    }
    'write' {
        $a = 'F000000'; $b = 'F000FFC'
        $oldA = Word phys $a; $oldB = Word phys $b
        Say "before   $a = $oldA   $b = $oldB"
        if ($oldA -notmatch '^[0-9A-F]{8}$' -or $oldB -notmatch '^[0-9A-F]{8}$') { Say 'cannot read the test page, stopping'; break }
        Say '--- written through phys, read through bar0'
        Run-Cli vwrite phys $a 0BC25008
        Run-Cli vwrite phys $b F43DAFF7
        Say ("bar0 0x$a  " + (Word bar0 $a) + "    bar0 0x$b  " + (Word bar0 $b))
        Say '--- written through bar0, read through phys'
        Run-Cli vwrite bar0 $a 13FE1002
        Run-Cli vwrite bar0 $b EC01EFFD
        Say ("phys 0x$a  " + (Word phys $a) + "    phys 0x$b  " + (Word phys $b))
        Say '--- a write outside the test page must be refused'
        Run-Cli vwrite phys EFFFFFC 0
        Run-Cli vwrite phys F001000 0
        Say '--- restore'
        Run-Cli vwrite phys $a $oldA
        Run-Cli vwrite phys $b $oldB
        Say ("after    $a = " + (Word bar0 $a) + "   $b = " + (Word bar0 $b))
        & $cli info 2>&1 | Select-String 'version|last stage|presents' | ForEach-Object { Say "$_" }
    }
}
