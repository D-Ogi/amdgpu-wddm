# Runs on unit A through postflight.py: the read-only postflight.ps1 inside a 30-second bounded child.
# bounded-child.exe opens its stdout/stderr files with CREATE_NEW and exits 125 before starting anything when they
# exist. Each run therefore gets its own directory, and only that run's files are ever printed: kmd175-deploy003
# postflights 1-5 all ended 125 on the first run's postflight.out and printed its stale "Competing test task".
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}-deploy[0-9]{3}$')][string]$Attempt,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ManifestSha256)
$ErrorActionPreference='Stop'
$d="C:\BC250\m15\$Attempt"
. "$d\kmd-transition\identity.ps1"
. "$d\kmd-transition\verify-stage.ps1"
. "$d\kmd168-transition\invoke-bounded.ps1"
if($d -notmatch $KmdDirectoryPattern){throw 'Unexpected directory'}
Assert-KmdStage $d $ManifestSha256|Out-Null
$run=Join-Path $PSScriptRoot ('postflight-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory $run|Out-Null
$r=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+30*[Diagnostics.Stopwatch]::Frequency) -Stdout "$run\postflight.out" -Stderr "$run\postflight.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$d\kmd-transition\postflight.ps1",'-Directory',$d)
'run '+$run
$r|ConvertTo-Json -Compress
Get-Content "$run\postflight.out" -Raw
if($r.exit_code -ne 0){
 Get-Content "$run\postflight.err" -Raw
 if($r.exit_code -eq 125 -and !$r.stdout){'bounded-child refused before starting the child (exit 125, no receipt)'}
 # Diagnostic only (the frozen check decides): the tasks postflight.ps1's own filter sees Running right now.
 'running tasks now: '+((@(Get-ScheduledTask|Where-Object {$_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI'})|ForEach-Object {$_.TaskPath+$_.TaskName}) -join ', ')
 throw 'Postflight failed'
}
