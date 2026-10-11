param(
    [Parameter(Mandatory)][int]$Port,
    [Parameter(Mandatory)][string]$Out,
    [string]$ArgsB64 = '',
    [int]$Remaining = 60
)
# Run 2 of the Performant Labs comparison: the fit check at one context size.
#
# The server is already loaded when this client starts, so the load itself is the fit evidence. This client
# only proves that the loaded server really serves at that context: one short request, 16 tokens. The
# supervisor reads `bc250kmd_cli vram` while the server still holds its buffers, and the llama buffer lines
# are in server.err.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'perf-http.ps1')

$sw = [Diagnostics.Stopwatch]::StartNew()
Publish-ClientPid $Out
$base = "http://127.0.0.1:$Port"
"client=client-fit remaining_s=$Remaining"

$props = Invoke-Get "$base/props"
$nctx = $null
if ($props) {
    $nctx = $props.default_generation_settings.n_ctx
    'props_n_ctx=' + $nctx
    'props_model=' + $props.model_path
} else {
    'props_FAILED'
}

$json = '{"prompt":"State one fact about GPU memory.","n_predict":16,"cache_prompt":false' +
        ',"temperature":0,"top_k":1,"seed":1234}'
$r = Invoke-Post -Url "$base/completion" -Json $json `
                 -BodyFile (Join-Path $Out 'req-body.json') -RespFile (Join-Path $Out 'req-resp.json') `
                 -TimeoutS ([int][Math]::Max(20, $Remaining - 5))
$ok = $false
$rec = $null
if ($r.body -and $r.body.timings) {
    $ok = $true
    $t = $r.body.timings
    $rec = [pscustomobject]@{
        prompt_n = $t.prompt_n; prompt_ms = $t.prompt_ms; prompt_per_second = $t.prompt_per_second
        predicted_n = $t.predicted_n; predicted_ms = $t.predicted_ms
        predicted_per_second = $t.predicted_per_second
    }
    'request_ok predicted_n={0} tg={1:N2} tok/s pp={2:N2} tok/s' -f `
        $t.predicted_n, $t.predicted_per_second, $t.prompt_per_second
} else {
    'request_FAILED curl_exit={0} {1} {2}' -f $r.curl_exit, $r.curl_err, $r.raw_head
}

$final = [pscustomobject]@{
    client = 'client-fit'
    n_ctx_served = $nctx
    request_ok = $ok
    timings = $rec
    client_elapsed_s = [math]::Round($sw.Elapsed.TotalSeconds, 1)
}
$final | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Out 'result.json') -Encoding UTF8
'client_elapsed_s={0:N1}' -f $sw.Elapsed.TotalSeconds
# The sentinel the supervisor watches for. Written last, so its presence means this client finished.
[IO.File]::WriteAllText((Join-Path $Out 'client.done'), 'exit=0')
'result_written=1'
exit 0
