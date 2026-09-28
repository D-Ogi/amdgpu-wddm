$ErrorActionPreference='Stop'
. "$PSScriptRoot\setup-log.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$read={$script:current.Clone()}
$write={param($s);$script:writes++;$script:current=$s.Clone()}
foreach($prior in @(@{present=$false;value=$null},@{present=$true;value=0},@{present=$true;value=-2147483648},@{present=$true;value=65535})){
 $script:current=$prior.Clone();$script:writes=0;$script:saved=$null
 $before=Start-KmdSetupLog -Read $read -Write $write -Save {param($s);if($script:writes){throw 'Mutation before backup'};$script:saved=$s.Clone()}
 if($script:current.value -ne 65535){throw 'Verbose value missing'}
 Restore-KmdSetupLog $before -Read $read -Write $write
 if(!(Test-KmdSetupLogState $script:current $prior)){throw 'Prior state lost'}
 Restore-KmdSetupLog $before -Read $read -Write $write
}
$script:current=@{present=$false;value=$null};$script:writes=0
Must-Reject {Start-KmdSetupLog -Read $read -Write $write -Save {throw 'Disk full'}}
if($script:writes){throw 'Changed logging without durable backup'}
$prior=$script:current.Clone()
Start-KmdSetupLog -Read $read -Write $write -Save {}|Out-Null
$script:current=@{present=$true;value=32};$writesBefore=$script:writes
Must-Reject {Restore-KmdSetupLog $prior -Read $read -Write $write}
if($script:writes -ne $writesBefore){throw 'Concurrent logging change overwritten'}
Must-Reject {Start-KmdSetupLog -Read $read -Write {} -Save {}}
'PASS: absent/zero/signed/verbose restore, backup failure, idempotence, concurrent change and failed write'
