# E18 witness, on the target: walk a four-level GPU page table out of VRAM through the driver's read escape
# (bc250kmd_cli vtable, physical path) and print what it maps. Read-only.
#   -Root <hex>   physical address of the root table (the "vidmm: root page table ... = physical 0x..." log line)
#   -Find <hex>   optional GPU virtual address: print the chain that translates it
param([Parameter(Mandatory)][string]$Root, [string]$Find = '', [string]$Cli = 'C:\BC250\e16-umd\bc250kmd_cli.exe', [int]$MaxTables = 40)

$ErrorActionPreference = 'Stop'
$mem = & $Cli memory
$vram = [Convert]::ToUInt64((($mem | Select-String '^vram\s+0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+)').Matches[0].Groups[1].Value), 16)
$vlen = [Convert]::ToUInt64((($mem | Select-String '^vram\s+0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+)').Matches[0].Groups[2].Value), 16)
"vram 0x{0:X} + 0x{1:X}" -f $vram, $vlen
$script:tables = 0

function Table([UInt64]$physical) {
    $script:tables++
    $entries = @{}
    foreach ($line in (& $Cli vtable ('{0:X}' -f ($physical - $vram)))) {
        if ($line -match '^\s*(\d+) 0x([0-9A-Fa-f]{16})') { $entries[[int]$Matches[1]] = [Convert]::ToUInt64($Matches[2], 16) }
    }
    $entries
}
function Addr([UInt64]$entry, [bool]$leaf) { if ($leaf) { $entry -band 0x0000FFFFFFFFF000 } else { $entry -band 0x0000FFFFFFFFFFC0 } }
function InVram([UInt64]$p) { $p -ge $vram -and $p -lt ($vram + $vlen) }

$rootPhysical = [Convert]::ToUInt64($Root, 16)
if ($Find) {
    $va = [Convert]::ToUInt64($Find, 16)
    $p = $rootPhysical
    for ($level = 3; $level -ge 0; $level--) {
        $index = [int](($va -shr (12 + 9 * $level)) -band 0x1FF)
        $t = Table $p
        $e = if ($t.ContainsKey($index)) { $t[$index] } else { [UInt64]0 }
        "level {0} table 0x{1:X} index {2,3} entry 0x{3:X16}" -f $level, $p, $index, $e
        if (($e -band 1) -eq 0) { "  not valid: the walk ends here"; break }
        $p = Addr $e ($level -eq 0)
        if ($level -eq 0) { "  VA 0x{0:X} -> physical 0x{1:X} ({2})" -f $va, $p, $(if ($e -band 2) { 'system memory' } else { "VRAM offset 0x{0:X}" -f ($p - $vram) }) }
    }
    return
}

# Full walk, bounded: counts per level, every directory entry checked to name a page inside VRAM.
$queue = New-Object System.Collections.Queue
$queue.Enqueue(@{ P = $rootPhysical; L = 3; Va = [UInt64]0 })
$valid = @(0, 0, 0, 0); $outside = 0; $sys = 0; $local = 0; $shown = 0
while ($queue.Count -gt 0 -and $script:tables -lt $MaxTables) {
    $n = $queue.Dequeue()
    $t = Table $n.P
    foreach ($index in ($t.Keys | Sort-Object)) {
        $e = $t[$index]
        if (($e -band 1) -eq 0) { continue }
        $valid[$n.L]++
        $va = $n.Va + ([UInt64]$index -shl (12 + 9 * $n.L))
        if ($n.L -gt 0) {
            $child = Addr $e $false
            if (-not (InVram $child)) { $outside++; "level {0} index {1}: 0x{2:X16} names a table OUTSIDE VRAM" -f $n.L, $index, $e; continue }
            $queue.Enqueue(@{ P = $child; L = $n.L - 1; Va = $va })
        } else {
            if ($e -band 2) { $sys++ } else { $local++ }
            if ($shown -lt 12) { $shown++; "leaf VA 0x{0:X} = 0x{1:X16}" -f $va, $e }
        }
    }
}
"walked {0} tables (limit {1}): valid entries level3 {2} level2 {3} level1 {4} level0 {5}; leaves: {6} VRAM, {7} system; {8} directory entries outside VRAM" -f `
    $script:tables, $MaxTables, $valid[3], $valid[2], $valid[1], $valid[0], $local, $sys, $outside
