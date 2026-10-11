param(
    [Parameter(Mandatory)][int]$Port,
    [Parameter(Mandatory)][string]$Out,
    [string]$ArgsB64 = '',
    [int]$Remaining = 240
)
# Run 1 of the Performant Labs comparison: decode on an empty context, then a depth ladder.
#
# The ladder grows ONE prompt in token-array form, so the hybrid model never has to roll back its recurrent
# state: each request is the previous prompt tokens, then the tokens the model just generated
# (`return_tokens`), then the next chunk of reference text. `cache_prompt` is on, so the server reprocesses
# only the new tail. After every step the client checks that `prompt_n` is about the new tokens only; if the
# server reprocesses the whole prefix instead, the ladder stops and says so.
#
# Every finished step is appended to ladder.jsonl before the next one starts, so a stop at the deadline
# still leaves the measured points on disk.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'perf-http.ps1')

$sw = [Diagnostics.Stopwatch]::StartNew()
Publish-ClientPid $Out
$base = "http://127.0.0.1:$Port"
$argText = if ($ArgsB64) { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ArgsB64.Trim('"'))) } else { '' }
$cfg = @{}
foreach ($kv in ($argText -split ';' | Where-Object { $_ -match '=' })) {
    $i = $kv.IndexOf('='); $cfg[$kv.Substring(0, $i)] = $kv.Substring($i + 1)
}
function Cfg([string]$key, $default) {
    if ($cfg.ContainsKey($key) -and $cfg[$key]) { return $cfg[$key] }
    return $default
}
$chunk   = [int](Cfg 'chunk'   4096)
$decode  = [int](Cfg 'decode'  64)
$empty   = [int](Cfg 'empty'   128)
$empties = [int](Cfg 'empties' 2)
$promptFile = [string](Cfg 'prompt' 'C:\BC250\perf\prompt.txt')
# ignore_eos=1 makes every decode run the full n_predict tokens. Without it a model that emits EOS early
# ends the run, and the decode rate is then measured over fewer tokens than the row claims. Default off,
# so the arms already recorded in RESULT-performant.md keep the behaviour they ran with.
$ignoreEos = [int](Cfg 'ignore_eos' 0)
$eosJson = if ($ignoreEos) { ',"ignore_eos":true' } else { '' }
"client=client-ladder remaining_s=$Remaining chunk=$chunk decode=$decode empty=$empty empties=$empties ignore_eos=$ignoreEos"
"prompt_file=$promptFile"

$ladder = Join-Path $Out 'ladder.jsonl'
$bodyFile = Join-Path $Out 'req-body.json'
$respFile = Join-Path $Out 'req-resp.json'
$props = Invoke-Get "$base/props"
if ($props) {
    'props_n_ctx=' + $props.default_generation_settings.n_ctx
    'props_model=' + $props.model_path
}

# ---------------------------------------------------------------- (a) decode on an empty context
$emptyResults = @()
for ($k = 1; $k -le $empties; $k++) {
    $p = 'Explain in one paragraph what memory bandwidth means for a language model that generates tokens.'
    $json = '{"prompt":' + (ConvertTo-Json -InputObject $p -Compress) +
            ',"n_predict":' + $empty + ',"cache_prompt":false,"temperature":0,"top_k":1,"seed":1234' +
            $eosJson + '}'
    $r = Invoke-Post -Url "$base/completion" -Json $json -BodyFile $bodyFile -RespFile $respFile -TimeoutS 120
    if ($r.body -and $r.body.timings) {
        $t = $r.body.timings
        $rec = [pscustomobject]@{
            kind = 'empty-context'; index = $k
            prompt_n = $t.prompt_n; prompt_ms = $t.prompt_ms
            predicted_n = $t.predicted_n; predicted_ms = $t.predicted_ms
            predicted_per_second = $t.predicted_per_second; prompt_per_second = $t.prompt_per_second
            elapsed_s = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        }
        $emptyResults += $rec
        Write-Line $ladder ($rec | ConvertTo-Json -Compress)
        'empty{0}: predicted_n={1} tg={2:N2} tok/s  prompt_n={3} pp={4:N2} tok/s' -f `
            $k, $t.predicted_n, $t.predicted_per_second, $t.prompt_n, $t.prompt_per_second
    } else {
        'empty{0}: FAILED curl_exit={1} {2} {3}' -f $k, $r.curl_exit, $r.curl_err, $r.raw_head
    }
}

# ---------------------------------------------------------------- the reference text, tokenized once
$text = [IO.File]::ReadAllText($promptFile)
'prompt_text_bytes=' + $text.Length
$all = Get-Tokens -Base $base -Text $text -AddSpecial $true -Work $Out -TimeoutS 180
if (-not $all) { 'FAILED /tokenize returned nothing; the ladder cannot run'; exit 2 }
'prompt_tokens_total=' + $all.Count
'tokenize_done_at_s={0:N1}' -f $sw.Elapsed.TotalSeconds

# ---------------------------------------------------------------- (b) the depth ladder
$ctx = New-Object 'System.Collections.Generic.List[int]'
$pos = 0
$step = 0
$cumPromptTokens = 0
$cumPromptMs = 0.0
$stepEstimate = ($chunk / 85.0) + ($decode / 16.0) + 4.0   # first guess only; measured after step 1
$steps = @()
$ladderStopReason = 'text exhausted'
while ($pos -lt $all.Count) {
    $left = $Remaining - $sw.Elapsed.TotalSeconds
    if ($left -lt $stepEstimate) {
        $ladderStopReason = ('budget: {0:N0} s left, next step needs about {1:N0} s' -f $left, $stepEstimate)
        break
    }
    $take = [Math]::Min($chunk, $all.Count - $pos)
    $depthBefore = $ctx.Count
    for ($i = 0; $i -lt $take; $i++) { $ctx.Add($all[$pos + $i]) }
    $pos += $take
    $step++

    $json = '{"prompt":[' + (New-JsonIntArray $ctx) + ']' +
            ',"n_predict":' + $decode + ',"cache_prompt":true,"return_tokens":true' +
            ',"temperature":0,"top_k":1,"seed":1234' + $eosJson + '}'
    $t0 = $sw.Elapsed.TotalSeconds
    $r = Invoke-Post -Url "$base/completion" -Json $json -BodyFile $bodyFile -RespFile $respFile `
                     -TimeoutS ([int][Math]::Max(30, [Math]::Min(280, $Remaining - $t0)))
    $t1 = $sw.Elapsed.TotalSeconds
    if (-not $r.body -or -not $r.body.timings) {
        'step{0}: FAILED curl_exit={1} {2} {3}' -f $step, $r.curl_exit, $r.curl_err, $r.raw_head
        $ladderStopReason = 'request failed at step ' + $step
        break
    }
    $t = $r.body.timings
    $reprocessed = ($t.prompt_n -gt ($take * 1.5)) -and ($step -gt 1)
    $cumPromptTokens += [int]$t.prompt_n
    $cumPromptMs += [double]$t.prompt_ms
    $rec = [pscustomobject]@{
        kind = 'ladder'; step = $step
        depth_before = $depthBefore; chunk_sent = $take; context_after_prompt = $ctx.Count
        prompt_n = $t.prompt_n; prompt_ms = $t.prompt_ms; prompt_per_second = $t.prompt_per_second
        predicted_n = $t.predicted_n; predicted_ms = $t.predicted_ms
        predicted_per_second = $t.predicted_per_second
        cum_prompt_tokens = $cumPromptTokens; cum_prompt_ms = [math]::Round($cumPromptMs, 1)
        cum_prefill_tok_per_s = [math]::Round($cumPromptTokens / ($cumPromptMs / 1000.0), 2)
        reprocessed_prefix = $reprocessed
        wall_s = [math]::Round($t1 - $t0, 1); elapsed_s = [math]::Round($t1, 1)
        tokens_returned = $(if ($r.body.tokens) { $r.body.tokens.Count } else { 0 })
    }
    $steps += $rec
    Write-Line $ladder ($rec | ConvertTo-Json -Compress)
    ('step{0}: depth={1} chunk={2} prompt_n={3} pp={4:N2} tok/s  tg={5:N2} tok/s  wall={6:N1}s cum_pp={7:N2}' -f `
        $step, $depthBefore, $take, $t.prompt_n, $t.prompt_per_second, $t.predicted_per_second,
        ($t1 - $t0), $rec.cum_prefill_tok_per_s)
    if ($reprocessed) {
        'STOP: the server reprocessed the whole prefix (prompt_n={0} for a {1}-token chunk)' -f $t.prompt_n, $take
        $ladderStopReason = 'prefix reprocessed at step ' + $step
        break
    }
    # Keep the generated tokens in the context, so the next request extends the cached prefix exactly.
    if ($r.body.tokens) { foreach ($g in $r.body.tokens) { $ctx.Add([int]$g) } }
    elseif ($r.body.content) {
        $gen = Get-Tokens -Base $base -Text $r.body.content -AddSpecial $false -Work $Out -TimeoutS 30
        if ($gen) { foreach ($g in $gen) { $ctx.Add($g) } }
        else { $ladderStopReason = 'no generated tokens to append at step ' + $step; break }
    }
    $stepEstimate = ($t1 - $t0) * 1.25 + 3.0
}
'ladder_stop_reason=' + $ladderStopReason
'ladder_steps=' + $steps.Count
'client_elapsed_s={0:N1}' -f $sw.Elapsed.TotalSeconds

$final = [pscustomobject]@{
    client = 'client-ladder'
    remaining_budget_s = $Remaining
    chunk = $chunk; decode = $decode; empty_predict = $empty; empty_runs = $empties
    prompt_file = $promptFile; prompt_text_bytes = $text.Length; prompt_tokens_total = $all.Count
    empty_context = $emptyResults
    ladder = $steps
    ladder_stop_reason = $ladderStopReason
    client_elapsed_s = [math]::Round($sw.Elapsed.TotalSeconds, 1)
}
$final | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Out 'result.json') -Encoding UTF8
# The sentinel the supervisor watches for. Written last, so its presence means this client finished.
[IO.File]::WriteAllText((Join-Path $Out 'client.done'), 'exit=0')
'result_written=1'
exit 0
