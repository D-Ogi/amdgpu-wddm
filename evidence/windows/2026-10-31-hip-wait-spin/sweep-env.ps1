# Dev PC side: one lab-arm bench per environment set on one model, with the Tctl check before each arm.
# Prints the pp/tg rows and the clock after each arm. Stops above 80 C before an arm (the 87 C rule keeps margin).
param([string]$Model, [string[]]$Sets, [string]$ToolArgs = '-p 512 -n 64 -fa 1 -ngl 99 -r 2', [string]$Llama = 'cap', [string]$Runtime = 'slab')
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
foreach ($e in $Sets) {
    $t = python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1
    if ($t -match 'Tctl ([\d.]+)' -and [double]$Matches[1] -gt 80) { "STOP: $t"; break }
    if ((python tools\win\bc250mon\mon.py stop? 2>&1) -notmatch 'No stop') { 'STOP flag set'; break }
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-arm.ps1 -Llama $Llama -Runtime $Runtime -Tool bench -Model $Model -ToolArgs $ToolArgs -EnvSet $e -Tag sweep 2>&1 | Out-Null
    "== [$e] $t"
    python tools\win\target.py ps P:\BC-250\scratch\t35\hip-lab\lab-errtail.ps1 -Run "run-$Llama-$Runtime" -Tool bench -Lines 3 2>&1 |
        Select-String 'pp512|tg64|MHz=|error|fail|amdhip64 stats' | ForEach-Object { ($_.Line -replace '\s+', ' ') }
}
