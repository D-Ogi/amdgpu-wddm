$ErrorActionPreference='Stop'
. "$PSScriptRoot\hang-detector.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
# Registry key double: the five RegistryKey methods these scripts call, case-insensitive names like the registry.
function New-FakeKey([hashtable]$Values){
 $store=[ordered]@{};foreach($k in $Values.Keys){$store[$k]=$Values[$k]}
 $key=[pscustomobject]@{Store=$store;Writes=0;IgnoreWrites=$false}
 $key|Add-Member ScriptMethod GetValueNames {@($this.Store.Keys)}
 $key|Add-Member ScriptMethod GetValueKind {param($n) [Microsoft.Win32.RegistryValueKind]$this.Store[$n].kind}
 $key|Add-Member ScriptMethod GetValue {param($n,$d,$o) ,$this.Store[$n].value}
 $key|Add-Member ScriptMethod SetValue {param($n,$v,$k) $this.Writes++;if(!$this.IgnoreWrites){$this.Store[$n]=@{kind=[string]$k;value=$v}}}
 $key|Add-Member ScriptMethod DeleteValue {param($n,$t) $this.Writes++;if(!$this.IgnoreWrites){$this.Store.Remove($n)}}
 $key|Add-Member ScriptMethod Flush {}
 return $key
}
function RoundTrip($State){$State|ConvertTo-Json -Depth 6|ConvertFrom-Json}
$other=@{EnableMmio=@{kind='DWord';value=1}}

# Absent baseline: candidate starts with an explicit 0, Cleanup restores absence, a second Cleanup changes nothing.
$key=New-FakeKey $other
$baseline=RoundTrip (Read-KmdHangDetector $key)
Assert-KmdHangDetectorBaseline $baseline
if($baseline.EnableHangBugcheck.present -or $baseline.HangBugcheckSeconds.present){throw 'Absence not captured'}
$candidate=Get-KmdCandidateHangDetector $baseline
$r=Set-KmdHangDetector -Key $key -Desired $candidate
if(!$key.Store.Contains('EnableHangBugcheck') -or $key.Store['EnableHangBugcheck'].value -ne 0 -or $key.Store.Contains('HangBugcheckSeconds')){throw 'Candidate state not written'}
Assert-KmdHangDetectorClosed (Read-KmdHangDetector $key)
$final=Restore-KmdHangDetectorFinal -Key $key -Baseline $baseline
if(!$final.changed -or $key.Store.Contains('EnableHangBugcheck') -or $key.Store.Contains('HangBugcheckSeconds') -or !$key.Store.Contains('EnableMmio')){throw 'Absence not restored'}
$again=Restore-KmdHangDetectorFinal -Key $key -Baseline $baseline
if($again.changed){throw 'Idempotent restore wrote'}

# The 174 install writes EnableHangBugcheck=0; restoring a baseline without it removes that value.
$key=New-FakeKey $other
$baseline=RoundTrip (Read-KmdHangDetector $key)
$key.Store['EnableHangBugcheck']=@{kind='DWord';value=0}
[void](Set-KmdHangDetector -Key $key -Desired $baseline)
if($key.Store.Contains('EnableHangBugcheck')){throw 'Install-introduced value survived rollback'}

# Present closed baseline with a limit: kept exactly; the candidate keeps the captured limit.
$key=New-FakeKey @{EnableHangBugcheck=@{kind='DWord';value=0};HangBugcheckSeconds=@{kind='DWord';value=20}}
$baseline=RoundTrip (Read-KmdHangDetector $key)
$candidate=Get-KmdCandidateHangDetector $baseline
if($candidate.HangBugcheckSeconds.value -ne 20 -or !(Test-KmdHangDetectorEqual $candidate $baseline)){throw 'Captured limit lost'}
if((Restore-KmdHangDetectorFinal -Key $key -Baseline $baseline).changed){throw 'Unchanged state rewritten'}

# Refusals: armed or non-DWORD baseline, malformed receipt, third-party change, failed readback.
Must-Reject {Assert-KmdHangDetectorBaseline (Read-KmdHangDetector (New-FakeKey @{EnableHangBugcheck=@{kind='DWord';value=1}}))}
Must-Reject {Assert-KmdHangDetectorBaseline (Read-KmdHangDetector (New-FakeKey @{EnableHangBugcheck=@{kind='String';value='0'}}))}
Must-Reject {Assert-KmdHangDetectorBaseline (Read-KmdHangDetector (New-FakeKey @{HangBugcheckSeconds=@{kind='String';value='10'}}))}
Must-Reject {ConvertTo-KmdHangDetectorState @{EnableHangBugcheck=@{present='false'};HangBugcheckSeconds=@{present=$false}}}
Must-Reject {ConvertTo-KmdHangDetectorState @{EnableHangBugcheck=@{present=$false}}}
Must-Reject {ConvertTo-KmdHangDetectorState $null}
Must-Reject {Assert-KmdHangDetectorClosed @{EnableHangBugcheck=@{present=$true;kind='DWord';value=1};HangBugcheckSeconds=@{present=$false}}}
$key=New-FakeKey @{EnableHangBugcheck=@{kind='DWord';value=1}}
$baseline=RoundTrip @{EnableHangBugcheck=@{present=$false;kind=$null;value=$null};HangBugcheckSeconds=@{present=$false;kind=$null;value=$null}}
Must-Reject {Restore-KmdHangDetectorFinal -Key $key -Baseline $baseline}
if($key.Writes -or $key.Store['EnableHangBugcheck'].value -ne 1){throw 'Third-party value overwritten'}
$key=New-FakeKey $other;$key.IgnoreWrites=$true
Must-Reject {Set-KmdHangDetector -Key $key -Desired (Get-KmdCandidateHangDetector $baseline)}
'PASS: absence captured and restored, install-added 0 removed, captured limit kept, idempotent cleanup, 8 refusals'
