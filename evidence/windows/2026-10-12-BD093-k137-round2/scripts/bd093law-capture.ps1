# BD-093 state B, round 2: the capture script of the new phases. It extends the 2026-10-12 kit
# (scratch\bd093\kit\bd093-capture.ps1, which keeps the state, cpu, gpu and control phases unchanged and
# is still used by the runner) with the phases the mechanism document asks for:
#
#   law       the instruction rate law probe: selftest, CPUID, the full suite, the measured sibling map,
#             the cache sweep. Read only: it runs instructions and reads QPC and RDTSC.
#   smu       the SMU side A/B inside one boot: `cpu readback` (message 0x43 per core and the 0x3D feature
#             mask, decoded bit by bit), `dpm` with its metrics tail, `health read`, and the start-log
#             witnesses. Getters only.
#   pmc       whether this Windows can count retired instructions at all (xperf, wpr, Get-Counter). A pure
#             availability probe: it decides whether the dispatch counter route exists without a driver.
#   clockcap  the ground truth injection: a real CPU clock ceiling of 2800 MHz through the KMD's own
#             allow-listed setter, with the law measured under it and the ceiling released again. The one
#             step of this script that writes anything, and it writes through the KMD as the single SMU
#             owner, inside the owner's pre-approved CPU clock space, with the driver's own trial window as
#             the dead-man revert.
#   load      the GPU load coupling arm: the law measured while the GPU runs a fixed load, so state A can
#             be taken at a GFX island power at or above state B's idle island power.
#   force     F0': the forced GFX operating point re-applied and moved through the KMD's own DPM floor in
#             state B (RequestGfxclk plus ForceGfxVid, both already on the shim allow-list), then released.
#             The nearest causal lever to MECHANISM.md F0 that needs no driver change.
#
#   powershell -ExecutionPolicy Bypass -File bd093law-capture.ps1 -Phase law -State A
#
# Nothing here writes a register, sends a message outside the CLI's existing allow-listed set, touches
# BIOS, NVRAM or CMOS, or restarts anything. Every phase appends its flushed lines to
# C:\BC250\tmp\bd093law\<state>-<phase>.txt.
param(
    [ValidateSet('law','lawmap','smu','pmc','clockcap','load','force')][string]$Phase = 'law',
    [ValidateSet('A','B','B2')][string]$State = 'A',
    [string]$Tag = '',
    [int]$Ms = 50,
    [int]$Cpu = 2,
    [string]$Work = 'C:\BC250\tmp\bd093law',
    # Only the development-PC dry run passes -Cli, pointing it at a stub that prints canned CLI output, so
    # that the temperature, island-power, feature-mask and gate paths are exercised with no lab. The lab
    # always uses the default.
    [string]$Cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe',
    [string]$Membw = 'C:\BC250\vkmembw\vkmembw.exe',
    [string]$GpuLoad = 'C:\BC250\gpuload\gpuload.exe'
)

$ErrorActionPreference = 'Continue'
$cli = $Cli
$law = Join-Path $Work 'bd093law.exe'
$probe = Join-Path $Work 'bd093probe.exe'
$membw = $Membw
$gpuload = $GpuLoad
$log = Join-Path $Work ($State + '-' + $Phase + '.txt')

if (-not (Test-Path $Work)) { New-Item -ItemType Directory -Path $Work -Force | Out-Null }

# Say writes to the console, not to the pipeline: in PowerShell a function's output stream is its return
# value, and a step that both reports and returns a number would hand its caller its own log lines.
function Say([string]$text) {
    $line = (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff') + ' ' + $State + '/' + $Phase + ' ' + $text
    [Console]::Out.WriteLine($line)
    [Console]::Out.Flush()
    Add-Content -Path $log -Value $line -Encoding Ascii
}

function Num($value, [int]$digits = 1) {
    # Invariant formatting: this Windows may be pl-PL, where -f writes a decimal comma into the evidence.
    return ([double]$value).ToString('F' + $digits, [System.Globalization.CultureInfo]::InvariantCulture)
}

# Every external call of this script has a wall-clock bound of its own, the CLI calls included. The ssh
# timeout kills only the client, so an unbounded child would keep the core, the SMU mailbox or the GPU after
# the runner has given up. The two temporary files are reused by every call of this phase, so a phase of
# twenty CLI calls still leaves two files behind and not forty.
function Run([string]$exe, [string[]]$exeArgs, [int]$keep = 40, [int]$boundS = 20) {
    if (-not (Test-Path $exe)) { return @('MISSING ' + $exe) }
    $so = Join-Path $Work ($State + '-' + $Phase + '-run.out')
    $se = Join-Path $Work ($State + '-' + $Phase + '-run.err')
    $p = if ($exeArgs.Count -gt 0) {
        Start-Process -FilePath $exe -ArgumentList $exeArgs -NoNewWindow -PassThru `
                      -RedirectStandardOutput $so -RedirectStandardError $se
    } else {
        Start-Process -FilePath $exe -NoNewWindow -PassThru `
                      -RedirectStandardOutput $so -RedirectStandardError $se
    }
    $null = $p.Handle                      # PS 5.1: without this ExitCode is not populated
    if (-not $p.WaitForExit($boundS * 1000)) {
        [Console]::Out.WriteLine('run BOUND ' + $boundS + ' s reached by ' + (Split-Path -Leaf $exe) +
                                 ': killing it, this call is void')
        try { $p.Kill() } catch { }
        $p.WaitForExit(5000) | Out-Null
    }
    $lines = @()
    foreach ($f in @($so, $se)) {
        if (Test-Path $f) { $lines += @(Get-Content $f -ErrorAction SilentlyContinue | ForEach-Object { [string]$_ }) }
    }
    return @($lines | Select-Object -First $keep)
}

# A child with a wall-clock bound of its own: the ssh timeout kills only the client, so without this a hung
# probe keeps the core or the GPU after the runner has given up.
function RunBounded([string]$exe, [string[]]$exeArgs, [int]$boundS, [string]$name) {
    $r = New-Object psobject
    $r | Add-Member NoteProperty Lines @()
    $r | Add-Member NoteProperty Code -2
    $r | Add-Member NoteProperty Seconds 0.0
    if (-not (Test-Path $exe)) { $r.Lines = @('MISSING ' + $exe); return $r }
    $so = Join-Path $Work ($State + '-' + $name + '.out')
    $se = Join-Path $Work ($State + '-' + $name + '.err')
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = if ($exeArgs.Count -gt 0) {
        Start-Process -FilePath $exe -ArgumentList $exeArgs -NoNewWindow -PassThru `
                      -RedirectStandardOutput $so -RedirectStandardError $se
    } else {
        Start-Process -FilePath $exe -NoNewWindow -PassThru `
                      -RedirectStandardOutput $so -RedirectStandardError $se
    }
    $null = $p.Handle                      # PS 5.1: without this ExitCode is not populated
    if (-not $p.WaitForExit($boundS * 1000)) {
        Say ($name + ' BOUND ' + $boundS + ' s reached: killing it, this step is void')
        try { $p.Kill() } catch { Say ($name + ' kill failed ' + $_.Exception.Message) }
        $p.WaitForExit(5000) | Out-Null
        $r.Code = -1
    } else { $r.Code = $p.ExitCode }
    $sw.Stop()
    $r.Seconds = $sw.Elapsed.TotalSeconds
    $lines = @()
    foreach ($f in @($so, $se)) {
        if (Test-Path $f) { $lines += @(Get-Content $f -ErrorAction SilentlyContinue | ForEach-Object { [string]$_ }) }
    }
    $r.Lines = @($lines)
    return $r
}

function Emit([string]$prefix, [string[]]$lines) {
    if ($null -eq $lines -or $lines.Count -eq 0) { Say ($prefix + ' (no output)'); return }
    foreach ($l in $lines) { Say ($prefix + ' ' + $l.Trim()) }
}

function CliState([string]$what) {
    Emit ('dpm ' + $what) (Run $cli @('dpm') 8)
    Emit ('cpu ' + $what) (Run $cli @('cpu') 12)
}

# Tctl now, and whether the reading is live. Three sources, in this order:
#
#   1. `cli telemetry`, which is the ONLY verb that prints the machine-readable field
#      (bc250kmd_cli.c:1377, inside Telemetry(), not inside the `dpm` verb) and the only one that marks a
#      value the driver could not refresh: it appends "(stale)" when BC250_DPM_FLAG_TEMPERATURE is clear.
#   2. the `dpm` verb's own sample line, "... VID 116)  60.6 C busy   0.0% ..." (DpmPrint at :2771), whose
#      stale marker sits right after the C.
#   3. the `cpu` line (:3378, "%ld.%01ld C"), which is the CPU surface's own snapshot and may be much older
#      than the rest; the CLI says so itself when BC250_CPU_FLAG_TEMP_VALID is clear.
#
# A stale value is reported as stale and not as a temperature: the first version of this script parsed
# `temperature_c=` out of the `dpm` verb, which never prints it, and so every thermal gate of the arm would
# have been decided by the CPU surface's cached snapshot.
$script:TempSource = 'none'
$script:TempStale = $true

function TempC() {
    $script:TempSource = 'none'; $script:TempStale = $true
    foreach ($l in (Run $cli @('telemetry') 10 25)) {
        $m = [regex]::Match([string]$l, 'temperature_c=([0-9]+\.[0-9]+)(\(stale\))?')
        if ($m.Success) {
            $script:TempSource = 'telemetry'; $script:TempStale = $m.Groups[2].Success
            return [double]::Parse($m.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture)
        }
    }
    foreach ($l in (Run $cli @('dpm') 8 25)) {
        $m = [regex]::Match([string]$l, '\)\s+([0-9]+\.[0-9])\s*(\(stale\))?\s+busy')
        if ($m.Success) {
            $script:TempSource = 'dpm-sample'; $script:TempStale = $m.Groups[2].Success
            return [double]::Parse($m.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture)
        }
    }
    foreach ($l in (Run $cli @('cpu') 12 25)) {
        $m = [regex]::Match([string]$l, '([0-9]+\.[0-9])\s*C\s*$')
        if ($m.Success) {
            $script:TempSource = 'cpu-snapshot'; $script:TempStale = $true
            return [double]::Parse($m.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture)
        }
    }
    return -1.0
}

# -Require is passed by every phase that writes to the hardware. Such a phase must not start without a live
# temperature: its own release rule (85 C in `force`) could never fire, and the 87 C rule would rest on a
# cached number. The read-only phases warn instead, because they write nothing and the KMD's own thermal
# policy stays in charge of the GPU.
function ThermalGate([double]$limit = 80.0, [switch]$Require) {
    $t = TempC
    Say ('thermal tctl_c=' + (Num $t) + ' gate_c=' + (Num $limit) + ' source=' + $script:TempSource +
         ' live=' + (& { if ($script:TempStale) { 'no' } else { 'yes' } }))
    if ($t -lt 0 -or $script:TempStale) {
        if ($Require) {
            Say 'thermal NO LIVE READING: this phase writes to the hardware and refuses to start without one'
            return $false
        }
        Say 'thermal no live reading: this read-only phase continues, the lead watches the overlay'
        return $true
    }
    if ($t -ge $limit) { Say ('thermal ABOVE GATE: this phase stops here, cool down first'); return $false }
    return $true
}

# The GFX island power in watts, from the `dpm` verb's power line ("power  35.9 W avg  37.2 W (gfx  9.6 W
# soc  0.7 W)", DpmPrintPower at bc250kmd_cli.c:2765). The same function prints "power n/a" or "power ?"
# when the SMU table is not fresh, so a missing value is returned as -1 and never as zero watts.
function IslandW([string[]]$lines) {
    $w = -1.0
    foreach ($l in $lines) {
        $m = [regex]::Match([string]$l, '\(gfx\s+([0-9]+\.[0-9])\s*W')
        if ($m.Success) { $w = [double]::Parse($m.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture) }
    }
    return $w
}

# The 32 feature bits of this firmware, in the order of
# ref\linux-src\drivers\gpu\drm\amd\pm\swsmu\inc\pmfw_if\smu_v11_8_pmfw.h:32-63 (bit 0 CCLK_CONTROLLER at
# :32, bit 31 DLDO at :63). The answer of message 0x3D is 32 bits wide, so bits 32 and up
# (MEAS_DRAM_BLACKOUT, CC1, PPT, STAPM, CSTATE_BOOST) are not in it and the decode says so instead of
# inventing them.
$FeatureNames = @(
    'CCLK_CONTROLLER','GFXCLK_EFFT_FREQ','DATA_CALCULATION','THERMAL','PLL_POWER_DOWN','FCLK_DPM',
    'GFX_DPM','DS_GFXCLK','DS_SOCCLK','DS_LCLK','CORE_CSTATES','G6_SSC','RM','SOC_DPM','DS_SMNCLK',
    'DS_MP1CLK','DS_MP0CLK','MGCG','DS_FUSE_SRAM','GFX_CKS','FP_THROTTLING','PROCHOT','CPUOFF',
    'UMC_THROTTLE','DF_THROTTLE','DS_MP3CLK','DS_SHUBCLK','TDC','UMC_CAL_SHARING','DFLL_BTC_CALIBRATION',
    'EDC','DLDO')

# A REFUSED `cpu readback` still prints a `features 0x...` line, because CpuWrite falls back to
# CpuPrint(&before), the start-time snapshot (bc250kmd_cli.c:3453). That snapshot is not a fresh 0x3D answer
# and must never be decoded as one, so the decode needs the success marker of the same call: CpuWrite prints
# "cpu readback: done, serial <n>" at :3456 only when the message went through.
function DecodeFeatures([string[]]$lines) {
    $sent = $false
    foreach ($l in $lines) { if ([regex]::IsMatch([string]$l, '^cpu readback: done,')) { $sent = $true } }
    if (-not $sent) {
        Say ('features REFUSED: this cpu readback did not send 0x3D, so the features value in its output is' +
             ' the start-time snapshot and is not decoded')
        return
    }
    $value = $null
    foreach ($l in $lines) {
        $m = [regex]::Match([string]$l, 'features 0x([0-9A-Fa-f]{8})')
        if ($m.Success) { $value = [uint32]('0x' + $m.Groups[1].Value) }
    }
    if ($null -eq $value) { Say 'features NOT FOUND in the readback output: this read decides nothing'; return }
    $on = @(); $off = @()
    for ($i = 0; $i -lt 32; $i++) {
        $name = $FeatureNames[$i] + '(' + $i + ')'
        if (($value -band ([uint32]1 -shl $i)) -ne 0) { $on += $name } else { $off += $name }
    }
    $hex = $value.ToString('X8')
    Say ('features value=0x' + $hex + ' bits=32 of 64 (bits 32..63 are not in this answer)')
    Say ('features on= ' + ($on -join ' '))
    Say ('features off= ' + ($off -join ' '))
    # The comparison is on the hex text. `[uint32]0xDD602C7D` throws in PowerShell 5.1 and in 7: the literal
    # is parsed as Int32 (-580899715) and the cast then fails, which left this line empty and printed an
    # error record instead of a verdict.
    Say ('features record_2026_09=0xDD602C7D same_as_record=' +
         (& { if ($hex -eq 'DD602C7D') { 'yes' } else { 'NO' } }) +
         ' (the record holds TWO samples with this same value, one from a K137 state and one from an' +
         ' apparently healthy one, so an identical mask is the expected answer, not a surprise)')
}

# The single-number state meter of the old kit, kept so every phase of both kits speaks one language.
function QuickMeter([int]$cpu, [int]$ms) {
    $lines = Run $probe @('kernels', [string]$cpu, [string]$ms, '1') 24
    $rate = 0.0
    foreach ($l in $lines) {
        $m = [regex]::Match([string]$l, 'name=dep_mix .*ops_per_us=([0-9.]+)')
        if ($m.Success) { $rate = [double]$m.Groups[1].Value }
    }
    Say ('meter dep_mix_ops_per_us=' + (Num $rate) + ' (reference: state A 575-582, state B 253-260)')
    return $rate
}

# The law probe's short suite, with the two lines the plan reads first pulled out of the output.
# The bound is passed by the caller, and every phase keeps the SUM of its children's bounds under its own
# ssh bound: otherwise the runner gives up while a child still holds the core (the trap the 2026-10-12 kit
# fixed for vkmembw). The quick suite measures about 8 to 12 s, so 35 to 40 s is four times the headroom.
function LawQuick([string]$what, [int]$boundS = 40) {
    $r = RunBounded $law @('quick', [string]$Cpu, [string]$Ms) $boundS ($Phase + '-quick-' + $what)
    Emit ('law ' + $what) ($r.Lines | Select-Object -First 60)
    Say ('law ' + $what + ' exit=' + $r.Code + ' seconds=' + (Num $r.Seconds))
    $clock = 0.0; $k = 0.0
    foreach ($l in $r.Lines) {
        $m = [regex]::Match([string]$l, 'imul16_mhz=([0-9.]+)')
        if ($m.Success -and $clock -eq 0.0) { $clock = [double]$m.Groups[1].Value }
        $m2 = [regex]::Match([string]$l, '^slope_imm8 .*slope_cyc_per_instr=([0-9.]+)')
        if ($m2.Success) { $k = [double]$m2.Groups[1].Value }
    }
    Say ('law ' + $what + ' SUMMARY clock_mhz=' + (Num $clock) + ' k_cyc_per_instr=' + (Num $k 4) +
         ' (state A reference: clock about 3489, k about 0.23; state B 2026-10-12: clock about 2790, k about 1.1)')
    $o = New-Object psobject
    $o | Add-Member NoteProperty Clock $clock
    $o | Add-Member NoteProperty K $k
    $o | Add-Member NoteProperty Code $r.Code
    return $o
}

# Every exit of a phase goes through here, so an `END phase=` line always closes the log. A phase that
# stops early is a result ("this step decides nothing"), and the reader must be able to tell it from a
# script that died.
function EndExit([int]$code) {
    Say ('END phase=' + $Phase + ' state=' + $State + ' exit=' + $code)
    exit $code
}

Say ('BEGIN tag=' + $Tag + ' host=' + $env:COMPUTERNAME + ' phase=' + $Phase + ' state=' + $State +
     ' ms=' + $Ms + ' cpu=' + $Cpu)
Say ('boot lastboot=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s') +
     ' uptime_s=' + [int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds)
$gpuDev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
if ($null -ne $gpuDev) { Say ('gpu device status=' + $gpuDev.Status + ' instance=' + $gpuDev.InstanceId) }
else { Say 'gpu device NOT FOUND' }

switch ($Phase) {

'law' {
    # About 40 s measured, 152 s of child bounds, inside a 175 s call: the sum of the inner bounds must stay
    # under the ssh bound, or the runner would give up while a child still held the core.
    # Its own positive control runs first, so a null result cannot be read as a measurement.
    if (-not (Test-Path $law)) { Say ('FATAL bd093law.exe missing at ' + $law); EndExit 2 }
    # The law suite holds one core at its full issue rate for about 40 s, so it reads Tctl first like every
    # other phase. It writes nothing, so a reading that is not live only warns.
    if (-not (ThermalGate 85.0)) { EndExit 5 }
    CliState 'before'
    $st = RunBounded $law @('selftest', [string]$Cpu, [string]$Ms) 30 'law-selftest'
    Emit 'selftest' $st.Lines
    Say ('selftest exit=' + $st.Code + ' seconds=' + (Num $st.Seconds))
    if ($st.Code -eq 3) { Say 'FATAL the emitter is unavailable on this Windows: every law line of this arm is void'; EndExit 3 }
    if ($st.Code -ne 0) { Say 'selftest FAILED: the lines below are recorded but may not be quoted as measurements' }
    # Pinned, so the three per-CPU APIC-ID fields cannot differ between the legs for the placement's sake.
    Emit 'cpuid' (RunBounded $law @('cpuid', [string]$Cpu) 12 'law-cpuid').Lines
    $full = RunBounded $law @('law', [string]$Cpu, [string]$Ms) 110 'law-full'
    Emit 'law' $full.Lines
    Say ('law exit=' + $full.Code + ' seconds=' + (Num $full.Seconds))
    CliState 'after'
}

'lawmap' {
    # About 25 s measured, 110 s of child bounds, inside a 120 s call. The two sweeps that are long enough
    # to need their own call: the measured sibling map and the cache size sweep.
    if (-not (Test-Path $law)) { Say ('FATAL bd093law.exe missing at ' + $law); EndExit 2 }
    if (-not (ThermalGate 85.0)) { EndExit 5 }
    CliState 'before'
    $pairs = RunBounded $law @('pairs', [string]([int]($Ms / 2))) 50 'law-pairs'
    Emit 'pairs' $pairs.Lines
    Say ('pairs exit=' + $pairs.Code + ' seconds=' + (Num $pairs.Seconds))
    $cache = RunBounded $law @('cache', [string]$Cpu, '25') 55 'law-cache'
    Emit 'cache' $cache.Lines
    Say ('cache exit=' + $cache.Code + ' seconds=' + (Num $cache.Seconds))
    CliState 'after'
}

'smu' {
    # About 60 s, getters only. In state B this phase runs FIRST: a PnP restart does not unload the driver
    # image, so the second start's lines sit in the wrapping tail of the log ring (head 256 lines, ring
    # 1024, guard.c:43-58) and a few hundred further lines would lose the RLC witness.
    if (-not (ThermalGate 85.0)) { EndExit 5 }
    Emit 'info' (Run $cli @('info') 6)
    CliState 'idle'
    Emit 'health' (Run $cli @('health','read') 10)
    $rb = Run $cli @('cpu','readback') 20
    Emit 'cpu readback idle' $rb
    DecodeFeatures $rb
    # 0x43 under load: the readback is the only path that sends it, and the load must be running while it
    # does. A CPU message is refused while the GPU is at or above half busy (BC250_CPU_GPU_BUSY_PERMILLE
    # 500), so the load here is a CPU load and nothing else.
    #
    # The job's output is KEPT and its kernel rates are printed. The 2026-10-12 arm lost its `contend` arm to
    # exactly the opposite: state A recorded noise_ops=0, the thief thread did no work, and the arm never
    # fired while still printing numbers. A readback taken beside a load that did nothing is an idle
    # readback, and this phase says so instead of labelling it "under load".
    $job = Start-Job -ScriptBlock {
        param($p, $cpu)
        & $p 'kernels' $cpu '2000' '1' 2>&1        # 7 kernels x 2 s: the load outlives the readback
    # The argument list is built inside @( ... ): in command-argument position PowerShell reads
    # `-ArgumentList $probe, [string]$Cpu` as the two strings "<probe>" and "[string]2", so the probe was
    # handed "[string]2" as its CPU index, printed its usage text and never ran. The new witness below
    # (job_state, line count and dep_mix rate) is what caught it offline.
    } -ArgumentList @($probe, ([string]$Cpu))
    Start-Sleep -Milliseconds 1500
    $rb2 = Run $cli @('cpu','readback') 20
    Emit 'cpu readback under-load' $rb2
    Wait-Job $job -Timeout 30 | Out-Null
    $jobOut = @(Receive-Job $job -ErrorAction SilentlyContinue | ForEach-Object { [string]$_ })
    $jobState = $job.State
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    $loadRate = 0.0
    foreach ($l in $jobOut) {
        $m = [regex]::Match([string]$l, 'name=dep_mix .*ops_per_us=([0-9.]+)')
        if ($m.Success) { $loadRate = [double]$m.Groups[1].Value }
    }
    Emit 'cpu-load' ($jobOut | Select-Object -First 24)
    Say ('cpu-load job_state=' + $jobState + ' lines=' + $jobOut.Count +
         ' dep_mix_ops_per_us=' + (Num $loadRate))
    if ($loadRate -le 0.0) {
        Say ('cpu readback under-load VOID: the CPU load produced no measurable work, so the readback above' +
             ' is an idle readback and the under-load 0x43 row decides nothing')
    } else {
        DecodeFeatures $rb2
    }
    Emit 'cpu readback after-load' (Run $cli @('cpu','readback') 20)
    $logOut = Join-Path $Work ($State + '-kmdlog.txt')
    $kl = Run $cli @('log') 1400 30
    Set-Content -Path $logOut -Value $kl -Encoding Ascii
    Say ('kmdlog lines=' + @($kl).Count + ' file=' + $logOut)
    Emit 'kmdlog' ($kl | Select-String -Pattern 'cumode|RLC_PG_CNTL|startup: clock|GfxTraceRlcState|dpm start|dpm stop' |
                   ForEach-Object { $_.Line } | Select-Object -Last 30)
}

'pmc' {
    # About 40 s, read only, state A only. Does an instruction and cycle counter exist on this Windows at
    # all? The answer decides whether the Zen 2 dispatch counters (D4) can be reached without a driver
    # escape. A failure here is a real answer, not a gap: it is why the port map of the law phase exists.
    if (-not (ThermalGate 85.0)) { EndExit 5 }
    $sysroot = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
    foreach ($tool in @('xperf.exe','wpr.exe','tracelog.exe','typeperf.exe')) {
        $where = @(Get-Command $tool -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
        Say ('pmctool ' + $tool + ' found=' + (& { if ($where.Count -gt 0) { $where[0] } else { 'no' } }))
    }
    $wpr = Join-Path $sysroot 'System32\wpr.exe'
    if (Test-Path $wpr) {
        Emit 'wpr-pmcsources' (Run $wpr @('-pmcsources') 40)
    } else { Say 'wpr.exe not present: no inbox PMC source list' }
    $xp = @(Get-Command 'xperf.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
    if ($xp.Count -gt 0) { Emit 'xperf-pmcsources' (Run $xp[0] @('-pmcsources') 40) }
    # The counter set the game harness uses, as the positive control of this phase: if this does not read,
    # nothing in this phase can be trusted either.
    try {
        $c = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation -ErrorAction Stop |
             Where-Object { $_.Name -notlike '*_Total' } | Select-Object -First 3
        foreach ($x in $c) {
            Say ('perf name=' + $x.Name + ' processor_performance=' + $x.PercentProcessorPerformance +
                 ' frequency=' + $x.ProcessorFrequency)
        }
    } catch { Say ('perf CIM failed ' + $_.Exception.Message) }
}

'clockcap' {
    # About 150 s. The ground truth: a REAL clock ceiling, so the two-parameter law is read once against a
    # known cause. Expected in state A: the clock witness follows the ceiling and k does not move. That is
    # what makes "clock 0.80x" and "issue rate 0.2x" two separate effects by construction, not by fitting.
    #
    # The write: message 0x8F SET_MAX_MHZ at 2800 MHz through `cli cpu set`, which is a LOWERING of the
    # firmware's own ceiling (bc250_cpu.h:  BC250_CPU_MIN_MHZ 2800, BC250_CPU_MAX_MHZ 3500; a value above
    # 3500 would need a real undervolt and is not used here). The KMD stays the single SMU owner, the
    # driver's own trial window reverts it with no caller (cpu.c: "trial over" -> CpuRevert), and this
    # script releases it explicitly as well. Session 454 already ran this exact ceiling (K219).
    #
    # Preconditions, because a REFUSED set would read exactly like "the clock moved and k did not":
    #   * CpuTune must be 1 in the driver's Parameters key, or the escape answers
    #     STATUS_INVALID_DEVICE_STATE (cpu.c:1282);
    #   * `cpu readback` must have answered in this start, or it answers STATUS_DEVICE_NOT_READY (:1284);
    #   * the KMD must be 0.7.216.24 or later (BD-094's fix), or `cpu reset` restores 3200 MHz instead of the
    #     firmware boost and every later trial of the boot runs degraded with no warm restart left.
    # The phase therefore asserts the success marker of the write, asserts that the capped clock really is
    # near 2800 MHz, and stops the arm (exit 6) if the release does not bring the baseline back.
    if (-not (ThermalGate 82.0 -Require)) { EndExit 5 }
    Emit 'info' (Run $cli @('info') 6)
    CliState 'before-cap'
    $base = LawQuick 'baseline' 40
    $set = Run $cli @('cpu','set','clock','2800','window','75000') 12
    Emit 'cpu set' $set
    # CpuWrite prints "cpu set: done, serial <n>" on success (bc250kmd_cli.c:3456) and "cpu set: refused,
    # status ..." otherwise (:3433). "applied|trial|ok" matched any line containing "ok" and was no gate.
    $applied = $false
    foreach ($l in $set) { if ([regex]::IsMatch([string]$l, '^cpu set: done,')) { $applied = $true } }
    Say ('clockcap set_accepted=' + $applied)
    if (-not $applied) {
        Say ('clockcap VOID the 2800 MHz ceiling was refused, so there is no ground truth in this leg and no' +
             ' row of the law may be read against it. Check CpuTune, the readback and BD-094 first')
        Emit 'cpu after-refusal' (Run $cli @('cpu') 12)
        EndExit 6
    }
    Emit 'cpu after-set' (Run $cli @('cpu') 12)
    Emit 'cpu readback after-set' (Run $cli @('cpu','readback') 20)
    $capped = LawQuick 'capped' 40
    $cappedOk = ($capped.Clock -gt 2660.0 -and $capped.Clock -lt 2940.0)
    Say ('clockcap capped_clock_followed=' + $cappedOk + ' target_mhz=2800 witness_mhz=' + (Num $capped.Clock))
    $rel = Run $cli @('cpu','reset') 10
    Emit 'cpu reset' $rel
    Start-Sleep -Seconds 2
    Emit 'cpu after-reset' (Run $cli @('cpu') 12)
    $back = LawQuick 'released' 40
    Say ('clockcap RESULT base_mhz=' + (Num $base.Clock) + ' capped_mhz=' + (Num $capped.Clock) +
         ' released_mhz=' + (Num $back.Clock) +
         ' base_k=' + (Num $base.K 4) + ' capped_k=' + (Num $capped.K 4) + ' released_k=' + (Num $back.K 4))
    Say 'clockcap READING a ceiling that moves the clock and leaves k alone is the positive control of the law'
    if (-not $cappedOk) {
        Say ('clockcap VOID the write was accepted but the clock witness did not follow the ceiling: this leg' +
             ' is not a ground truth and the arm stops here')
        EndExit 6
    }
    if ($back.Clock -gt 0 -and $base.Clock -gt 0 -and [math]::Abs($back.Clock - $base.Clock) / $base.Clock -gt 0.05) {
        Say ('clockcap FATAL the released clock did not return to the baseline (BD-094 on a KMD before' +
             ' 0.7.216.24 restores 3200 MHz): every later trial of this boot would run degraded. The arm stops' +
             ' here and the way back is a Windows restart')
        EndExit 6
    }
}

'load' {
    # About 150 s. The law under a fixed GPU load. In state A this puts the GFX island at or above the power
    # it draws at idle in state B, so a current-driven core throttle (M1a) would have to show here as well.
    # The SMU is not read while the load runs: a CPU message is refused at or above half GPU busy.
    #
    # THE LOADER MUST OUTLIVE THE MEASUREMENT. vkmembw with no argument is not a load: its defaults are
    # --mib 256 --iters 20 --warmup-ms 2000 and both placements (vkmembw.c:264), so it warms up twice for
    # 2 s, runs 20 timed dispatches of 28 ms each and exits in about 5 s. The first version of this phase
    # slept 4 s and then measured for 6 to 10 s, so the law was taken with the GPU already going idle and
    # W-2, the only M1a separator of the arm, printed numbers while deciding nothing. The loader now gets a
    # warmup longer than the whole phase and one placement, so it copies for the whole window
    # ("--warmup-ms keeps the GPU busy with copies", vkmembw.c:17), and the island power is a hard gate.
    if (-not (ThermalGate 80.0 -Require)) { EndExit 5 }
    $pre = Run $cli @('dpm') 8
    Emit 'dpm before-load' $pre
    Emit 'cpu before-load' (Run $cli @('cpu') 12)
    $idleW = IslandW $pre
    $idle = LawQuick 'gpu-idle' 40
    $loader = $null; $loaderName = 'none'; $loaderOut = ''
    if (Test-Path $gpuload) {
        $loaderOut = Join-Path $Work ($State + '-gpuload.out')
        $loader = Start-Process -FilePath $gpuload -ArgumentList @('--seconds','90') -NoNewWindow -PassThru `
                  -RedirectStandardOutput $loaderOut `
                  -RedirectStandardError (Join-Path $Work ($State + '-gpuload.err'))
        $loaderName = 'gpuload'
    } elseif (Test-Path $membw) {
        $loaderOut = Join-Path $Work ($State + '-loadmembw.out')
        $loader = Start-Process -FilePath $membw `
                  -ArgumentList @('--placement','local','--mib','256','--warmup-ms','90000') `
                  -NoNewWindow -PassThru `
                  -RedirectStandardOutput $loaderOut `
                  -RedirectStandardError (Join-Path $Work ($State + '-loadmembw.err'))
        $loaderName = 'vkmembw-warmup90s'
    }
    Say ('load loader=' + $loaderName + ' idle_gfx_w=' + (Num $idleW))
    if ($null -eq $loader) {
        Say 'load NO GPU LOAD TOOL: this phase decides nothing and is void'
        EndExit 4
    }
    $null = $loader.Handle
    Start-Sleep -Seconds 5
    $d1 = Run $cli @('dpm') 8
    Emit 'dpm under-load-1' $d1
    $w1 = IslandW $d1
    $under = LawQuick 'gpu-loaded' 40
    $d2 = Run $cli @('dpm') 8
    Emit 'dpm under-load-2' $d2
    $w2 = IslandW $d2
    $t = TempC
    Say ('load tctl_c=' + (Num $t) + ' source=' + $script:TempSource)
    # The loader is this script's own child, so ending it here breaks no rule and leaves nothing on the GPU.
    # It is killed at once: it was started to hold the island up during the measurement and the measurement
    # is over, and waiting for a 90 s warmup to end would spend the phase's whole budget on nothing.
    $exitedEarly = $loader.HasExited
    if (-not $exitedEarly) { try { $loader.Kill() } catch { Say 'load kill failed' } }
    $loader.WaitForExit(5000) | Out-Null
    # A loader that ended by itself before the kill is the B1 failure: the measurement was taken on an idle
    # GPU. The island-power gate below is the hard check, and this line names the cause when it fails.
    Say ('load loader_exited_on_its_own=' + (& { if ($exitedEarly) { 'YES, it did not outlive the measurement' }
                                                 else { 'no, it was still running and was killed' } }))
    Emit 'loader' (Get-Content $loaderOut -ErrorAction SilentlyContinue | Select-Object -Last 8)
    Start-Sleep -Seconds 3
    $post = Run $cli @('dpm') 8
    Emit 'dpm after-load' $post
    Emit 'cpu after-load' (Run $cli @('cpu') 12)
    Say ('load RESULT idle_k=' + (Num $idle.K 4) + ' loaded_k=' + (Num $under.K 4) +
         ' idle_mhz=' + (Num $idle.Clock) + ' loaded_mhz=' + (Num $under.Clock) +
         ' idle_gfx_w=' + (Num $idleW) + ' loaded_gfx_w_1=' + (Num $w1) + ' loaded_gfx_w_2=' + (Num $w2) +
         ' after_gfx_w=' + (Num (IslandW $post)))
    # The gate W-2 rests on: state B draws 17.2 W at its 500 MHz idle point, so the loaded island must be
    # clearly above that or neither leg of W-2 can be read. 20.0 W is that bound with a margin; the
    # 2026-10-12 record measured 43.9 W under exactly this loader.
    $gateW = 20.0
    if ($w1 -lt 0 -or $w2 -lt 0) {
        Say ('load GATE VOID the GFX island power could not be read in at least one sample (the CLI prints' +
             ' "power n/a" or "power ?" when the SMU table is not fresh), so W-2 decides nothing')
        EndExit 4
    }
    if ($w1 -lt $gateW -or $w2 -lt $gateW) {
        Say ('load GATE FAILED the island stayed under ' + (Num $gateW) + ' W for at least one sample' +
             ' (state B draws 17.2 W at idle), so the GPU was not loaded through the measurement and W-2' +
             ' decides nothing. Check the loader output above')
        EndExit 4
    }
    Say ('load GATE PASSED the island held above ' + (Num $gateW) + ' W through the measurement, so the k' +
         ' pair above is the M1a reading: k unchanged means real GFX current does not cap the cores')
}

'force' {
    # About 120 s, state B only. F0': the GFX operating point is re-applied and moved through the KMD's own
    # DPM floor, which sends RequestGfxclk and ForceGfxVid (both on the shim allow-list,
    # bc250_clock.c:142-157) from the live owner, and is then released. MECHANISM.md F0 asks for
    # UnforceGfxVid as well; that message is refused by the allow-list by design, needs a driver change and
    # is therefore NOT in this arm. What this arm can still decide: whether a fresh force transaction at a
    # different point, and the island power that follows it, moves the issue rate cap at all.
    if (-not (ThermalGate 78.0 -Require)) { EndExit 5 }
    CliState 'before-force'
    $before = LawQuick 'before-force' 35
    $f1 = Run $cli @('dpm','floor','1500') 10
    Emit 'dpm floor 1500' $f1
    Start-Sleep -Seconds 6
    Emit 'dpm at-floor-1' (Run $cli @('dpm') 8)
    $t = TempC
    # At or above 85 C the floor goes off HERE and the phase ends. The first version printed "releasing it
    # now" and then ran a full-rate CPU load for up to 35 s before the release, which is not "at once".
    if ($t -ge 85.0) {
        Say ('force THERMAL ' + (Num $t) + ' C at the raised floor: releasing it now and ending the phase')
        Emit 'dpm floor off' (Run $cli @('dpm','floor','off') 10)
        Start-Sleep -Seconds 5
        Emit 'dpm after-release' (Run $cli @('dpm') 8)
        Say ('force RESULT VOID released at ' + (Num $t) + ' C before the at-floor measurement; before_k=' +
             (Num $before.K 4) + ' before_mhz=' + (Num $before.Clock))
        EndExit 5
    }
    Start-Sleep -Seconds 6
    Emit 'dpm at-floor-2' (Run $cli @('dpm') 8)
    $atFloor = LawQuick 'at-floor' 35
    $f2 = Run $cli @('dpm','floor','off') 10
    Emit 'dpm floor off' $f2
    Start-Sleep -Seconds 8
    $rel = Run $cli @('dpm') 8
    Emit 'dpm after-release' $rel
    $after = LawQuick 'after-release' 35
    Say ('force RESULT before_k=' + (Num $before.K 4) + ' atfloor_k=' + (Num $atFloor.K 4) +
         ' after_k=' + (Num $after.K 4) + ' before_mhz=' + (Num $before.Clock) +
         ' atfloor_mhz=' + (Num $atFloor.Clock) + ' after_mhz=' + (Num $after.Clock) +
         ' after_gfx_w=' + (Num (IslandW $rel)))
    # The floor lives in the start's own Tune state (dpm.c:1814-1817), so it has no revert timer of its own:
    # a killed phase or a dropped session leaves the GPU forced at 1500 MHz until someone releases it. The
    # release by hand is `bc250kmd_cli dpm floor off`, and a device stop or a Windows restart clears it too.
    foreach ($l in $rel) {
        if ([regex]::IsMatch([string]$l, 'floor\s+1500')) {
            Say ('force WARNING the dpm line still shows a 1500 MHz floor after the release: run' +
                 ' `bc250kmd_cli dpm floor off` by hand before any further step')
        }
    }
    Say 'force READING k back near 0.23 after the excursion is a fix lead; k unchanged leaves F1 to the driver'
}

}

EndExit 0
