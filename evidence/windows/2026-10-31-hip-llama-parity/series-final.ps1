# Dev PC side: the tester.35 HIP measurement on one runtime and one llama build, for the MEASURED file of the llama
# asset and the parity table. Protocol of the published bc250-rocm figures (TARGETS.md): llama-bench -fa 1 -ngl 99
# (their -mmp 0 is not a switch of this llama-bench), pp512 (-p 512 -n 0 -r 3) and tg64 (-p 0 -n 64 -r 3) in separate
# invocations, -Reps invocations each.
# Then llama-perplexity on the three models, test-backend-ops (series-ops.ps1 groups) and one chat answer through
# the model's chat template. Before every arm: Tctl under -CoolC (one read every 20 s, at most 240 s), the STOP flag,
# and a stop above 80 C. Each arm stays inside the 170 s bound of lab-arm.ps1.
param([string]$Llama = 'a1g', [string]$Runtime = 'set35r', [string]$Tag = 'final', [int]$Reps = 2,
      [double]$CoolC = 68, [string[]]$Parts = @('bench', 'ppl', 'ops', 'chat'), [string]$EnvSet = 'T35_NOENV=1')
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$m = [ordered]@{
    '1.5B' = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf'
    '8B'   = 'C:\BC250\llmvram\models\Qwen3-8B-Q8_0.gguf'
    '35B'  = 'C:\BC250\strata\models\Qwen3.6-35B-A3B-UD-IQ2_M.gguf'
}
function Tctl { $t = python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1; if ($t -match 'Tctl ([\d.]+)') { [double]$Matches[1] } else { 999 } }
function Ready {
    $t0 = Get-Date; $c = Tctl
    while ($c -gt $CoolC -and ((Get-Date) - $t0).TotalSeconds -lt 240) { Start-Sleep -Seconds 20; $c = Tctl }
    if ($c -gt 80) { "STOP: Tctl $c"; return $false }
    if ((python tools\win\bc250mon\mon.py stop? 2>&1) -notmatch 'No stop') { 'STOP flag set'; return $false }
    "Tctl $c"; return $true
}
function Arm($tool, $model, $toolArgs, $tag) {
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-arm.ps1 -Llama $Llama -Runtime $Runtime -Tool $tool -Model $model -ToolArgs $toolArgs -EnvSet $EnvSet -Tag $tag 2>&1 |
        Select-String 'arm |hashes:|exit |KILLED|MHz=|pp512|tg64|Final estimate|error|fail|Paris|> ' | ForEach-Object { ($_.Line -replace '\s+', ' ').Trim() }
}
"#### $Tag start $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z llama=$Llama runtime=$Runtime"
$stop = $false
if ($Parts -contains 'bench') {
    foreach ($k in $m.Keys) {
        foreach ($row in @(@('pp512', '-p 512 -n 0'), @('tg64', '-p 0 -n 64'))) {
            for ($r = 1; $r -le $Reps; $r++) {
                $ok = Ready; $ok[0..($ok.Count - 2)]; if (-not $ok[-1]) { $stop = $true; break }
                "#### $Tag bench $k $($row[0]) rep $r $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
                # This llama-bench has no -mmp switch (-lm/--load-mode replaces it). The load mode stays at its
                # default, as in RESULTS.md; it changes how the weights are read, not the pp/tg timings.
                Arm 'bench' $m[$k] "-fa 1 -ngl 99 $($row[1]) -r 3" "$Tag-$k-$($row[0])-$r"
            }
            if ($stop) { break }
        }
        if ($stop) { break }
    }
}
if (-not $stop -and $Parts -contains 'ppl') {
    foreach ($k in $m.Keys) {
        $ok = Ready; $ok[0..($ok.Count - 2)]; if (-not $ok[-1]) { $stop = $true; break }
        "#### $Tag ppl $k $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
        Arm 'ppl' $m[$k] '-f C:\BC250\tmp\bd117-fa-r3\wikitext2-test-prefix.txt -c 512 --chunks 16 -fa on -ngl 99' "$Tag-ppl-$k"
    }
}
if (-not $stop -and $Parts -contains 'ops') {
    "#### $Tag ops $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
    & P:\BC-250\scratch\t35\hip-lab\series-ops.ps1 -Tag "$Tag-ops" -Llama $Llama -Runtime $Runtime -EnvSet $EnvSet -CoolC $CoolC
}
if (-not $stop -and $Parts -contains 'chat') {
    $ok = Ready; $ok[0..($ok.Count - 2)]
    if ($ok[-1]) {
        "#### $Tag chat 8B $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
        Arm 'chat' $m['8B'] '--jinja -cnv -st -f C:\BC250\tmp\t35hip\chat\chat-in\chat-paris.txt -n 256 -ngl 99 -fa on --temp 0' "$Tag-chat"
    }
}
"#### $Tag done $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
