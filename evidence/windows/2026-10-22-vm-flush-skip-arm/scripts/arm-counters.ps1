# KMD summary counter deltas of the VM flush arm runs (host side, pwsh 7): from the "before:" and "after:" lines that
# cp-run.ps1 wrote into arm\cp-<label>.log. One line per run: the start temperature, the window between the two
# summary reads, the node-0 jobs, the ring VM flushes, the FLUSH_TLB of the paging stream, the VMID invalidations
# they caused, the paging submissions and the game's average and minimum FPS.
#   pwsh arm-counters.ps1 r0 a1 a2 b1 b2
param([Parameter(ValueFromRemainingArguments)][string[]]$Labels)
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$arm = Join-Path $PSScriptRoot 'arm'
$out = foreach ($l in $Labels) {
    $log = Join-Path $arm "cp-$l.log"
    $vals = @{}
    foreach ($phase in 'before', 'after') {
        $o = @{ sk = 0L }
        foreach ($x in (Get-Content -LiteralPath $log | Where-Object { $_ -match " $($phase): " })) {
            if ($x -match 'node 0 hardware: (\d+) submitted') { $o.jobs = [long]$Matches[1] }
            if ($x -match '\s([\d.]+) wddm summary: VMID pool FLUSH_TLB: (\d+) built, (\d+) VMID') { $o.t = [double]$Matches[1]; $o.ft = [long]$Matches[2]; $o.inv = [long]$Matches[3] }
            if ($x -match '\(paging, open\): (\d+) hardware') { $o.pg = [long]$Matches[1] }
            if ($x -match 'ring VM flush on: (\d+) frames') { $o.rf = [long]$Matches[1] }
            if ($x -match ': (\d+) jobs without the flush') { $o.sk = [long]$Matches[1] }
        }
        $vals[$phase] = $o
    }
    $b = $vals['before']; $a = $vals['after']
    $dt = $a.t - $b.t; $jobs = $a.jobs - $b.jobs; $ft = $a.ft - $b.ft
    $gate = (Select-String -LiteralPath $log -Pattern 'gate temperature: tctl ([\d.]+)' | Select-Object -First 1).Matches[0].Groups[1].Value
    $fps = (Select-String -LiteralPath $log -Pattern 'averageFps ([\d.]+) minFps ([\d.]+)' | Select-Object -First 1).Matches[0]
    '{0}: start {1} C, window {2:F1} s, jobs {3}, ring flushes {4}, FLUSH_TLB {5} ({6:F0}/s, {7:F2}/job), VMID invalidations {8}, paging submissions {9}, game avg {10:F2} min {11:F2}' -f `
        $l, $gate, $dt, $jobs, ($a.rf - $b.rf), $ft, ($ft / $dt), ($ft / $jobs), ($a.inv - $b.inv), ($a.pg - $b.pg),
        [double]$fps.Groups[1].Value, [double]$fps.Groups[2].Value
}
$out
