# Dev PC side: one llama-bench arm per goal model (8B, 1.5B, 35B) on the current start, for a large-page arm
# (the KMD LargePages value of this start is set before the restart). sweep-env.ps1 checks Tctl and STOP before
# each arm; each arm stays inside 170 s. -Tag names the arm in the log.
param([string]$Tag = 'lp', [string]$Llama = 'bload', [string]$Runtime = 'slab', [string[]]$Sets = @('X=0'))
$m = [ordered]@{
    '8B'   = 'C:\BC250\llmvram\models\Qwen3-8B-Q8_0.gguf'
    '1.5B' = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf'
    '35B'  = 'C:\BC250\strata\models\Qwen3.6-35B-A3B-UD-IQ2_M.gguf'
}
foreach ($k in $m.Keys) {
    "#### $Tag $k $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
    & P:\BC-250\scratch\t35\hip-lab\sweep-env.ps1 -Model $m[$k] -Sets $Sets -Llama $Llama -Runtime $Runtime
}
"#### done $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
