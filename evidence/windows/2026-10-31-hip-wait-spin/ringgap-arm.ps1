# Dev PC side: the KMD ring-gap counters (ring_gap.h, cumulative since WddmStart) read before and after one
# lab-arm bench, and the difference per node. The summary escape is a Level Two call, so it runs only between arms
# on an idle lab. Tctl and STOP are checked first.
param([string]$Llama = 'e57', [string]$Runtime = 't35l', [string]$Model = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf',
      [string]$ToolArgs = '-p 0 -n 64 -fa 1 -ngl 99 -r 3', [string]$EnvSet = 'BC250_HIP_STATS=1', [string]$Tag = 'gap')
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$t = python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1
if ($t -match 'Tctl ([\d.]+)' -and [double]$Matches[1] -gt 80) { "STOP: $t"; exit 3 }
if ((python tools\win\bc250mon\mon.py stop? 2>&1) -notmatch 'No stop') { 'STOP flag set'; exit 3 }
function Read-Gaps {
    $lines = python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-kmd-summary.ps1 -Pattern 'ring idle|ring gap us' 2>&1
    $r = @{}
    foreach ($l in $lines) {
        if ($l -match 'node (\d+) ring idle (\d+) us in (\d+) gaps') { $r["$($Matches[1]).idle"] = [double]$Matches[2]; $r["$($Matches[1]).gaps"] = [double]$Matches[3] }
        elseif ($l -match 'node (\d+) ring gap us (.*)$') {
            $n = $Matches[1]
            foreach ($kv in ($Matches[2] -split ' ')) { if ($kv -match '^([^:]+):(\d+)$') { $r["$n.b$($Matches[1])"] = [double]$Matches[2] } }
        }
    }
    $r
}
$a = Read-Gaps
python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-arm.ps1 -Llama $Llama -Runtime $Runtime -Tool bench -Model $Model -ToolArgs $ToolArgs -EnvSet $EnvSet -Tag $Tag 2>&1 |
    Select-String 'tg64|pp512|exit|KILLED|MHz=' | ForEach-Object { ($_.Line -replace '\s+', ' ') }
$b = Read-Gaps
"== $Tag [$EnvSet] llama=$Llama runtime=$Runtime $t"
foreach ($n in 0, 1) {
    if (-not $b.ContainsKey("$n.gaps")) { continue }
    $dg = $b["$n.gaps"] - $a["$n.gaps"]; $di = $b["$n.idle"] - $a["$n.idle"]
    $h = foreach ($k in '<16', '16', '32', '64', '128', '512', '2k', '4k', '8k+') { "${k}:$($b["$n.b$k"] - $a["$n.b$k"])" }
    "node $n gaps $dg idle_us $di avg_us $(if ($dg) { [math]::Round($di / $dg, 1) } else { 0 }) | $($h -join ' ')"
}
