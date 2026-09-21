# E15 on the target (bc250kmd 0.6.2: the undo's KIQ dequeue, SDMA fences, the compute dispatch), one phase per call,
# everything logged under C:\BC250\e15\out. Runs elevated (SSH session). E12's script with the fence modes of 0.6.2.
#
#   -Phase install                  new package, gates closed by the INF
#   -Phase gate -On 0|1             EnableMmio, EnableVram, EnableGart, EnablePsp, EnableGfx, EnableIh all to that value; restart the
#                                   device; confirm
#   -Phase sweep -Tag x             GC, MMHUB and MP0 register sweeps through bc250rd (the independent witness)
#   -Phase gart -Op plan|enable|restore     the GART command of bc250kmd
#   -Phase psp -Op plan|load|unload         the PSP command of bc250kmd
#   -Phase gfx -Op plan|run|fini|state -Stage N     the GFX command of bc250kmd (stages 1..7)
#   -Phase ih -Op plan|init|fini|state      the IH command of bc250kmd (interrupt counts, vectors seen)
#   -Phase fence -Op gfx|c0..c7|kiq|s0|s1 -Count N [-Mode noint|test|dispatch]
#                                   N fences on one ring (needs gfx run 6 or later; interrupts need ih init and stage 8);
#                                   test = SDMA ring test; dispatch = the memset shader with N workgroups on a compute ring
#   -Phase firmware                 sizes and SHA-256 of the firmware files the driver will read
param(
    [Parameter(Mandatory)][ValidateSet('install', 'gate', 'sweep', 'gart', 'psp', 'gfx', 'ih', 'fence', 'firmware')][string]$Phase,
    [string]$Op = 'plan',
    [string]$Tag = 'x',
    [int]$On = 0,
    [int]$Stage = 0,
    [int]$Count = 1,
    [ValidateSet('', 'noint', 'test', 'dispatch')][string]$Mode = '',
    [string]$Package = 'C:\BC250\e15'
)

$ErrorActionPreference = 'Continue'
$out = Join-Path $Package 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = (Get-Date).ToString('HHmmss')
$log = Join-Path $out "$Phase-$Op-$Tag-$stamp.txt"
$cli = Join-Path $Package 'bc250kmd_cli.exe'
$rd = 'C:\BC250\bc250rd\bc250rd_cli.exe'
$reglist = 'C:\BC250\bc250rd\reglist.txt'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$hwid = 'PCI\VEN_1002&DEV_13FE'
$firmware = 'C:\BC250\firmware'

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
    Say ("gates    EnableMmio {0}  EnableMmioWrite {1}  EnableVram {2}  EnableVramWrite {3}  EnableGart {4}  EnablePsp {5}  EnableGfx {6}  EnableIh {7}" -f `
        $p.EnableMmio, $p.EnableMmioWrite, $p.EnableVram, $p.EnableVramWrite, $p.EnableGart, $p.EnablePsp, $p.EnableGfx, $p.EnableIh)
    # The INF's .HW section runs only on a real install; without these values the device silently stays on the line.
    $msi = Get-ItemProperty ("HKLM:\SYSTEM\CurrentControlSet\Enum\{0}\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties" -f $gpu.InstanceId) -ErrorAction SilentlyContinue
    Say ("msi key  MSISupported {0}  MessageNumberLimit {1}" -f $(if ($null -eq $msi) { 'ABSENT' } else { $msi.MSISupported }), $msi.MessageNumberLimit)
}
function Alive { & $cli info 2>&1 | Select-String 'version|last stage|presents' | ForEach-Object { Say "$_" } }

Say ("time " + (Get-Date).ToString('s') + "  phase $Phase $Op $Tag")
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
        Set-ItemProperty $params -Name EnablePsp -Value $On -Type DWord
        Set-ItemProperty $params -Name EnableGfx -Value $On -Type DWord
        Set-ItemProperty $params -Name EnableIh -Value $On -Type DWord
        $gpu = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
        Say ("disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        Start-Sleep -Seconds 3
        Say ("enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        if (Wait-Stage61) { & $cli confirm | ForEach-Object { Say "$_" } } else { Say 'stage 61 NOT reached' }
        State
    }
    'sweep' {
        foreach ($ip in 'GC', 'MMHUB', 'MP0', 'NBIO', 'OSSSYS') {
            $file = Join-Path $out "sweep-$ip-$Tag-$stamp.log"
            & $rd sweep $reglist "$ip." > $file 2>&1
            Say ("sweep $ip exit $LASTEXITCODE, $((Get-Content $file | Measure-Object -Line).Lines) lines -> $file")
        }
        Alive
    }
    'firmware' {
        Get-ChildItem $firmware -Filter 'cyan_skillfish2_*.bin' | Sort-Object Name | ForEach-Object {
            Say ("{0,8}  {1}  {2}" -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name)
        }
    }
    'gfx' {
        if ($Stage -gt 0) { & $cli gfx $Op $Stage 2>&1 | ForEach-Object { Say "$_" } } else { & $cli gfx $Op 2>&1 | ForEach-Object { Say "$_" } }
        Say "exit code $LASTEXITCODE"
        Alive
    }
    'fence' {
        if ($Mode -ne '') { & $cli fence $Op $Count $Mode 2>&1 | ForEach-Object { Say "$_" } } else { & $cli fence $Op $Count 2>&1 | ForEach-Object { Say "$_" } }
        Say "exit code $LASTEXITCODE"
    }
    default {
        & $cli $Phase $Op 2>&1 | ForEach-Object { Say "$_" }
        Say "exit code $LASTEXITCODE"
        Alive
    }
}
