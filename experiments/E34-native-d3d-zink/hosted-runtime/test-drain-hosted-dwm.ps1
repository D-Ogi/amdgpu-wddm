$ErrorActionPreference='Stop'
. "$PSScriptRoot\drain-hosted-dwm.ps1"
$script:events=New-Object 'System.Collections.Generic.List[string]'
$script:procs=@()
function Get-Process {param($Name,$ErrorAction) return $script:procs}
function Stop-Process {param($InputObject,[switch]$Force,$ErrorAction) $script:events.Add("stop:$($InputObject.Id)")}
function Write-DurableText {param($Path,$Text) $j=$Text|ConvertFrom-Json;$script:events.Add($j.phase)}
function New-ControlProcess([int]$Id,[bool]$ExitResult) {
 $p=[pscustomobject]@{Id=$Id;Handle=123;StartTime=[DateTime]::UtcNow;HasExited=$false;ExitResult=$ExitResult}
 $p|Add-Member ScriptMethod WaitForExit {param($Timeout) if($Timeout -ne 5000){throw 'Wrong bound'};$script:events.Add("wait:$($this.Id)");return $this.ExitResult}
 return $p
}
$script:procs=@((New-ControlProcess 100 $true),(New-ControlProcess 200 $true))
$ids=@(Stop-HostedDwmBeforeInterop -Directory 'unused')
if(($script:events -join ',') -ne 'before-stop,stop:100,stop:200,wait:100,wait:200,all-original-processes-exited'){throw 'Drain ordering'}
if($ids.Count -ne 2 -or $ids[0].pid -ne 100 -or $ids[1].pid -ne 200){throw 'Identity receipt'}
$script:events.Clear();$script:procs=@(New-ControlProcess 300 $false)
$threw=$false
try {Stop-HostedDwmBeforeInterop -Directory 'unused' | Out-Null;$script:events.Add('adapter-disable')} catch {if($_ -notmatch 'drain timeout'){throw};$threw=$true}
if(!$threw -or $script:events.Contains('adapter-disable') -or $script:events.Contains('all-original-processes-exited')){throw 'Timeout did not stop transition'}
$script:events.Clear();$script:procs=@()
$ids=@(Stop-HostedDwmBeforeInterop -Directory 'unused')
if($ids.Count -ne 0 -or ($script:events -join ',') -ne 'before-stop,all-original-processes-exited'){throw 'Empty snapshot'}
$script:events.Clear();$script:procs=@(New-ControlProcess 400 $true);$script:procs[0].HasExited=$true
Stop-HostedDwmBeforeInterop -Directory 'unused' | Out-Null
if($script:events.Contains('stop:400') -or !$script:events.Contains('wait:400')){throw 'Already-exited original'}
'PASS drain ordering, original identities, bounded timeout, empty and exited snapshots; no real processes touched'
