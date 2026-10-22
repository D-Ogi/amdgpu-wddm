# Lab side (PowerShell 5.1), read only: which Vulkan ICD a pt-run.ps1 run loaded. Lists the files of the run
# directory and prints every line that names a vulkan_radeon.dll or a radeon_icd.json (loader debug output, module
# lists), at most -Max lines.
#   run-icd-check.ps1 -Run C:\BC250\pathtrace\runs\<stamp>-q2rtx-timedemo [-Max 40]
param([Parameter(Mandatory)][string]$Run, [int]$Max = 40)
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o'))"
if (-not (Test-Path -LiteralPath $Run)) { "no run directory $Run"; exit 1 }
Get-ChildItem -LiteralPath $Run -Recurse -File | ForEach-Object { 'file {0} {1}' -f $_.FullName.Substring($Run.Length), $_.Length }
$hits = Get-ChildItem -LiteralPath $Run -Recurse -File -Include *.log, *.txt, *.cmd |
    Select-String -Pattern 'vulkan_radeon\.dll|radeon_icd\.json' -ErrorAction SilentlyContinue
"icd lines: $(@($hits).Count)"
$hits | Select-Object -First $Max | ForEach-Object { '{0}:{1}: {2}' -f $_.Filename, $_.LineNumber, $_.Line.Trim() }
