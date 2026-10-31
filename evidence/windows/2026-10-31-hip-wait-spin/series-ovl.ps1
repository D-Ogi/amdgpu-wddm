# Dev PC side: the hip-overlap kit arms (scratch\t35\hip-overlap\kit\README.txt), llama-bench per model and per
# environment set, in the order of -Sets (A, B, C, D, then A again as the drift control). sweep-env.ps1 checks Tctl
# and STOP before each arm; each arm stays inside 170 s. -Models picks from 8B, 1.5B, 35B (35B needs 12 GiB).
param([string]$Tag = 'ovl', [string]$Llama = 'ovl', [string]$Runtime = 'sovl', [string[]]$Models = @('1.5B', '8B'),
      [string]$ToolArgs = '-p 512 -n 64 -fa 1 -ngl 99 -r 2',
      [string[]]$Sets = @('BC250_HIP_STATS=1',
                          'BC250_HIP_STATS=1;BC250_HIP_STREAM_OVERLAP=1',
                          'BC250_HIP_STATS=1;GGML_CUDA_GRAPH_OPT=1;GGML_CUDA_GRAPH_OPT_NO_GRAPH=1',
                          'BC250_HIP_STATS=1;BC250_HIP_STREAM_OVERLAP=1;GGML_CUDA_GRAPH_OPT=1;GGML_CUDA_GRAPH_OPT_NO_GRAPH=1',
                          'BC250_HIP_STATS=1'))
$m = [ordered]@{
    '8B'   = 'C:\BC250\llmvram\models\Qwen3-8B-Q8_0.gguf'
    '1.5B' = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf'
    '35B'  = 'C:\BC250\strata\models\Qwen3.6-35B-A3B-UD-IQ2_M.gguf'
}
# pwsh -File passes "1.5B,8B" as one string: split it here.
$Models = @($Models | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim(" '") } | Where-Object { $_ })
foreach ($k in $Models) {
    if (-not $m.Contains($k)) { "unknown model '$k' (8B, 1.5B, 35B)"; exit 2 }
    "#### $Tag $k $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z llama=$Llama runtime=$Runtime args=[$ToolArgs]"
    & P:\BC-250\scratch\t35\hip-lab\sweep-env.ps1 -Model $m[$k] -Sets $Sets -Llama $Llama -Runtime $Runtime -ToolArgs $ToolArgs
}
"#### done $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
