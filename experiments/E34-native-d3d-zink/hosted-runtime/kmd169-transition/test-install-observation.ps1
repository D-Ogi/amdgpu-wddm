$ErrorActionPreference='Stop'
. "$PSScriptRoot\install-observation.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$missing=Read-KmdInstallField {throw 'Binding removed'}
if($missing.readable -or $null -ne $missing.value -or !$missing.error){throw 'Unreadable field misrepresented'}
$zero=Read-KmdInstallField {0}
if(!$zero.readable -or $zero.value -ne 0){throw 'Valid problem0 lost'}
foreach($mode in @('success','nonzero','exception','running')){
 $script:events=New-Object Collections.Generic.List[string]
 $script:reads=0
 $read={
  $script:reads++
  $script:events.Add("read$script:reads")
  @{fields=@{service=@{readable=$true;value=@{state=$(if($mode -eq 'running'){'Running'}else{'Stopped'})}};version=$(if($script:reads -eq 1){'166'}else{$null})}}
 }
 $save={param($stage,$value);$script:events.Add("save-$stage");if($stage -eq 'after' -and $null -ne $value.observation.fields.version){throw 'Post-install binding loss hidden'}}
 $install={$script:events.Add('install');if($mode -eq 'exception'){throw 'Native launch failed'};if($mode -eq 'nonzero'){return 1};return 0}
 $action={Invoke-KmdObservedInstall -Read $read -Save $save -Install $install}
 if($mode -eq 'success'){& $action|Out-Null}else{Must-Reject $action}
 $expected=if($mode -eq 'running'){'read1,save-before'}else{'read1,save-before,install,read2,save-after'}
 if(($script:events -join ',') -ne $expected){throw "Incorrect observation order: $mode"}
}
'PASS: missing binding, native error/exception, stopped-service admission and before/after ordering'
