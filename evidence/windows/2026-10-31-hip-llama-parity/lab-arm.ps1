# One HIP parity arm on unit A, bounded. Builds a run directory from a llama.cpp build (new|base) and a HIP
# runtime (t34 = installed tester.34, rel|lab = hip/runtime-token-cost builds), runs one tool, prints its output tail
# with Tctl, GPU clock and CU before and after. Every arm stays inside -Bound seconds (default 170, rule: 3 minutes).
param(
    [ValidateSet('new', 'base', 'cap', 'bload', 'ovl', 'e57', 'a1g')][string]$Llama = 'new',
    [ValidateSet('t34', 'rel', 'lab', 'srel', 'slab', 'socc', 'sovl', 't35r', 't35l', 't35pkg', 'idl', 'idr', 'idl2', 'idr2', 'idl3', 'idr3', 'set35r', 'set35l', 'inst35')][string]$Runtime = 't34',
    [ValidateSet('bench', 'ops', 'ppl', 'chat')][string]$Tool = 'bench',
    [string]$Model = '',
    [string]$ToolArgs = '',
    [string]$EnvSet = '',
    [int]$Bound = 170,
    [string]$Tag = ''
)
$ErrorActionPreference = 'Continue'
$k = 'C:\BC250\tmp\t35hip'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
function Now { [DateTime]::UtcNow.ToString('HH:mm:ss') }
function Probe($w) {
    $t = (& $cli clock read 2>&1 | Select-String 'MHz=' | Select-Object -First 1).Line
    "$(Now) $w $t"
}
$run = Join-Path $k ("run-$Llama-$Runtime")
New-Item -ItemType Directory -Force $run | Out-Null
Copy-Item (Join-Path $k "llama-$Llama\*") $run -Force
Copy-Item (Join-Path $k "rt-$Runtime\*") $run -Force
$exe = @{ bench = 'llama-bench.exe'; ops = 'test-backend-ops.exe'; ppl = 'llama-perplexity.exe'; chat = 'llama-completion.exe' }[$Tool]
$a = @()
if ($Model) { $a += '-m'; $a += $Model }
$a += @($ToolArgs -split ' ' | Where-Object { $_ })
foreach ($p in ($EnvSet -split ';' | Where-Object { $_ })) { $n, $v = $p -split '=', 2; Set-Item "env:$n" $v }
"$(Now) arm $Tag llama=$Llama runtime=$Runtime tool=$Tool env=[$EnvSet] args=[$($a -join ' ')]"
"hashes: ggml-hip $((Get-FileHash (Join-Path $run 'ggml-hip.dll')).Hash.Substring(0,8)) amdhip64 $((Get-FileHash (Join-Path $run 'amdhip64.dll')).Hash.Substring(0,8))"
Probe 'before'
$out = Join-Path $run "$Tool.out"; $err = Join-Path $run "$Tool.err"
$t0 = Get-Date
$p = Start-Process -FilePath (Join-Path $run $exe) -ArgumentList $a -WorkingDirectory $run -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$null = $p.Handle   # PS 5.1: without a handle taken before the exit, ExitCode reads back empty
$hot = $false
while (-not $p.WaitForExit(5000)) {
    if (((Get-Date) - $t0).TotalSeconds -gt $Bound) { Stop-Process -Id $p.Id -Force; "$(Now) KILLED at bound $Bound s"; break }
}
"$(Now) exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalSeconds) s"
Probe 'after'
Get-Content $out | Select-Object -Last 40
Get-Content $err | Where-Object { $_ -match 'error|fail|Error|ggml_cuda_init|found|Device|warning: |final estimate|PPL|tok' } | Select-Object -Last 15
