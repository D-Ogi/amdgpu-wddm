# Dev PC side: llama-perplexity on the three parity models, one bounded lab-arm each, Tctl and STOP checks before
# each arm. Prints the arm line, the hashes, the exit and the final estimate. Input: the wikitext-2 test prefix on
# unit A (64730 bytes), -c 512, -Chunks chunks.
param([string]$Tag = 'ppl', [string]$Llama = 'bload', [string]$Runtime = 'slab', [int]$Chunks = 16,
      [string[]]$Only = @(), [string]$EnvSet = 'X=0')   # an empty argument does not reach the lab: X=0 is inert
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$m = [ordered]@{ '8B' = 'C:\BC250\llmvram\models\Qwen3-8B-Q8_0.gguf'; '1.5B' = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf'; '35B' = 'C:\BC250\strata\models\Qwen3.6-35B-A3B-UD-IQ2_M.gguf' }
$args0 = "-f C:\BC250\tmp\bd117-fa-r3\wikitext2-test-prefix.txt -c 512 --chunks $Chunks -fa on -ngl 99"
foreach ($k in $m.Keys) {
    if ($Only.Count -and $Only -notcontains $k) { continue }
    $t = python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1
    if ($t -match 'Tctl ([\d.]+)' -and [double]$Matches[1] -gt 80) { "STOP: $t"; break }
    if ((python tools\win\bc250mon\mon.py stop? 2>&1) -notmatch 'No stop') { 'STOP flag set'; break }
    "#### $Tag $k $t"
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-arm.ps1 -Llama $Llama -Runtime $Runtime -Tool ppl -Model $m[$k] -ToolArgs $args0 -EnvSet $EnvSet -Tag $Tag 2>&1 |
        Select-String 'arm |hashes:|exit |KILLED|MHz=|Final estimate|error|fail' | ForEach-Object { $_.Line.Trim() }
}
