# One isolated gate process. The parent owns its deadline and descendant job.
param([Parameter(Mandatory)][string]$Plan,[Parameter(Mandatory)][string]$Name)
$ErrorActionPreference='Stop'
$planData=Get-Content -LiteralPath $Plan -Raw | ConvertFrom-Json -AsHashtable
$job=@($planData.jobs | Where-Object name -eq $Name)
if($job.Count -ne 1){throw "Gate name is not unique: $Name"}
foreach($key in $planData.variables.Keys){Set-Variable -Name $key -Value $planData.variables[$key]}
# Quick's actions use their original script directory, never a caller's cwd.
$qualityRoot=Join-Path $repo 'tools/quality'
$quickPath=Join-Path $qualityRoot 'quick.ps1'
$global:LASTEXITCODE=0
try {
    if($job[0].runner) {
        & pwsh -NoProfile -File (Join-Path $repo $job[0].runner) @($job[0].arguments) -Out (Join-Path $Out $Name)
    } else {
        & ([scriptblock]::Create($job[0].action))
    }
    exit $LASTEXITCODE
} catch { Write-Error $_; exit 1 }
