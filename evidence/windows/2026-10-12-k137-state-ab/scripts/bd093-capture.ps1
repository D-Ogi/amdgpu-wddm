# BD-093 / K137 capture. One script, one selector, one flushed line per step. Read-only on the hardware:
# it runs instructions, reads SMU getters through bc250kmd_cli, reads allow-listed GC counters through the
# deployed read-only bc250rd, runs vkmembw, and restarts one non-GPU PnP device as the control. No register
# write, no SMU message outside the getter set the CLI already sends, no clock or voltage change, no
# firmware write.
#
#   powershell -ExecutionPolicy Bypass -File bd093-capture.ps1 -Phase state   -State A
#   powershell -ExecutionPolicy Bypass -File bd093-capture.ps1 -Phase cpu     -State A
#   powershell -ExecutionPolicy Bypass -File bd093-capture.ps1 -Phase gpu     -State A
#   powershell -ExecutionPolicy Bypass -File bd093-capture.ps1 -Phase control -State A
#
# Every phase prints its lines to stdout and appends them to C:\BC250\tmp\bd093\<state>-<phase>.txt.
param(
    [ValidateSet('state','cpu','gpu','control')][string]$Phase = 'state',
    [ValidateSet('A','B')][string]$State = 'A',
    [string]$Tag = '',
    [string]$Work = 'C:\BC250\tmp\bd093'
)

$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$rd = 'C:\BC250\bc250rd\bc250rd_cli.exe'
$probe = Join-Path $Work 'bd093probe.exe'
$membw = 'C:\BC250\vkmembw\vkmembw.exe'
# Test-Path does not search PATH, so every tool is named by its full path. A bare 'pnputil.exe' resolved
# against the SSH home directory and the control phase reported MISSING and then measured nothing (review
# blocker 3): a null control read exactly like the plan's strongest conclusion.
$sysroot = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
$pnputil = Join-Path $sysroot 'System32\pnputil.exe'
$reglist = Join-Path $Work 'rlc-clock.txt'
$log = Join-Path $Work ($State + '-' + $Phase + '.txt')

if (-not (Test-Path $Work)) { New-Item -ItemType Directory -Path $Work -Force | Out-Null }

# Say writes straight to the console, not to the pipeline: in PowerShell everything a function writes to
# the output stream becomes part of its return value, and a step that both reports and returns a number
# would hand the caller its own log lines.
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

function Run([string]$exe, [string[]]$exeArgs, [int]$keep = 40) {
    if (-not (Test-Path $exe)) { return @('MISSING ' + $exe) }
    $out = & $exe @exeArgs 2>&1 | ForEach-Object { [string]$_ }
    if ($null -eq $out) { return @() }
    return @($out | Select-Object -First $keep)
}

# A child process with a wall-clock bound of its own. The ssh timeout kills only the client, so without
# this a hung vkmembw or probe keeps the GPU and the core after the runner has given up (review gap b).
# Returns a psobject: Lines, Code (-1 = killed on the bound), Seconds.
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

# ---- the off-CPU time base ---------------------------------------------------------------------------
# GOLDEN_TSC (SMUIO, 100 MHz, free running) read by name through the CLI. amdgpu reads UPPER, LOWER, then
# UPPER again and re-reads LOWER when the upper half moved, because the lower half wraps every 42.9 s
# (gfx_v10_0.c:7694-7706). Without that re-read one sample in about every 140 lands 2^32 away (review
# major 6). The QPC instant is the midpoint of the LOWER read, so the golden window and the QPC window are
# bracketed the same way at both ends and a slower CLI in state B cannot bias the ratio.
function RegValue([string]$name) {
    foreach ($l in (Run $cli @('read', $name) 6)) {
        $m = [regex]::Match([string]$l, '^read\s+0x[0-9a-fA-F]+\s+([0-9A-Fa-f]{8})')
        if ($m.Success) { return [uint64]('0x' + $m.Groups[1].Value) }
    }
    return $null
}

function GoldenTsc() {
    $hv = RegValue 'GOLDEN_TSC_COUNT_UPPER_Cyan_Skillfish'
    $ta = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $lv = RegValue 'GOLDEN_TSC_COUNT_LOWER_Cyan_Skillfish'
    $tb = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $hc = RegValue 'GOLDEN_TSC_COUNT_UPPER_Cyan_Skillfish'
    if ($null -eq $hv -or $null -eq $lv -or $null -eq $hc) { return $null }
    if ($hc -ne $hv) {
        $hv = $hc
        $ta = [System.Diagnostics.Stopwatch]::GetTimestamp()
        $lv = RegValue 'GOLDEN_TSC_COUNT_LOWER_Cyan_Skillfish'
        $tb = [System.Diagnostics.Stopwatch]::GetTimestamp()
        if ($null -eq $lv) { return $null }
    }
    $s = New-Object psobject
    $s | Add-Member NoteProperty Value (($hv * [uint64]4294967296) + $lv)
    $s | Add-Member NoteProperty Qpc (($ta + $tb) / 2.0)
    $s | Add-Member NoteProperty SpanS (($tb - $ta) / [double][System.Diagnostics.Stopwatch]::Frequency)
    return $s
}

# ---- the delivered GFX clock -------------------------------------------------------------------------
# RLC_GPU_CLOCK_COUNT_LSB/MSB is NOT a free-running counter: it is a latch that only updates when
# RLC_CAPTURE_GPU_CLOCK_COUNT is written 1 (gfx_v9_0.c:4297-4299), and gfx10 does not use it at all. Two
# read-only samples of it return the same stale value, which would have made the whole C-F capture read
# zero in both states (review blocker 1). This writes nothing, so the free-running counter is the one
# gfx10 and our own sweeps see moving: RLC_GPU_CLOCK_32 (0x3b108, facts M812 names it among the GC timers
# that vary), with RLC_GPU_CLOCK_32_RES_SEL (0x3b104) read beside it because that 6-bit field divides the
# counter and an unknown divisor must be on the record, not in a conclusion. The A/B that answers Q5 is a
# RATIO of delivered to commanded clock, in which the divisor cancels.
# The latch pair stays in the list as a witness: if both samples of it are equal, the latch reading is
# confirmed on this silicon at no cost.
function WriteReglist() {
    $rows = @(
        'GC.RLC_GPU_CLOCK_32_RES_SEL 0x3b104',
        'GC.RLC_REFCLOCK_TIMESTAMP_LSB 0x3b030',
        'GC.RLC_REFCLOCK_TIMESTAMP_MSB 0x3b034',
        'GC.RLC_GPU_CLOCK_32 0x3b108',
        'GC.RLC_REFCLOCK_TIMESTAMP_LSB_RECHECK 0x3b030',
        'GC.RLC_GPU_CLOCK_COUNT_LSB_LATCH_WITNESS 0x3b090',
        'GC.RLC_GPU_CLOCK_COUNT_MSB_LATCH_WITNESS 0x3b094')
    Set-Content -Path $reglist -Value $rows -Encoding Ascii
}

# One bc250rd sweep of the list, with the QPC midpoint of the sweep. Every row keeps its own name, so the
# repeated REFCLOCK row is a real tear check and not a duplicate key that the first value shadows
# (review minor 11).
function RlcSample() {
    $t0 = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $out = Run $rd @('sweep', $reglist, 'GC.') 24
    $t1 = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $v = @{}
    foreach ($l in $out) {
        $m = [regex]::Match([string]$l, '^(GC\.\S+)\s+0x[0-9a-f]+\s+([0-9A-F]{8})')
        if ($m.Success -and -not $v.ContainsKey($m.Groups[1].Value)) { $v[$m.Groups[1].Value] = [uint64]('0x' + $m.Groups[2].Value) }
    }
    $s = New-Object psobject
    $s | Add-Member NoteProperty Raw ($out -join ' | ')
    $s | Add-Member NoteProperty Qpc (($t0 + $t1) / 2.0)
    $s | Add-Member NoteProperty Gpu32 $null
    $s | Add-Member NoteProperty Ref $null
    $s | Add-Member NoteProperty ResSel $null
    $s | Add-Member NoteProperty Latch $null
    $s | Add-Member NoteProperty RefTorn 0
    if ($v.ContainsKey('GC.RLC_GPU_CLOCK_32')) { $s.Gpu32 = $v['GC.RLC_GPU_CLOCK_32'] }
    if ($v.ContainsKey('GC.RLC_GPU_CLOCK_32_RES_SEL')) { $s.ResSel = ($v['GC.RLC_GPU_CLOCK_32_RES_SEL'] -band 0x3F) }
    if ($v.ContainsKey('GC.RLC_REFCLOCK_TIMESTAMP_LSB') -and $v.ContainsKey('GC.RLC_REFCLOCK_TIMESTAMP_MSB')) {
        $lsb = $v['GC.RLC_REFCLOCK_TIMESTAMP_LSB']
        $msb = $v['GC.RLC_REFCLOCK_TIMESTAMP_MSB']
        if ($v.ContainsKey('GC.RLC_REFCLOCK_TIMESTAMP_LSB_RECHECK') -and
            $v['GC.RLC_REFCLOCK_TIMESTAMP_LSB_RECHECK'] -lt $lsb) { $s.RefTorn = 1 }
        $s.Ref = ($msb * [uint64]4294967296) + $lsb
    }
    if ($v.ContainsKey('GC.RLC_GPU_CLOCK_COUNT_LSB_LATCH_WITNESS')) { $s.Latch = $v['GC.RLC_GPU_CLOCK_COUNT_LSB_LATCH_WITNESS'] }
    return $s
}

# The 32-bit counter wraps, so the delta is taken modulo 2^32 and the window must stay short enough for
# that to be unambiguous: at 2000 MHz and RES_SEL 0 one wrap is 2.1 s, so a window above about 1.5 s is
# reported as AMBIGUOUS instead of being turned into a number.
function RlcDelivered([string]$what, $a, $b) {
    if ($null -eq $a -or $null -eq $b -or $null -eq $a.Gpu32 -or $null -eq $b.Gpu32) {
        Say ('rlc ' + $what + ' UNREADABLE (bc250rd gave no GC.RLC_GPU_CLOCK_32 value)'); return
    }
    $d32 = [double](($b.Gpu32 - $a.Gpu32 + [uint64]4294967296) % [uint64]4294967296)
    $qpcS = ($b.Qpc - $a.Qpc) / [double][System.Diagnostics.Stopwatch]::Frequency
    $byQpc = if ($qpcS -gt 0) { $d32 / $qpcS / 1000000.0 } else { 0 }
    $byRef = 0.0
    $dRef = 'na'
    if ($null -ne $a.Ref -and $null -ne $b.Ref -and $b.Ref -gt $a.Ref) {
        $dRef = $b.Ref - $a.Ref
        $byRef = $d32 / [double]$dRef * 100.0
    }
    $flag = if ($qpcS -gt 1.5) { ' WRAP-AMBIGUOUS (window over 1.5 s)' } else { '' }
    $tear = if ($a.RefTorn -or $b.RefTorn) { ' REFCLOCK-TORN' } else { '' }
    Say ('rlc ' + $what + ' d_clock32=' + $d32 + ' qpc_s=' + (Num $qpcS 3) +
         ' mhz_by_qpc=' + (Num $byQpc) + ' mhz_by_refclk=' + (Num $byRef) +
         ' d_refclk=' + $dRef + ' res_sel=' + $(if ($null -ne $b.ResSel) { $b.ResSel } else { 'na' }) +
         ' latch_a=' + $(if ($null -ne $a.Latch) { $a.Latch } else { 'na' }) +
         ' latch_b=' + $(if ($null -ne $b.Latch) { $b.Latch } else { 'na' }) +
         ' latch_moved=' + $(if ($null -ne $a.Latch -and $null -ne $b.Latch -and $a.Latch -ne $b.Latch) { 1 } else { 0 }) +
         $flag + $tear)
}

# ---- the CPU-side instruments ------------------------------------------------------------------------
# The APERF / MPERF counter of one logical CPU. Read through CIM, whose property names are the same in
# every Windows language; `Get-Counter` needs the localised counter names and fails outright on a
# non-English Windows, which is how the counter leg of the K223 bundle came back as 0 per cent.
# Win32_PerfFormattedData is cooked over the provider's own refresh interval, not over our load window, so
# every sample carries its own instant and the caller spaces them (review minor 13).
function PerfSample([string]$what, [string]$instance = '0,2') {
    $at = (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff')
    try {
        $d = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation `
             -Filter ("Name='" + $instance + "'") -ErrorAction Stop | Select-Object -First 1
        if ($null -eq $d) { Say ('counter ' + $what + ' instance ' + $instance + ' NOT PRESENT'); return }
        Say ('counter ' + $what + ' at=' + $at + ' window=provider-refresh-not-our-load instance=' + $instance +
             ' pct_processor_performance=' + $d.PercentProcessorPerformance +
             ' pct_processor_time=' + $d.PercentProcessorTime +
             ' processor_frequency_mhz=' + $d.ProcessorFrequency +
             ' pct_of_maximum_frequency=' + $d.PercentofMaximumFrequency)
        return
    } catch { Say ('counter ' + $what + ' CIM FAILED ' + $_.Exception.Message) }
    try {
        $c = Get-Counter -Counter '\Processor Information(0,2)\% Processor Performance' `
             -SampleInterval 1 -MaxSamples 1 -ErrorAction Stop
        foreach ($s in $c.CounterSamples) { Say ('counter ' + $what + ' fallback ' + $s.Path + ' = ' + (Num $s.CookedValue)) }
    } catch { Say ('counter ' + $what + ' FALLBACK FAILED ' + $_.Exception.Message) }
}

# `cli cpu` with no verb is BC250_CPU_OP_READ, and that path "sends nothing": it returns the cached
# snapshot (cpu.c:1229, the comment at :1267-1270). CoreMHz[] in it is filled by CpuReadStage, which runs
# once per start from the worker (cpu.c:799) and on demand only for READBACK and SEARCH_STEP
# (cpu.c:1286-1290, :1435). So a bare `cli cpu` inside a load window prints the START-TIME IDLE clocks in
# both legs, and the plan's own row "0x43 says 3500 while the meter says 1550" would have been guaranteed
# by construction (review blocker 2). `cpu readback` is what sends message 0x43 (cpu.c:390).
# It needs an elevated shell (SSH here is elevated) and CpuTune 1 (the INF writes it from 0.7.216), and it
# holds CpuLock and the SMU owner lock for about 19 getters at a 10 ms gap, roughly 0.2-0.5 s. That is a
# deliberate cost of this capture and it is the only way 0x43 can be read under load at all.
function CpuReadback([string]$what) {
    Emit ('cpu readback ' + $what) (Run $cli @('cpu','readback') 16)
}

function CliState([string]$what) {
    Emit ('dpm ' + $what) (Run $cli @('dpm') 6)
    Emit ('cpu ' + $what) (Run $cli @('cpu') 12)
}

function QuickMeter([int]$cpu, [int]$ms) {
    $lines = Run $probe @('kernels', [string]$cpu, [string]$ms, '1') 24
    Emit 'meter' $lines
    $rate = 0.0
    foreach ($l in $lines) {
        $m = [regex]::Match([string]$l, 'name=dep_mix .*ops_per_us=([0-9.]+)')
        if ($m.Success) { $rate = [double]$m.Groups[1].Value }
    }
    return $rate
}

Say ('BEGIN tag=' + $Tag + ' host=' + $env:COMPUTERNAME + ' phase=' + $Phase + ' state=' + $State)
Say ('boot lastboot=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s') +
     ' uptime_s=' + [int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds)
foreach ($p in @(Get-CimInstance Win32_Processor)) {
    Say ('topology name=' + $p.Name.Trim() + ' cores=' + $p.NumberOfCores +
         ' logical=' + $p.NumberOfLogicalProcessors + ' max_clock_mhz=' + $p.MaxClockSpeed +
         ' current_clock_mhz=' + $p.CurrentClockSpeed)
}
$gpuDev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
if ($null -ne $gpuDev) { Say ('gpu device status=' + $gpuDev.Status + ' instance=' + $gpuDev.InstanceId) } else { Say 'gpu device NOT FOUND' }
if (-not (Test-Path $probe)) { Say ('FATAL probe missing at ' + $probe); exit 2 }

switch ($Phase) {

'state' {
    # ~40 s. Says which state the machine is in, so a warm restart is spent only when it is needed.
    Emit 'info' (Run $cli @('info') 6)              # `version` is not a command of this CLI (minor 9)
    CliState 'idle'
    Emit 'timebase' (Run $probe @('timebase','1000') 4)
    PerfSample 'idle'
    $rate = QuickMeter 2 300
    PerfSample 'just-after-load'
    $verdict = if ($rate -ge 420) { 'healthy-like' } elseif ($rate -gt 0) { 'k137-like' } else { 'unreadable' }
    Say ('STATE-GUESS dep_mix_ops_per_us=' + (Num $rate) + ' verdict=' + $verdict +
         ' (reference: fresh 575, K137 258, K223)')
}

'cpu' {
    # ~110 s. The CPU half: the kernels, the stolen-time histogram with its own positive control, the
    # per-CPU sweep, thread scaling, memory latency, and the SMU effective clock read inside the load.

    # C-G, over a 10 s window. The threshold the plan reads is 0.5 %, so the window has to be long enough
    # for a CLI launch at either end not to spend it: two launches of 0.2 s over 3 s were already 6 %
    # (review major 6). Both golden reads are bracketed by their own QPC pair and the midpoints are what
    # the ratio uses.
    $g0 = GoldenTsc
    Emit 'timebase' (Run $probe @('timebase','10000') 4)
    $g1 = GoldenTsc
    if ($null -ne $g0 -and $null -ne $g1 -and $g1.Value -gt $g0.Value) {
        $qs = ($g1.Qpc - $g0.Qpc) / [double][System.Diagnostics.Stopwatch]::Frequency
        Say ('timebase golden_tsc_delta=' + ($g1.Value - $g0.Value) + ' qpc_s=' + (Num $qs 3) +
             ' golden_mhz=' + (Num (($g1.Value - $g0.Value) / $qs / 1000000.0) 3) +
             ' read_span_a_s=' + (Num $g0.SpanS 3) + ' read_span_b_s=' + (Num $g1.SpanS 3) +
             ' (expect 100.000; the read spans bound the error)')
    } else { Say 'timebase golden_tsc UNREADABLE' }

    # The kernel suite runs in the background so that the SMU effective clock (message 0x43 through
    # `cli cpu readback`) and the APERF/MPERF counter are sampled while the core is loaded. That has never
    # been done in either state.
    $probeOut = Join-Path $Work ($State + '-kernels.txt')
    $p = Start-Process -FilePath $probe -ArgumentList @('kernels','2','1500','1') -NoNewWindow -PassThru `
         -RedirectStandardOutput $probeOut -RedirectStandardError (Join-Path $Work ($State + '-kernels.err'))
    $null = $p.Handle
    Start-Sleep -Milliseconds 1500
    for ($i = 0; $i -lt 3; $i++) {
        CpuReadback ('under-load ' + $i)
        PerfSample ('under-load ' + $i)
        Start-Sleep -Milliseconds 700
    }
    if (-not $p.WaitForExit(60000)) {
        Say 'kernels BOUND 60 s reached: killing it, the kernel table of this leg is void'
        try { $p.Kill() } catch { Say ('kernels kill failed ' + $_.Exception.Message) }
        $p.WaitForExit(5000) | Out-Null
    }
    Say ('kernels exit=' + $p.ExitCode)
    Emit 'kernel' (Get-Content $probeOut -ErrorAction SilentlyContinue)

    # C-B. The coarse arm is the one with the validated 50 % control; the fine arm is as fine as a
    # software timer can go here (QPC ticks at 0.1 us on this part, so a 500-operation chunk of about
    # 0.9 us is already only 9 ticks). Nothing in software resolves per-cycle stretching, which is why
    # the reading table no longer offers "lower clock" as the only conclusion of a falling median.
    Emit 'hist' (Run $probe @('hist','2','2000','2500') 4)
    # 1000 ms, not 2500: at 500 operations a chunk the recorded-sample cap (2 M) is reached inside 2.5 s
    # in the healthy state, and then the median would describe only the first part of the window.
    Emit 'hist' (Run $probe @('hist','2','500','1000') 4)
    Emit 'hist' (Run $probe @('hist','2','2000','2500','steal') 4)
    Emit 'hist' (Run $probe @('hist','2','2000','1500','contend') 4)
    Emit 'sweep' (Run $probe @('sweep','250') 24)
    Emit 'threads' (Run $probe @('threads','400','1,2,4,6,8') 12)
    Emit 'latency' (Run $probe @('latency','2','250') 8)
    PerfSample 'after-load'
    CliState 'after-load'
    Emit 'health' (Run $cli @('health','read') 8)   # `health` alone returns 2 and prints nothing (minor 9)
}

'gpu' {
    # ~150 s. The GPU half: the delivered GFX clock from the free-running RLC counter (never read in
    # either state), the gating witness the start log prints for free, bandwidth as the control, power.
    WriteReglist
    Emit 'rd info' (Run $rd @('info') 10)
    CliState 'idle'
    # Idle: three short pairs rather than one 3 s pair, because the counter is 32 bits wide and wraps.
    for ($i = 0; $i -lt 3; $i++) {
        $a = RlcSample
        if ($i -eq 0) { Say ('rlc raw-idle ' + $a.Raw) }
        Start-Sleep -Milliseconds 900
        $b = RlcSample
        RlcDelivered ('idle-' + $i) $a $b
    }

    # Under a GPU load. vkmembw is bandwidth-bound and its own baseline is known (copy 386-389 GB/s,
    # M776), so it is both the memory-path control and the load for the delivered-clock reading. The
    # counter is sampled about once a second DURING the run and only the samples inside the steady part
    # are turned into a clock: one pair spanning the whole process mixes Vulkan init and teardown at idle
    # clocks into the average, and state B is slower, so each leg would mix a different proportion of
    # idle into the same number (review major 7).
    $mbOut = Join-Path $Work ($State + '-vkmembw.txt')
    $mbErr = Join-Path $Work ($State + '-vkmembw.err')
    if (-not (Test-Path $membw)) { Say ('vkmembw MISSING ' + $membw) }
    else {
        $mp = Start-Process -FilePath $membw -NoNewWindow -PassThru `
              -RedirectStandardOutput $mbOut -RedirectStandardError $mbErr
        $null = $mp.Handle
        $samples = @()
        $deadline = (Get-Date).AddSeconds(90)
        while (-not $mp.HasExited -and (Get-Date) -lt $deadline) {
            $samples += , (RlcSample)
            Start-Sleep -Milliseconds 900
        }
        if (-not $mp.HasExited) {
            Say 'vkmembw BOUND 90 s reached: killing it, the loaded clock of this leg is void'
            try { $mp.Kill() } catch { Say ('vkmembw kill failed ' + $_.Exception.Message) }
            $mp.WaitForExit(5000) | Out-Null
        }
        Say ('vkmembw exit=' + $mp.ExitCode + ' rlc_samples=' + $samples.Count)
        for ($i = 1; $i -lt $samples.Count; $i++) { RlcDelivered ('under-vkmembw pair-' + $i) $samples[$i-1] $samples[$i] }
        $mb = @(Get-Content $mbOut -ErrorAction SilentlyContinue) + @(Get-Content $mbErr -ErrorAction SilentlyContinue)
        Emit 'vkmembw' ($mb | Where-Object { $_ -match 'GB/s|placement|check' } | Select-Object -First 12)
    }
    CliState 'after-vkmembw'

    # The start log holds the cumode RLC_PG_CNTL witness and the startup clock line, printed at every
    # start and captured in no evidence file so far. The ring keeps the first 256 lines of a DRIVER LOAD
    # for as long as it stays loaded plus the last 768 (guard.c:43-58, BC250_LOG_HEAD_LINES 256,
    # BC250_LOG_RING_LINES 1024), so in state A the first start is safe in the head. A PnP restart does
    # not unload the image, so the SECOND start's lines go into the wrapping tail: in state B this phase
    # has to run before anything else fills 768 lines.
    $logOut = Join-Path $Work ($State + '-kmdlog.txt')
    $kl = Run $cli @('log') 1200
    Set-Content -Path $logOut -Value $kl -Encoding Ascii
    Say ('kmdlog lines=' + @($kl).Count + ' file=' + $logOut)
    Emit 'kmdlog' ($kl | Select-String -Pattern 'cumode|RLC_PG_CNTL|startup: clock|GfxTraceRlcState|RLC startup policy' |
                   ForEach-Object { $_.Line } | Select-Object -Last 24)
}

'control' {
    # ~70 s. The control the record does not have: a PnP stop and start of a device that is not the GPU.
    # If the CPU meter is untouched by it, the OS PnP and power-management path is excluded without the
    # confounded EnableNativeSmu 0 arm (C13: with no SMU owner the full-WDDM start never brings PSP, RLC
    # and GART up, so that arm changed five things at once and can decide nothing).
    # The discriminating leg is state A: in state B the meter is already low and a null result there says
    # nothing about causation (review major 5).
    $before = QuickMeter 2 400
    Say ('control meter-before dep_mix_ops_per_us=' + (Num $before))
    $sshIf = -1
    try {
        $conn = Get-NetTCPConnection -LocalPort 22 -State Established -ErrorAction Stop | Select-Object -First 1
        if ($null -ne $conn) {
            $ip = Get-NetIPAddress -IPAddress $conn.LocalAddress -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $ip) { $sshIf = $ip.InterfaceIndex }
        }
    } catch { Say ('control ssh-interface lookup failed ' + $_.Exception.Message) }
    Say ('control ssh_interface_index=' + $sshIf)
    # Without the interface that carries this session there is no way to know which adapter must not be
    # touched, and the first candidate can be exactly that one. Refuse instead of guessing (review gap a).
    if ($sshIf -lt 0) {
        Say 'control ABORT: this session interface is unknown, so no adapter can be ruled safe. The lead picks the control device by hand.'
        Say ('END phase=' + $Phase + ' state=' + $State + ' log=' + $log)
        exit 4
    }
    $cand = Get-PnpDevice -PresentOnly -Class Net | Where-Object { $_.Status -eq 'OK' -and $_.InstanceId -notlike 'PCI\VEN_1002*' }
    $pick = $null
    foreach ($dev in $cand) {
        $ad = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.PnPDeviceID -eq $dev.InstanceId } | Select-Object -First 1
        if ($null -eq $ad) { continue }
        if ($ad.InterfaceIndex -eq $sshIf) { Say ('control skip (carries this session) ' + $dev.FriendlyName); continue }
        $pick = $dev
        Say ('control picked ' + $dev.FriendlyName + ' instance=' + $dev.InstanceId + ' ifindex=' + $ad.InterfaceIndex)
        break
    }
    if ($null -eq $pick) {
        Say 'control NO SAFE DEVICE: skipped, the lead decides the control by hand'
        Say ('END phase=' + $Phase + ' state=' + $State + ' log=' + $log)
        exit 4
    }
    # pnputil's text is localised, so the gate is its exit code and the device status, not a string.
    $r = RunBounded $pnputil @('/restart-device', $pick.InstanceId) 60 'pnputil'
    Emit 'control pnputil' $r.Lines
    Say ('control pnputil exit=' + $r.Code + ' seconds=' + (Num $r.Seconds 1))
    Start-Sleep -Seconds 10
    $st = (Get-PnpDevice -InstanceId $pick.InstanceId).Status
    Say ('control device status=' + $st)
    if ($r.Code -ne 0 -or $st -ne 'OK') {
        Say 'control ABORT: the restart did not happen or the device did not come back. This phase decides nothing; do not read it as a null result.'
        Say ('END phase=' + $Phase + ' state=' + $State + ' log=' + $log)
        exit 5
    }
    $after = QuickMeter 2 400
    Say ('control meter-after dep_mix_ops_per_us=' + (Num $after) +
         ' ratio=' + (Num $(if ($before -gt 0) { $after / $before } else { 0 }) 3))
}

}

Say ('END phase=' + $Phase + ' state=' + $State + ' log=' + $log)
