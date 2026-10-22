param(
    [Parameter(Mandatory)][string]$Tag,            # arm name, e.g. run1-ctx92k
    [Parameter(Mandatory)][string]$ServerArgsB64,  # llama-server arguments, UTF-8 base64 (no quoting traps)
    [Parameter(Mandatory)][string]$Client,         # client script file name, staged next to this script
    [string]$ClientArgsB64 = '',                   # client arguments, UTF-8 base64
    [string]$KitDir = 'C:\BC250\perf\kit',         # where the client scripts and perf-http.ps1 are staged
    [string]$ExePath = 'C:\BC250\llmvram\bin-upstream\llama-server.exe',  # the server build under test
    [string]$EnvB64 = '',                          # "K=V;K=V" set in the server child only, UTF-8 base64
    [string]$ExtraHashB64 = '',                    # ";"-separated extra files to hash into the record
    [int]$Port = 8780,
    [int]$Deadline = 300,                          # THE single supervised total bound, cleanup included
    [int]$WorkloadStop = 0,                        # stop server and client at this elapsed time (0 = Deadline)
    [int]$ServerReadyS = 120,                      # bound on waiting for /health after the server starts
    [int]$ProbeEvery = 4,                          # seconds between fresh Tctl probes
    [int]$ThermalStale = 12,                       # seconds without a good Tctl probe that stops the arm
    [double]$StartCeilC = 80,                      # Tctl must be under this before an arm starts
    [int]$CoolWaitS = 240                          # wait up to this long for that, OUTSIDE the arm deadline
)
# One bounded llama-server arm on unit A for the Performant Labs long-context comparison.
#
# Same structure and the same fail-closed thermal gate as scratch\seg0-cpu-write\lab\kit\seg0-arm2.ps1.
# Two differences, both because the workload here is a server plus a client, not one process:
#   * the server runs as a scheduled task in the logged-on user's session, because Vulkan needs that session,
#     while the client is an ordinary child process of this script in session 0: it only speaks HTTP to
#     127.0.0.1 and touches no GPU,
#   * the supervision loop has two phases, "loading" (waiting for /health, bounded by $ServerReadyS) and
#     "client". The thermal gate and the deadline run in both, so a model load that hangs is stopped by the
#     same rule as a client that hangs.
#
# Read-only on the disk apart from its own output directory under C:\BC250\perf\runs. Nothing is installed.
# No registry value, clock, fan, BIOS or TDR setting is written. The server binds 127.0.0.1 only.
# Thermal rule (2026-10-04): stop at once at Tctl >= 89 C, stop after 10 s held at >= 87 C.
$ErrorActionPreference = 'Stop'
$thermal = @('C:\BC250\seg0\thermal.ps1', (Join-Path $KitDir 'thermal.ps1'),
             (Join-Path $PSScriptRoot 'thermal.ps1')) |
           Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $thermal) { Write-Output 'REFUSED thermal.ps1 not found in C:\BC250\seg0 or the kit directory'; exit 7 }
. $thermal
'thermal_gate=' + $thermal
'thermal_gate_sha256=' + (Get-FileHash $thermal -Algorithm SHA256).Hash

$root = 'C:\BC250\perf'
$cli  = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$task = 'BC250-PerfLlm'
$exe  = $ExePath
$curl = 'C:\Windows\system32\curl.exe'
$ProcPattern = 'llama-*'

$SrvArgs   = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ServerArgsB64))
$ClientArg = if ($ClientArgsB64) { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ClientArgsB64)) } else { '' }
$EnvSet    = if ($EnvB64)        { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($EnvB64)) }        else { '' }
$Extra     = if ($ExtraHashB64)  { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ExtraHashB64)) }  else { '' }
if ($WorkloadStop -le 0 -or $WorkloadStop -gt $Deadline) { $WorkloadStop = $Deadline }

$out = Join-Path $root ('runs\' + $Tag + '-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))
New-Item -ItemType Directory -Force $out | Out-Null
'out=' + $out
'tag=' + $Tag
'start_utc=' + (Get-Date).ToUniversalTime().ToString('s') + 'Z'
'boot_utc=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s') + 'Z'
'deadline_s=' + $Deadline
'workload_stop_s=' + $WorkloadStop
'server_ready_bound_s=' + $ServerReadyS
'port=' + $Port
'server_args=' + $SrvArgs
'client=' + $Client
'client_args=' + $ClientArg
'child_env=' + $EnvSet

# ---------------------------------------------------------------- bounded Tctl probe
$probeOut = Join-Path $out 'probe.out'
$probeErr = Join-Path $out 'probe.err'
$probeLog = New-Object System.Collections.Generic.List[string]
function Probe-Tctl {
    if (-not (Test-Path $cli)) { return $null }
    try {
        $p = Start-Process -FilePath $cli -ArgumentList 'dpm', '1', '250' -NoNewWindow -PassThru `
                           -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
    } catch { return $null }
    if (-not $p.WaitForExit(5000)) {
        try { $p.Kill() } catch { }
        return $null
    }
    $text = Get-Content $probeOut -ErrorAction SilentlyContinue
    $hit = $text | Where-Object { $_ -match '([0-9]+(\.[0-9]+)?)\s*C busy' } | Select-Object -Last 1
    if (-not $hit) { return $null }
    if ($hit -match '([0-9]+(\.[0-9]+)?)\s*C busy') { return [double]$Matches[1] }
    return $null
}

# ---------------------------------------------------------------- gates before anything starts
if (-not (Test-Path $exe))  { 'REFUSED llama-server not staged: ' + $exe; exit 3 }
if (-not (Test-Path $curl)) { 'REFUSED curl.exe not found; the client speaks HTTP through it'; exit 3 }
$clientPath = Join-Path $KitDir $Client
if (-not (Test-Path $clientPath)) { 'REFUSED client script not staged: ' + $clientPath; exit 3 }
'server=' + $exe
'server_sha256=' + (Get-FileHash $exe -Algorithm SHA256).Hash
'client_sha256=' + (Get-FileHash $clientPath -Algorithm SHA256).Hash
foreach ($f in ($Extra -split ';' | Where-Object { $_ })) {
    if (Test-Path $f) {
        'extra_sha256=' + (Get-FileHash $f -Algorithm SHA256).Hash + ' bytes=' + (Get-Item $f).Length + ' path=' + $f
    } else { 'REFUSED extra file missing: ' + $f; exit 3 }
}

$gpu = @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -lt 1) { 'REFUSED no BC-250 display adapter'; exit 3 }
'gpu_status=' + $gpu[0].Status
'driver_version=' + (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
$sysIcd = 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll'
if (Test-Path $sysIcd) { 'system_icd_sha256=' + (Get-FileHash $sysIcd -Algorithm SHA256).Hash }

$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$pp = Get-ItemProperty $par -ErrorAction SilentlyContinue
'cu_mode=' + $pp.CuMode + ' cu_last_applied=' + $pp.CuModeLastApplied
'dpm_mode=' + $pp.DpmMode + ' dpm_max_mhz=' + $pp.DpmMaxMHz + ' dpm_idle_mhz=' + $pp.DpmIdleMHz
if ($pp.CuMode -ne 40 -or $pp.CuModeLastApplied -ne 40) {
    'REFUSED CU gate: want 40/40, have ' + $pp.CuMode + '/' + $pp.CuModeLastApplied; exit 3
}
if (Get-Process $ProcPattern -ErrorAction SilentlyContinue) { 'REFUSED a llama process is already running'; exit 3 }
# No -ErrorAction SilentlyContinue: a query that cannot answer must stop the arm, not read as "nothing running".
$standing = @($task, 'BC250 monitor overlay', 'BC250 net watchdog')
$busy = Get-ScheduledTask |
        Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' -and $standing -notcontains $_.TaskName }
if ($busy) { 'REFUSED competing task(s): ' + (($busy | ForEach-Object TaskName) -join ','); exit 3 }

# Fail closed on missing Tctl. The cool-down wait runs before the stopwatch, so it never eats the deadline
# and never counts against the llama run-time budget.
$cool = [Diagnostics.Stopwatch]::StartNew()
$tc0 = $null
do {
    $tc0 = $null
    for ($i = 0; $i -lt 4 -and $null -eq $tc0; $i++) { $tc0 = Probe-Tctl }
    if ($null -eq $tc0) { 'REFUSED no Tctl reading from the KMD CLI (fail closed)'; exit 4 }
    if ($tc0 -lt $StartCeilC) { break }
    'cooling: tctl={0} >= {1} C, waited {2:N0} s' -f $tc0, $StartCeilC, $cool.Elapsed.TotalSeconds
    Start-Sleep -Seconds 5
} while ($cool.Elapsed.TotalSeconds -lt $CoolWaitS)
'cooldown_wait_s={0:N0}' -f $cool.Elapsed.TotalSeconds
'tctl_before=' + $tc0
if ($tc0 -ge $StartCeilC) { 'REFUSED Tctl ' + $tc0 + ' >= ' + $StartCeilC + ' C after the cool-down wait'; exit 4 }

function Capture([string]$file, [string[]]$cliArgs) {
    try { & $cli @cliArgs 2>&1 | Set-Content (Join-Path $out $file) }
    catch { ('capture failed: ' + $_.Exception.Message) | Set-Content (Join-Path $out $file) }
}
Capture 'health-before.txt' @('health', 'read')
Capture 'vram-before.txt'   @('vram')
Capture 'dpm-before.txt'    @('dpm', '2', '500')
'vram_before:'; Get-Content (Join-Path $out 'vram-before.txt') | Select-Object -First 8
$osB = Get-CimInstance Win32_OperatingSystem
'avail_phys_mib_before={0:N0}' -f ($osB.FreePhysicalMemory / 1024)

# ---------------------------------------------------------------- the server
# Vulkan needs the logged-on user's interactive session; an SSH session lives in session 0 with no window.
# A scheduled task in that session is how every kit of this lab runs a GPU client.
$envLines = ($EnvSet -split ';' | Where-Object { $_ -match '=' } | ForEach-Object { "set $_" }) -join "`r`n"
$runCmd = @"
@echo off
set MESA_SHADER_CACHE_DISABLE=false
$envLines
"$exe" $SrvArgs --host 127.0.0.1 --port $Port < NUL > "$out\server.out" 2> "$out\server.err"
echo %ERRORLEVEL% > "$out\server.exit"
"@
$runCmd | Set-Content "$out\run.cmd" -Encoding ASCII
"& cmd.exe /c $out\run.cmd`n[IO.File]::WriteAllText('$out\worker.exit', [string]`$LASTEXITCODE)" |
    Set-Content "$out\worker.ps1" -Encoding ASCII
'command_line=' + $exe + ' ' + $SrvArgs + ' --host 127.0.0.1 --port ' + $Port

$user = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $user) { 'REFUSED no interactive user logged on (Vulkan needs that session)'; exit 3 }
$action = New-ScheduledTaskAction -Execute conhost.exe `
            -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File $out\worker.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

$sampler = $null
$client = $null
$clientPid = $null
$stop = $null
$gate = $null
$ready = $null
$finished = $false
$phase = 'loading'
$sw = [Diagnostics.Stopwatch]::StartNew()
$sizeLog = New-Object System.Collections.Generic.List[string]
try {
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null

# The 1 s record of clock, temperature and busy percent. EVIDENCE only; no decision reads it.
try {
    $sampler = Start-Process -FilePath $cli -ArgumentList 'dpm', ([string]($WorkloadStop + 5)), '1000' `
                             -RedirectStandardOutput "$out\dpm.txt" -NoNewWindow -PassThru
} catch { 'sampler_start_failed (record only, the probe decides)' }

$sw.Restart()
Start-ScheduledTask $task
$gate = New-ThermalGate -StartedWith $tc0 -Now ([datetime]::UtcNow) -StaleS $ThermalStale
$nextProbe = [datetime]::UtcNow
do {
    Start-Sleep -Milliseconds 1500
    $el = $sw.Elapsed.TotalSeconds
    $len = (Get-Item "$out\client.out" -ErrorAction SilentlyContinue).Length
    $sizeLog.Add(('{0:N1} {1} {2}' -f $el, $phase, $len))

    $tc = $null
    if ([datetime]::UtcNow -ge $nextProbe) {
        $nextProbe = [datetime]::UtcNow.AddSeconds($ProbeEvery)
        $tc = Probe-Tctl
        $probeLog.Add(('{0:N1} {1}' -f $el, $(if ($null -eq $tc) { 'STALE' } else { $tc })))
        $gate = Update-ThermalGate -Gate $gate -Tctl $tc -Now ([datetime]::UtcNow)
    } else {
        $gate = Update-ThermalGate -Gate $gate -Tctl $null -Now ([datetime]::UtcNow) -Attempted $false
    }
    if ($gate.Stop) { $stop = $gate.Stop; break }
    if ($el -ge $WorkloadStop) { $stop = "WORKLOAD-STOP $WorkloadStop s"; break }

    if ($phase -eq 'loading') {
        $h = ''
        try { $h = (& $curl -s -m 2 "http://127.0.0.1:$Port/health" 2>$null) -join '' } catch { $h = '' }
        if ($h -match '"status"\s*:\s*"ok"') {
            $ready = $el
            'server_ready_s={0:N1}' -f $ready
            $client = Start-Process -FilePath 'powershell.exe' -PassThru -NoNewWindow `
                -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $clientPath,
                              '-Port', $Port, '-Out', $out, '-ArgsB64', ('"' + $ClientArgsB64 + '"'),
                              '-Remaining', ([int]($WorkloadStop - $el - 12)) `
                -RedirectStandardOutput "$out\client.out" -RedirectStandardError "$out\client.err"
            $clientPid = $client.Id
            'client_pid_from_process_object=' + $clientPid
            $phase = 'client'
        } elseif (Test-Path "$out\worker.exit") {
            $stop = 'SERVER-EXITED before /health was ok'; break
        } elseif ($el -ge $ServerReadyS) {
            $stop = "SERVER-NOT-READY in $ServerReadyS s"; break
        }
    } else {
        # NOT $client.HasExited. The object Start-Process -PassThru returns has no live handle here, its
        # HasExited getter throws, and PowerShell swallows a property getter's exception and yields $null,
        # so the test was silently always false and the smoke arm ran to its stop after the client had
        # finished. Two independent signals instead: the sentinel the client writes last, and the pid.
        if (-not $clientPid) {
            # The client states its own pid at startup, because the Start-Process object's Id came back
            # empty on this machine. Read it once, as soon as it appears.
            $fromFile = ((Get-Content "$out\client.pid" -ErrorAction SilentlyContinue) -join '').Trim()
            if ($fromFile -match '^\d+$') {
                $clientPid = [int]$fromFile
                'client_pid=' + $clientPid
            }
        }
        $gone = $false
        if ($clientPid) {
            try { $null = Get-Process -Id $clientPid -ErrorAction Stop } catch { $gone = $true }
        }
        if ((Test-Path "$out\client.done") -or $gone) {
            $finished = $true
            $stop = $null
            break
        }
    }
} while ($true)
} finally {
# ---------------------------------------------------------------- cleanup, on every exit path
if (-not $stop -and -not $finished) { $stop = 'ERROR in supervision (see the PowerShell error after this line)' }
# The client first: it must never outlive the server it talks to. Killed by pid, for the same reason the
# loop watches the pid: the Start-Process object's own members are not reliable here.
if ($clientPid) {
    $alive = $null
    try { $alive = Get-Process -Id $clientPid -ErrorAction Stop } catch { }
    if ($alive) {
        try { Stop-Process -Id $clientPid -Force -ErrorAction SilentlyContinue } catch { }
        for ($i = 0; $i -lt 6; $i++) {
            Start-Sleep -Milliseconds 500
            $still = $null
            try { $still = Get-Process -Id $clientPid -ErrorAction Stop } catch { }
            if (-not $still) { break }
        }
    }
}
'client_done_sentinel=' + ((Get-Content "$out\client.done" -ErrorAction SilentlyContinue) -join '').Trim()
# The VRAM reading has to be taken while the server still holds its buffers: that is the fit evidence.
Capture 'vram-alive.txt' @('vram')
'vram_while_server_alive:'; Get-Content (Join-Path $out 'vram-alive.txt') -ErrorAction SilentlyContinue | Select-Object -First 8
Stop-ScheduledTask $task -ErrorAction SilentlyContinue
Get-Process $ProcPattern -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
for ($i = 0; $i -lt 20 -and (Get-Process $ProcPattern -ErrorAction SilentlyContinue) -and
     $sw.Elapsed.TotalSeconds -lt ($Deadline - 4); $i++) {
    Start-Sleep -Seconds 1
    Get-Process $ProcPattern -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
'llama_lifetime_s={0:N1}' -f $sw.Elapsed.TotalSeconds
if ($stop) { 'STOPPED ' + $stop }
Unregister-ScheduledTask $task -Confirm:$false -ErrorAction SilentlyContinue
if ($sampler -and -not $sampler.HasExited) { Stop-Process -Id $sampler.Id -Force -ErrorAction SilentlyContinue }
}
$elapsed = $sw.Elapsed.TotalSeconds
$tcMax = if ($gate) { $gate.MaxC } else { $null }
$leftover = @(Get-Process $ProcPattern -ErrorAction SilentlyContinue)
'leftover_workload_processes=' + $leftover.Count
if ($leftover.Count) { $leftover | ForEach-Object { 'LEFTOVER ' + $_.Name + ' pid ' + $_.Id } }
$sizeLog | Set-Content "$out\client-size.txt"
$probeLog | Set-Content "$out\tctl-probes.txt"
Remove-Item $probeOut, $probeErr -ErrorAction SilentlyContinue

$tc1 = Probe-Tctl
Capture 'health-after.txt' @('health', 'read')
Capture 'dpm-after.txt'    @('dpm', '2', '500')

'end_utc=' + (Get-Date).ToUniversalTime().ToString('s') + 'Z'
'total_elapsed_after_cleanup_s={0:N1}' -f $elapsed
'supervisor_outcome=' + $(if ($finished) { 'client_finished' } else { 'STOPPED ' + $stop })
'server_ready_s=' + $(if ($null -ne $ready) { '{0:N1}' -f $ready } else { 'never' })
'server_exit=' + ((Get-Content "$out\server.exit" -ErrorAction SilentlyContinue) -join '').Trim()
'tctl_after=' + $tc1
'tctl_max_probed=' + $tcMax
'tctl_probes=' + $probeLog.Count + ' stale=' + @($probeLog | Where-Object { $_ -match 'STALE' }).Count
'client_out_bytes=' + (Get-Item "$out\client.out" -ErrorAction SilentlyContinue).Length
'server_err_bytes=' + (Get-Item "$out\server.err" -ErrorAction SilentlyContinue).Length
$osA = Get-CimInstance Win32_OperatingSystem
'avail_phys_mib_after={0:N0}' -f ($osA.FreePhysicalMemory / 1024)
'health_after:'; Get-Content (Join-Path $out 'health-after.txt') | Select-Object -First 12

if ($sampler) {
    $mhz = Get-Content "$out\dpm.txt" -ErrorAction SilentlyContinue | ForEach-Object { if ($_ -match 'dpm\s+(\d+) MHz') { [int]$Matches[1] } }
    $tcs = Get-Content "$out\dpm.txt" -ErrorAction SilentlyContinue | ForEach-Object { if ($_ -match '([0-9.]+) C busy') { [double]$Matches[1] } }
    if ($mhz) { 'dpm_samples=' + $mhz.Count + ' gfx_mhz min/avg/max=' + ($mhz | Measure-Object -Minimum -Average -Maximum |
        ForEach-Object { '{0}/{1:N0}/{2}' -f $_.Minimum, $_.Average, $_.Maximum }) }
    if ($mhz) { 'gfx_mhz_at_2000=' + @($mhz | Where-Object { $_ -ge 2000 }).Count }
    if ($tcs) { 'dpm_tctl_max=' + ($tcs | Measure-Object -Maximum).Maximum }
}

# The llama buffer lines are the fit evidence; they go to stderr.
'--- server.err buffer and offload lines'
Get-Content "$out\server.err" -ErrorAction SilentlyContinue | Where-Object {
    $_ -match 'buffer size|model buffer|KV self|KV cache|recurrent|compute buffer|graph|offload|load_tensors|n_ctx|device lost|ErrorOutOfDeviceMemory|failed|error|cannot|unable'
} | Select-Object -First 90
'--- server.err tail'
Get-Content "$out\server.err" -ErrorAction SilentlyContinue | Select-Object -Last 25
'--- client.out'
Get-Content "$out\client.out" -ErrorAction SilentlyContinue | Select-Object -Last 120
'--- client.err'
Get-Content "$out\client.err" -ErrorAction SilentlyContinue | Select-Object -Last 30

if ($stop -like 'THERMAL*') { exit 5 }
if (-not $finished) { exit 6 }
exit 0
