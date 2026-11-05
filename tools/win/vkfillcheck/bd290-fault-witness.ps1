# BD-290 fault witness: runs one copy-eom case of vkfillcheck against one ICD and reports whether the KMD logged a
# GPU VM fault in the page after the copy's source.
#
# Why a witness is needed: on our stack a read of an unmapped page does not fail the copy. The KMD programs the VM
# contexts as amdgpu does (driver/amdgpu-import/gfxhub_v2_0.c: the default page and an interrupt), so the load reads
# the default page, the shader stores only the bytes of the range, and vkfillcheck sees correct bytes. The KMD logs
# the interrupt as "ih: GPU FAULT va 0x... vmid ..." (driver/kmd/ih.c, every flavour). vkfillcheck prints, on its
# "eom" lines, the page after each copy-eom source. This script reads the KMD log ring (bc250kmd_cli log) before and
# after the case, and the IH vector counts (bc250kmd_cli ih state, lab-flavour KMD only) where the KMD has them.
#
# Windows PowerShell 5.1 (the lab's remote shell), elevated (bc250kmd_cli log needs an administrator).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File bd290-fault-witness.ps1 `
#       -Exe C:\BC250\vkfillcheck\vkfillcheck-x64.exe -Icd 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll' `
#       -Cli C:\BC250\bin\bc250kmd_cli.exe [-Size 4093] [-OutDir C:\BC250\tmp\bd290-witness]
#
# Exit: 0 no fault in the page after the source, 1 one or more faults there (the over-read, also when vkfillcheck
# failed, for example with exit 3 after a fence timeout: that hang is the bug), 2 setup error or an error of this
# script, 3 vkfillcheck did not pass and no fault is in the page after the source, 4 inconclusive (ordinary
# placement, or the KMD log restarted during the run).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Icd,
    [Parameter(Mandatory = $true)][string]$Cli,
    [int]$Size = 4093,
    [string]$OutDir = '',
    [switch]$AnyDevice      # passes --any-device: a dry run on a PC without a BC-250
)
# Continue, not Stop: in Windows PowerShell 5.1 a native program's stderr under 2>&1 becomes an error record, and Stop
# ends the script there (seen with the release bc250kmd_cli, which answers "unknown command ih"). Every native call
# below checks its own exit code.
$ErrorActionPreference = 'Continue'
# A failure of this script itself is a setup error (exit 2), never a verdict: exit 1 means OVERREAD only.
trap {
    Write-Host "setup: script error: $_"
    exit 2
}

function Get-MaxSequence([string[]]$Lines) {
    $max = -1
    foreach ($l in $Lines) {
        if ($l -match '^\s*(\d+)\s+\d+\.\d{3}\s') { $s = [int64]$Matches[1]; if ($s -gt $max) { $max = $s } }
    }
    return $max
}

function Get-Utcl2Counts([string[]]$Lines) {
    # "K client <id> source <id> count <n>" from bc250kmd_cli ih state; UTCL2 is client 0x1B (27), ih_fault.h.
    $h = @{}
    foreach ($l in $Lines) {
        if ($l -match '^K client 27 source (\d+) count (\d+)') { $h[$Matches[1]] = [int64]$Matches[2] }
    }
    return $h
}

foreach ($p in @($Exe, $Icd, $Cli)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "setup: not found: $p"; exit 2 }
}
if ($Size -lt 1 -or $Size -gt 2097152) { Write-Host "setup: -Size $Size is outside 1..2097152"; exit 2 }
if (-not $OutDir) { $OutDir = Join-Path 'C:\BC250\tmp' ('bd290-witness-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 1. Before: the log ring and the IH counts.
$logBefore = @(& $Cli log 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { Write-Host "setup: '$Cli log' exit $LASTEXITCODE (an elevated shell is needed)"; $logBefore | Select-Object -First 3; exit 2 }
$logBefore | Set-Content -Path (Join-Path $OutDir 'log-before.txt') -Encoding ascii
$seqBefore = Get-MaxSequence $logBefore
$ihBefore = @(& $Cli ih state 2>&1 | ForEach-Object { "$_" })
$ihCode = $LASTEXITCODE
$ihBefore | Set-Content -Path (Join-Path $OutDir 'ih-before.txt') -Encoding ascii
$haveIh = ($ihCode -eq 0) -and -not ($ihBefore -match '^refused')
$utclBefore = Get-Utcl2Counts $ihBefore

# 2. The case.
$case = "copy-eom:${Size}:4"
$runArgs = @('--icd', $Icd, '--case', $case)
if ($AnyDevice) { $runArgs += '--any-device' }
$run = @(& $Exe @runArgs 2>&1 | ForEach-Object { "$_" })
$runCode = $LASTEXITCODE
$run | Set-Content -Path (Join-Path $OutDir 'vkfillcheck.txt') -Encoding ascii
Start-Sleep -Seconds 2     # the IH DPC logs after the interrupt; give it time before the second read

# 3. After.
$logAfter = @(& $Cli log 2>&1 | ForEach-Object { "$_" })
$logAfter | Set-Content -Path (Join-Path $OutDir 'log-after.txt') -Encoding ascii
$seqAfter = Get-MaxSequence $logAfter
$ihAfter = @()
if ($haveIh) {
    $ihAfter = @(& $Cli ih state 2>&1 | ForEach-Object { "$_" })
    $ihAfter | Set-Content -Path (Join-Path $OutDir 'ih-after.txt') -Encoding ascii
}
$utclAfter = Get-Utcl2Counts $ihAfter

# 4. The pages vkfillcheck printed for this case.
$next = @{}; $last = @{}
$placement = 'unknown'
foreach ($l in $run) {
    if ($l -match '^eom placement capture-replay') { $placement = 'capture-replay' }
    elseif ($l -match '^eom placement ORDINARY') { $placement = 'ordinary' }
    if ($l -match 'last source page 0x([0-9A-Fa-f]+) next page 0x([0-9A-Fa-f]+)') {
        $last[[Convert]::ToUInt64($Matches[1], 16)] = 1
        $next[[Convert]::ToUInt64($Matches[2], 16)] = 1
    }
}

# 5. New fault lines, by page.
$inNext = @(); $inLast = @(); $other = @()
foreach ($l in $logAfter) {
    if ($l -notmatch '^\s*(\d+)\s+\d+\.\d{3}\s') { continue }
    if ([int64]$Matches[1] -le $seqBefore) { continue }
    if ($l -match 'ih: GPU FAULT va 0x([0-9A-Fa-f]+)') {
        $va = [Convert]::ToUInt64($Matches[1], 16)
        $page = $va - ($va % [UInt64]4096)
        if ($next.ContainsKey($page)) { $inNext += $l } elseif ($last.ContainsKey($page)) { $inLast += $l } else { $other += $l }
    }
}

Write-Host "icd          $Icd"
$run | Where-Object { $_ -match '^(icd module|eom|PASS|FAIL|cases|result)' } | ForEach-Object { Write-Host "  $_" }
Write-Host ("pages        next {0}; last source {1}" -f (($next.Keys | ForEach-Object { '0x{0:X}' -f $_ }) -join ' '),
    (($last.Keys | ForEach-Object { '0x{0:X}' -f $_ }) -join ' '))
Write-Host "log          sequence $seqBefore before, $seqAfter after"
Write-Host ("faults       {0} in the page after the source, {1} in the last source page, {2} elsewhere" -f $inNext.Count, $inLast.Count, $other.Count)
foreach ($l in ($inNext + $inLast + $other)) { Write-Host "  $l" }
if ($haveIh) {
    $keys = @($utclBefore.Keys) + @($utclAfter.Keys) | Sort-Object -Unique
    foreach ($k in $keys) {
        $b = 0; $a = 0
        if ($utclBefore.ContainsKey($k)) { $b = $utclBefore[$k] }
        if ($utclAfter.ContainsKey($k)) { $a = $utclAfter[$k] }
        Write-Host ("ih           UTCL2 source {0}: {1} -> {2} (+{3})" -f $k, $b, $a, ($a - $b))
    }
    if ($keys.Count -eq 0) { Write-Host 'ih           no UTCL2 vectors before or after' }
} else {
    Write-Host 'ih           IH counts not available (release-flavour KMD); the log ring is the witness'
}
Write-Host "output       $OutDir"

if ($seqBefore -lt 0 -or $seqAfter -lt $seqBefore) { Write-Host 'verdict      INCONCLUSIVE: the KMD log ring restarted or was empty'; exit 4 }
# A fault in the next page is the over-read whatever vkfillcheck returned. On unit A the faulting copy can also stop
# the queue: the fence times out and vkfillcheck exits 3. That hang is a result of the bug, so the verdict is still
# OVERREAD and not VKFILLCHECK-FAILED.
if ($inNext.Count -gt 0) {
    $why = ''
    if ($runCode -ne 0) { $why = " (vkfillcheck exit ${runCode}: the fault also stopped the copy)" }
    Write-Host "verdict      OVERREAD: the copy loaded the page after its source$why"
    exit 1
}
if ($runCode -ne 0) { Write-Host "verdict      VKFILLCHECK-FAILED (exit $runCode) with no fault in the page after the source"; exit 3 }
if ($placement -ne 'capture-replay') { Write-Host "verdict      INCONCLUSIVE: placement $placement, the next page may be mapped"; exit 4 }
Write-Host 'verdict      NO-OVERREAD: no fault in the page after the source'
exit 0
