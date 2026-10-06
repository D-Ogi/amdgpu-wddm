# L-CU: GUI-PLAN-v7, task 496. Infrastructure prepared only; no lab run recorded.
param(
    [Parameter(Mandatory)][ValidateSet('Prepare','Start','Observe','Stop','Cleanup','Supervise','Worker','Guard')][string]$Mode,
    [Parameter(Mandatory)][string]$ConfigPath
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\gui-trial-common.ps1"
Invoke-GuiTrial -Mode $Mode -ConfigPath $ConfigPath -Trial 'L-CU' -Script $PSCommandPath
