# E18 run 003 witness, second form: a D3DKMT client without a context never gets SetRootPageTable, so its root is not
# in the log. VidMm allocates page table pages downwards from the top of the segment; this scans the pages below
# -Top for a level 1 table whose entry for -Va is valid, follows it to the level 0 table and reads the data page.
param([string]$Va = '10000000', [string]$Top = '1FDFFF000', [int]$Pages = 48, [string]$Size = '0x10000', [int]$Hold = 90,
      [string]$Cli = 'C:\BC250\e16-umd\bc250kmd_cli.exe', [string]$Probe = 'C:\BC250\tmp\kmtprobe.exe')

$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\e16\out'
$stamp = (Get-Date).ToString('HHmmss')
$out = Join-Path $dir "kmtprobe-$stamp.txt"
$log = Join-Path $dir "scan-$stamp.txt"
function Say([string]$t) { $t | Tee-Object -FilePath $log -Append }
function Table([UInt64]$offset) {
    $entries = @{}
    foreach ($line in (& $Cli vtable ('{0:X}' -f $offset))) {
        if ($line -match '^\s*(\d+) 0x([0-9A-Fa-f]{16})') { $entries[[int]$Matches[1]] = [Convert]::ToUInt64($Matches[2], 16) }
    }
    $entries
}

$mem = & $Cli memory
$vram = [Convert]::ToUInt64((($mem | Select-String '^vram\s+0x([0-9A-Fa-f]+)').Matches[0].Groups[1].Value), 16)
$p = Start-Process $Probe -ArgumentList "--va 0x$Va --size $Size --hold $Hold" -RedirectStandardOutput $out -RedirectStandardError "$out.err" -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and -not (Select-String -Path $out -Pattern '^RESULT|FAILED|failed' -Quiet -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 500 }
Say ((Get-Content $out | Select-String '^RESULT|MapGpuVirtualAddress|honoured' | ForEach-Object { $_.Line.Trim() }) -join ' | ')

$va = [Convert]::ToUInt64($Va, 16)
$i1 = [int](($va -shr 21) -band 0x1FF); $i0 = [int](($va -shr 12) -band 0x1FF)
$top = [Convert]::ToUInt64($Top, 16)
$found = $false
for ($n = 0; $n -lt $Pages -and -not $found; $n++) {
    $offset = $top - [UInt64]($n * 4096)
    $t = Table $offset
    if ($t.Count -eq 0) { continue }
    $first = ($t.Keys | Sort-Object | Select-Object -First 1)
    Say ("table at VRAM 0x{0:X}: {1} nonzero, first index {2} = 0x{3:X16}" -f $offset, $t.Count, $first, $t[$first])
    if ($t.ContainsKey($i1) -and (($t[$i1] -band 0xFFF) -eq 1)) {
        $child = ($t[$i1] -band 0x0000FFFFFFFFFFC0) - $vram
        $leafs = Table $child
        if ($leafs.ContainsKey($i0) -and (($leafs[$i0] -band 0x73) -eq 0x71)) {
            $found = $true
            Say ("LEVEL 1 candidate at VRAM 0x{0:X}: entry {1} = 0x{2:X16} -> level 0 table at VRAM 0x{3:X}, {4} valid leaves" -f $offset, $i1, $t[$i1], $child, $leafs.Count)
            foreach ($k in ($leafs.Keys | Sort-Object | Select-Object -First 16)) { Say ("  leaf {0,3} (VA 0x{1:X}) = 0x{2:X16}" -f $k, (($va -band 0xFFFFFFFFFFE00000) + ([UInt64]$k -shl 12)), $leafs[$k]) }
            $page = ($leafs[$i0] -band 0x0000FFFFFFFFF000) - $vram
            Say ("data page of VA 0x{0:X} at VRAM 0x{1:X}, first qwords:" -f $va, $page)
            & $Cli vtable ('{0:X}' -f $page) | Select-Object -First 5 | ForEach-Object { Say "  $_" }
            $last = ($leafs.Keys | Sort-Object | Select-Object -Last 1)
            $pageN = ($leafs[$last] -band 0x0000FFFFFFFFF000) - $vram
            Say ("data page of leaf {0} at VRAM 0x{1:X}, first qwords:" -f $last, $pageN)
            & $Cli vtable ('{0:X}' -f $pageN) | Select-Object -First 3 | ForEach-Object { Say "  $_" }
        }
    }
}
if (-not $found) { Say "no level 1 table with a valid entry $i1 found in $Pages pages below 0x$Top" }
if (-not $p.WaitForExit(($Hold + 40) * 1000)) { Say "kmtprobe still running: killing it"; $p.Kill() }
Say ("kmtprobe: " + ((Get-Content $out -Tail 1) -join ''))
