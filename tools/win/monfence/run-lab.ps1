# run-lab.ps1 - one bounded monfence run on the lab, with the KMD's own instruments read before and after.
#
# Runs ON THE TARGET (Windows PowerShell 5.1, elevated SSH session, session 0), for example
#   python tools\win\target.py ps tools\win\monfence\run-lab.ps1
# after monfence.exe has been pushed to C:\BC250\tmp\monfence\. Arguments after the script name reach the client
# through -ClientArgs, e.g. -ClientArgs '--int-sel','3'.
#
# What it does, every step bounded:
#   1. bc250kmd_cli `ih state` and `log summary`     (read-only escapes; 15 s each)
#   2. monfence.exe                                  (killed after -KillSeconds, default 90; its own watchdog is 55 s)
#   3. bc250kmd_cli `ih state` and `log summary <next>` (15 s each)
#   4. analysis: VM-fault vectors that appeared during the run (IH client 27 UTCL2 = gfxhub, 18 VMC = mmhub, source 0;
#      decoded as driver/amdgpu-import/reference/gmc_v10_0.c:106-113 does), matched against the fence page and the
#      client's own buffers; the vidmm PTE-encoding counters (coherent / noncoherent / snoop mismatches) before and
#      after; the exit code's meaning.
# Worst case 15+15+90+15+15 s = 2.5 minutes, inside the three-minute lab bound.
#
# It writes only below C:\BC250\tmp (refuses anything else) and starts nothing that outlives it. It does not touch the
# overlay, the clocks, the KMD state or the registry. A VM fault in this test is a clean failure by design: the KMD
# runs the gfxhub with noretry and fault redirection to the default page (driver/kmd/gart.c:147,
# driver/shim/bc250_gmc.c:304-308), so a faulting write goes nowhere and the engine is not halted.

param(
    [string]$Exe = 'C:\BC250\tmp\monfence\monfence.exe',
    [string]$Cli = 'C:\BC250\m8\bc250kmd_cli.exe',
    [string]$OutRoot = 'C:\BC250\tmp\monfence',
    [ValidateRange(10, 120)][int]$KillSeconds = 90,
    [string[]]$ClientArgs = @()
)

$ErrorActionPreference = 'Stop'

# The IH client ids of a VM protection fault (third_party/linux-amdgpu/soc15_ih_clientid.h: SOC15_IH_CLIENTID_UTCL2,
# SOC15_IH_CLIENTID_VMC). build.ps1 checks these two lines against that header.
$UtcL2Client = 27
$VmcClient = 18

$ExitMeaning = @{
    0  = 'PASS: GPU-written monitored fence works: value, CPU wake, GPU wait release, four threads'
    2  = 'bad command line'
    3  = 'setup failed (adapter, device, paging queue, allocation, context or fence creation)'
    4  = 'client watchdog fired (a D3DKMT call or a wait did not return)'
    5  = 'no usable FenceValueGPUVirtualAddress returned'
    10 = 'packet control: RELEASE_MEM into our own allocation did not land (packet or submit path, not the fence)'
    11 = 'read control: the GPU did not read the CPU-signalled value at the fence GPU VA (no write was attempted)'
    12 = '(a) CPU mapping did not read N after the GPU write (see diagnosis= on the S.a line)'
    13 = '(b) WaitForSynchronizationObjectFromCpu(N) did not wake in time, or woke early'
    14 = '(c) work behind WaitForSynchronizationObjectFromGpu ran before the fence reached N'
    15 = '(c) work behind the GPU wait was not released in time'
    16 = 'torn value, or another fence on the page changed'
    17 = 'latency series iteration failed'
    21 = '(d) packet or read control failed in a thread'
    22 = '(d) (a) failed in a thread'
    23 = '(d) (b) failed in a thread'
    24 = '(d) (c) ordering failed (own or cross-thread wait)'
    25 = '(d) (c) release failed (own or cross-thread wait)'
    26 = '(d) thread barrier timeout or thread did not finish'
    27 = '(d) torn value or a foreign fence changed'
    30 = 'queued GPU waits could not be released by a CPU signal'
}

function Assert-UnderTmp([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith('C:\BC250\tmp\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to write outside C:\BC250\tmp: $full"
    }
    return $full
}

function Invoke-Bounded([string]$File, [string[]]$Arguments, [string]$StdOut, [int]$Seconds) {
    $sp = @{ FilePath = $File; RedirectStandardOutput = $StdOut; RedirectStandardError = "$StdOut.err"; PassThru = $true; NoNewWindow = $true }
    if ($Arguments -and $Arguments.Count -gt 0) { $sp.ArgumentList = $Arguments }
    $started = Get-Date
    $p = Start-Process @sp
    $null = $p.Handle                       # keeps ExitCode readable after exit (Windows PowerShell quirk)
    $exited = $p.WaitForExit($Seconds * 1000)
    $result = @{ Exited = $exited; Code = $null; Killed = $false; Seconds = 0.0 }
    if (-not $exited) {
        try { $p.Kill() } catch { }
        $result.Killed = $true
        $result.Exited = $p.WaitForExit(10000)
    }
    if ($result.Exited -and -not $result.Killed) { $result.Code = $p.ExitCode }
    $result.Seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    return $result
}

function Read-Lines([string]$Path) {
    if (Test-Path $Path) { return @(Get-Content -LiteralPath $Path) }
    return @()
}

function Get-Kinds([string[]]$Lines) {
    $k = @{}
    foreach ($l in $Lines) {
        if ($l -match '^K client (\d+) source (\d+) count (\d+)') { $k["$($Matches[1]):$($Matches[2])"] = [int64]$Matches[3] }
    }
    return $k
}

function Get-Coherence([string[]]$Lines) {
    $c = @{}
    foreach ($l in $Lines) {
        if ($l -match 'vidmm summary: PTE encoding level (\d+) segment (\d+) coherent (-?\d+) noncoherent (-?\d+) snoop mismatches (-?\d+)') {
            $c["level $($Matches[1]) segment $($Matches[2])"] = @([int64]$Matches[3], [int64]$Matches[4], [int64]$Matches[5])
        }
    }
    return $c
}

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$dir = Assert-UnderTmp (Join-Path $OutRoot "run-$stamp")
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$summary = New-Object System.Collections.Generic.List[string]
function Say([string]$Text) { $summary.Add($Text); Write-Output $Text }

Say "MONFENCE-RUN start utc=$stamp dir=$dir kill_s=$KillSeconds args=$($ClientArgs -join ' ')"
if (-not (Test-Path $Exe)) { Say "MONFENCE-RUN error: $Exe not found"; exit 3 }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Exe).Hash
Say "client $Exe sha256=$hash"
$haveCli = Test-Path $Cli
if (-not $haveCli) { Say "instrument: $Cli not found; IH and vidmm checks skipped" }

# 1. before
$logFrom = 0
if ($haveCli) {
    $r = Invoke-Bounded $Cli @('ih', 'state') (Join-Path $dir 'ih-before.txt') 15
    Say "ih state before: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
    $r = Invoke-Bounded $Cli @('log', 'summary') (Join-Path $dir 'log-before.txt') 15
    Say "log summary before: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
    foreach ($l in (Read-Lines (Join-Path $dir 'log-before.txt'))) {
        if ($l -match '^\s*(\d+)\s+\d+\.\d{3} ') { $seq = [int64]$Matches[1]; if ($seq -ge $logFrom) { $logFrom = $seq + 1 } }
    }
}

# 2. the client
$clientOut = Join-Path $dir 'monfence.out'
$run = Invoke-Bounded $Exe $ClientArgs $clientOut $KillSeconds
Say "client: exit=$($run.Code) killed=$($run.Killed) exited=$($run.Exited) s=$($run.Seconds)"

# 3. after
if ($haveCli) {
    $r = Invoke-Bounded $Cli @('ih', 'state') (Join-Path $dir 'ih-after.txt') 15
    Say "ih state after: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
    $r = Invoke-Bounded $Cli @('log', 'summary', "$logFrom") (Join-Path $dir 'log-after.txt') 15
    Say "log summary after (from $logFrom): exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
}

# 4. analysis
$out = Read-Lines $clientOut
foreach ($l in $out) { if ($l -match '^(CHECK|LAT|SUMMARY|RESULT|RESCUE|FENCE|NOTE|PHASE) ') { Write-Output "  | $l" } }
$checks = @($out | Where-Object { $_ -match '^CHECK ' })
$failed = @($checks | Where-Object { $_ -match '^CHECK \S+ FAIL' })
Say "checks: $($checks.Count) printed, $($failed.Count) failed"
foreach ($l in $failed) { Say "  failed: $l" }

# The client's GPU address ranges, for matching fault addresses.
$ranges = New-Object System.Collections.Generic.List[object]
foreach ($l in $out) {
    if ($l -match '^FENCE (w\d) .*gpu=0x([0-9A-Fa-f]+) page=0x([0-9A-Fa-f]+)') {
        $page = [Convert]::ToUInt64($Matches[3], 16)
        $ranges.Add(@{ Name = "fence page ($($Matches[1]) fence at 0x$($Matches[2]))"; Lo = $page; Hi = $page + 0xFFF })
    }
    if ($l -match '^BUFFER (w\d) ib=0x([0-9A-Fa-f]+) rb=0x([0-9A-Fa-f]+)') {
        $ib = [Convert]::ToUInt64($Matches[2], 16); $rb = [Convert]::ToUInt64($Matches[3], 16)
        $ranges.Add(@{ Name = "$($Matches[1]) IB buffer"; Lo = $ib; Hi = $ib + 0xFFFF })
        $ranges.Add(@{ Name = "$($Matches[1]) readback page"; Lo = $rb; Hi = $rb + 0xFFF })
    }
}

$faults = 0
if ($haveCli) {
    $ihBefore = Read-Lines (Join-Path $dir 'ih-before.txt')
    $ihAfter = Read-Lines (Join-Path $dir 'ih-after.txt')
    $ring = @($ihAfter | Where-Object { $_ -match '^ring ' })
    if ($ring.Count -gt 0) { Say "ih $($ring[0].Trim())" }
    $kb = Get-Kinds $ihBefore
    $ka = Get-Kinds $ihAfter
    foreach ($key in ($ka.Keys | Sort-Object)) {
        $before = 0; if ($kb.ContainsKey($key)) { $before = $kb[$key] }
        $delta = $ka[$key] - $before
        if ($delta -ne 0) {
            $name = ''
            if ($key -eq "${UtcL2Client}:0") { $name = ' (UTCL2 VM fault, gfxhub)'; $faults += $delta }
            elseif ($key -eq "${VmcClient}:0") { $name = ' (VMC VM fault, mmhub)'; $faults += $delta }
            Say "ih kind client:source $key +$delta$name"
        }
    }
    $seen = @{}
    foreach ($l in $ihBefore) { if ($l -match '^V ') { $seen[$l] = $true } }
    foreach ($l in $ihAfter) {
        if ($l -notmatch '^V client (\d+) source (\d+) ring (\d+) vmid (\d+) pasid (\d+) data ([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8})') { continue }
        if ($seen.ContainsKey($l)) { continue }
        $client = [int]$Matches[1]; $source = [int]$Matches[2]
        if ($source -ne 0 -or ($client -ne $UtcL2Client -and $client -ne $VmcClient)) { continue }
        $d0 = [Convert]::ToUInt64($Matches[6], 16); $d1 = [Convert]::ToUInt64($Matches[7], 16)
        $addr = ($d0 -shl 12) -bor (($d1 -band 0xF) -shl 44)
        $write = ($d1 -band 0x20) -ne 0
        $retry = ($d1 -band 0x80) -ne 0
        $what = 'outside every range this client printed'
        foreach ($rg in $ranges) { if ($addr -ge $rg.Lo -and $addr -le $rg.Hi) { $what = $rg.Name; break } }
        Say ("FAULT client {0} vmid {1} page 0x{2:X12} {3}{4} -> {5}" -f $client, $Matches[4], $addr,
             $(if ($write) { 'write' } else { 'read' }), $(if ($retry) { ' retry' } else { '' }), $what)
    }
    Say "vm faults during run: $faults"

    $cb = Get-Coherence (Read-Lines (Join-Path $dir 'log-before.txt'))
    $ca = Get-Coherence (Read-Lines (Join-Path $dir 'log-after.txt'))
    foreach ($key in ($ca.Keys | Sort-Object)) {
        $b = @(0, 0, 0); if ($cb.ContainsKey($key)) { $b = $cb[$key] }
        $a = $ca[$key]
        Say ("vidmm PTE encoding {0}: coherent {1} (+{2}) noncoherent {3} (+{4}) snoop mismatches {5} (+{6})" -f $key,
             $a[0], ($a[0] - $b[0]), $a[1], ($a[1] - $b[1]), $a[2], ($a[2] - $b[2]))
    }
}

$code = $run.Code
$meaning = 'killed by the runner after the bound (client hung in a call; see monfence.out tail)'
if ($null -ne $code) {
    if ($ExitMeaning.ContainsKey([int]$code)) { $meaning = $ExitMeaning[[int]$code] } else { $meaning = 'unknown exit code (crash?)' }
}
$resultLine = @($out | Where-Object { $_ -match '^RESULT ' })
if ($resultLine.Count -gt 0) { Say $resultLine[-1] } else { Say 'RESULT line missing (watchdog, kill or crash)' }
Say "MONFENCE-RUN exit=$code killed=$($run.Killed) vm_faults=$faults meaning=$meaning"
$summary | Set-Content -LiteralPath (Assert-UnderTmp (Join-Path $dir 'summary.txt')) -Encoding ASCII
if ($null -eq $code) { exit 99 }
exit [int]$code
