# Dev PC side, gate A2 of scratch\t35\release\FINAL-GATES.md: HIP on the INSTALLED tester.35 (12 GiB carve-out,
# 40 CU, DPM floor 1500). llama-bench pp512 and tg64 of the three models (-Repeat runs each), from <InstallRoot>\tools\llama
# with the runtime that Windows finds through PATH (tools\hip; lab-arm-inst.ps1); the 8B perplexity with the asset
# build's llama-perplexity on a copy of tools\hip (lab-arm.ps1 -Llama a1g -Runtime inst35); one chat answer from the
# installed llama-completion. Before every arm: Tctl under -CoolC, the STOP flag, a stop above 80 C. Each arm stays
# inside the 170 s bound. The rows must match H0 (series-finalb.log, series-final.log). -Repeat invocations of each
# bench row (2, as H0).
param([double]$CoolC = 68, [string]$Tag = 'a2', [string]$EnvSet = 'T35_NOENV=1', [int]$Repeat = 2)
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$H = 'P:\BC-250\scratch\t35\hip-lab'
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
$keep = 'arm |hashes:|runtime search|exit |KILLED|MHz=|pp512|tg64|Final estimate|error|fail|Paris|> |^no '
function Inst($tool, $model, $toolArgs, $tag) {
    python tools\win\target.py ps "$H\lab-arm-inst.ps1" -Tool $tool -Model $model -ToolArgs $toolArgs -EnvSet $EnvSet -Tag $tag 2>&1 |
        Select-String $keep | ForEach-Object { ($_.Line -replace '\s+', ' ').Trim() }
}
function Copied($tool, $model, $toolArgs, $tag) {
    python tools\win\target.py ps "$H\lab-arm.ps1" -Llama a1g -Runtime inst35 -Tool $tool -Model $model -ToolArgs $toolArgs -EnvSet $EnvSet -Tag $tag 2>&1 |
        Select-String $keep | ForEach-Object { ($_.Line -replace '\s+', ' ').Trim() }
}
"#### $Tag start $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z llama=installed runtime=installed"
python tools\win\target.py ps "$H\prep-inst35.ps1" 2>&1 | Select-Object -Last 6
python tools\win\target.py ps "$H\lab-clock-pin.ps1" -MHz 1500 2>&1 | Select-Object -Last 4
$stop = $false
foreach ($k in $m.Keys) {
    foreach ($row in @(@('pp512', '-p 512 -n 0'), @('tg64', '-p 0 -n 64'))) {
        foreach ($i in 1..$Repeat) {
            $ok = Ready; $ok[0..($ok.Count - 2)]; if (-not $ok[-1]) { $stop = $true; break }
            "#### $Tag bench $k $($row[0]) run $i $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
            Inst 'bench' $m[$k] "-fa 1 -ngl 99 $($row[1]) -r 3" "$Tag-$k-$($row[0])-$i"
        }
        if ($stop) { break }
    }
    if ($stop) { break }
}
if (-not $stop) {
    $ok = Ready; $ok[0..($ok.Count - 2)]
    if ($ok[-1]) {
        "#### $Tag ppl 8B $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
        Copied 'ppl' $m['8B'] '-f C:\BC250\tmp\bd117-fa-r3\wikitext2-test-prefix.txt -c 512 --chunks 16 -fa on -ngl 99' "$Tag-ppl-8B"
        $ok = Ready; $ok[0..($ok.Count - 2)]
        if ($ok[-1]) {
            "#### $Tag chat 8B $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
            Inst 'chat' $m['8B'] '--jinja -cnv -st -f C:\BC250\tmp\t35hip\chat\chat-in\chat-paris.txt -n 256 -ngl 99 -fa on --temp 0' "$Tag-chat"
        }
    }
}
"#### $Tag done $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
