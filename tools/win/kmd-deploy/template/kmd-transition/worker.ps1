param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$Tool)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\run-arm.ps1"
$boundary=Get-Content "$Directory\boundary.json" -Raw|ConvertFrom-Json
$result=Invoke-KmdTransitionArm candidate $Directory $Tool $boundary.qpc $boundary.frequency
Write-DurableText "$Directory\candidate-result.json" ($result|ConvertTo-Json)
if(!$result.success){exit 1}
exit 0
