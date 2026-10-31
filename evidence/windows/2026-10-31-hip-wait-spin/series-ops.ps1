# Dev PC side: test-backend-ops on the ROCm0 backend in bounded lab arms (one op group each), Tctl and STOP checks
# and a cool-down wait (Tctl under -CoolC, at most 150 s) before each arm. Prints the arm line, the hashes, the exit
# and the count summary of lab-ops-sum.ps1.
param([string]$Tag = 'ops', [string]$Llama = 'bload', [string]$Runtime = 'socc', [string[]]$Only = @(),
      [string]$EnvSet = 'X=0', [double]$CoolC = 68)
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$groups = [ordered]@{
    'mmid'  = '-o MUL_MAT_ID'
    'mmq8'  = '-o MUL_MAT -p type_a=q8_0'
    'mmq4k' = '-o MUL_MAT -p type_a=q4_K'
    'mmiq2' = '-o MUL_MAT -p type_a=iq2_'
    'mmf16' = '-o MUL_MAT -p type_a=f16'
    'fa128' = '-o FLASH_ATTN_EXT -p hsk=128,'
    'fa256' = '-o FLASH_ATTN_EXT -p hsk=256,'
    # Unary and GLU tests carry the op's own name (ggml_op_desc), not UNARY or GLU.
    'misc'  = '-o RMS_NORM,L2_NORM,ROPE,SOFT_MAX,GET_ROWS,SET_ROWS,CPY,CONT,ADD,MUL,SCALE,SWIGLU,SILU,SIGMOID,SOFTPLUS,ARGSORT,SUM_ROWS,TOP_K,SSM_CONV,GATED_DELTA_NET'
}
function Tctl { $t = python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1; if ($t -match 'Tctl ([\d.]+)') { [double]$Matches[1] } else { 999 } }
foreach ($k in $groups.Keys) {
    if ($Only.Count -and $Only -notcontains $k) { continue }
    $t0 = Get-Date
    $c = Tctl
    while ($c -gt $CoolC -and ((Get-Date) - $t0).TotalSeconds -lt 150) { Start-Sleep -Seconds 15; $c = Tctl }
    if ($c -gt 80) { "STOP: Tctl $c"; break }
    if ((python tools\win\bc250mon\mon.py stop? 2>&1) -notmatch 'No stop') { 'STOP flag set'; break }
    "#### $Tag $k Tctl $c"
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-arm.ps1 -Llama $Llama -Runtime $Runtime -Tool ops -ToolArgs "$($groups[$k]) -b ROCm0" -EnvSet $EnvSet -Tag "$Tag-$k" 2>&1 |
        Select-String 'arm |hashes:|exit |KILLED|MHz=' | ForEach-Object { $_.Line.Trim() }
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-ops-sum.ps1 -Run "run-$Llama-$Runtime" 2>&1 | ForEach-Object { $_.TrimEnd() }
}
