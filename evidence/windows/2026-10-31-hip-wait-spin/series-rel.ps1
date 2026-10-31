# Dev PC side: the default spin of the release runtime, with no switch set. Each arm is one sweep-env.ps1 call
# (Tctl and STOP checked before it, 170 s bound). T35_NOENV=1 is a variable that no part of the stack reads: it
# keeps the environment set of an arm not empty. The lab runtime with BC250_HIP_WAIT_SPIN_US=0 is the no-spin control.
param([string]$Llama = 'a1g', [string]$Rel = 'idr3', [string]$Lab = 'idl3')
$m15 = 'C:\BC250\llmvram\models\qwen2.5-1.5b-instruct-q4_k_m.gguf'
$m8 = 'C:\BC250\llmvram\models\Qwen3-8B-Q8_0.gguf'
$tg = '-p 0 -n 64 -fa 1 -ngl 99 -r 3'
$pp = '-p 512 -n 0 -fa 1 -ngl 99 -r 3'
$arms = @(
    @('1.5B tg64', $m15, $tg, $Rel, 'T35_NOENV=1'),
    @('1.5B tg64', $m15, $tg, $Lab, 'BC250_HIP_WAIT_SPIN_US=0'),
    @('1.5B tg64', $m15, $tg, $Lab, 'T35_NOENV=1'),
    @('1.5B tg64', $m15, $tg, $Rel, 'T35_NOENV=1'),
    @('1.5B pp512', $m15, $pp, $Rel, 'T35_NOENV=1'),
    @('8B tg64', $m8, $tg, $Rel, 'T35_NOENV=1'),
    @('8B tg64', $m8, $tg, $Lab, 'BC250_HIP_WAIT_SPIN_US=0'),
    @('8B tg64', $m8, $tg, $Rel, 'T35_NOENV=1')
)
foreach ($a in $arms) {
    "#### $($a[0]) $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z llama=$Llama runtime=$($a[3]) args=[$($a[2])]"
    & P:\BC-250\scratch\t35\hip-lab\sweep-env.ps1 -Model $a[1] -Sets @($a[4]) -Llama $Llama -Runtime $a[3] -ToolArgs $a[2]
}
"#### done $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
