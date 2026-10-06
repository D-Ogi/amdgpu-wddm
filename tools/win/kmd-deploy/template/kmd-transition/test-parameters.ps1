$ErrorActionPreference='Stop'
. "$PSScriptRoot\parameters.ps1"
. "$PSScriptRoot\hang-detector.ps1"
# The capture as postflight reads it: baseline.json through ConvertFrom-Json.
function Saved([hashtable]$Values){$o=[ordered]@{};foreach($k in ($Values.Keys|Sort-Object)){$o[$k]=[ordered]@{value=$Values[$k];kind='DWord'}};$o|ConvertTo-Json -Depth 4|ConvertFrom-Json}
function Live([hashtable]$Values){$o=[ordered]@{};foreach($k in ($Values.Keys|Sort-Object)){$o[$k]=@{value=$Values[$k];kind='DWord'}};$o}
$skip=@(Get-KmdHangDetectorNames)
$base=@{EnableMmio=1;EnableGfx=1;CuMode=24;DpmMode=0;UnconfirmedStarts=0;LastStage=61;CuModeLastApplied=24;CuModeLastReason=6;DpmLastMode=0;DpmLastReason=0}

# CuModeLastReason captured 6 (cold-boot refusal), 0 after the candidate's start. Reported, no failure.
$live=$base.Clone();$live.CuModeLastReason=0;$live.LastStage=62
$c=Compare-KmdCapturedParameters -Saved (Saved $base) -Live (Live $live) -Skip $skip
if($c.differs.Count){throw "telemetry counted as a difference: $($c.differs -join ',')"}
$reason=@($c.kmd_written|Where-Object {$_.name -eq 'CuModeLastReason'})
if($reason.Count -ne 1 -or !$reason[0].changed -or $reason[0].captured -ne 6 -or $reason[0].live -ne 0){throw 'CuModeLastReason not reported'}
if(@($c.kmd_written|Where-Object {$_.name -eq 'CuModeLastApplied'})[0].changed){throw 'unchanged value reported as changed'}

# Operator values still fail: changed, missing, and next to telemetry changes.
$live=$base.Clone();$live.CuMode=40
$c=Compare-KmdCapturedParameters -Saved (Saved $base) -Live (Live $live) -Skip $skip
if(($c.differs -join ',') -ne 'CuMode'){throw 'changed operator value not caught'}
$live=$base.Clone();$live.Remove('EnableGfx');$live.DpmLastReason=3
$c=Compare-KmdCapturedParameters -Saved (Saved $base) -Live (Live $live) -Skip $skip
if(($c.differs -join ',') -ne 'EnableGfx'){throw 'missing operator value not caught'}
$live=$base.Clone();$live.DpmMode=1;$live.EnableMmio=0
$c=Compare-KmdCapturedParameters -Saved (Saved $base) -Live (Live $live) -Skip $skip
if(($c.differs|Sort-Object) -join ',' -ne 'DpmMode,EnableMmio'){throw 'two changes not both caught'}

# A KMD-written value that appears or disappears is reported; a new non-KMD value is listed as added.
$live=$base.Clone();$live.Remove('DpmLastMode');$live.DpmSession=0x31;$live.KeepLog=1
$c=Compare-KmdCapturedParameters -Saved (Saved $base) -Live (Live $live) -Skip $skip
if($c.differs.Count){throw 'KMD-written presence change counted as a difference'}
if(!@($c.kmd_written|Where-Object {$_.name -eq 'DpmLastMode' -and $_.changed -and $null -eq $_.live}).Count){throw 'vanished telemetry not reported'}
if(!@($c.kmd_written|Where-Object {$_.name -eq 'DpmSession' -and $_.changed -and $null -eq $_.captured}).Count){throw 'new telemetry not reported'}
if((@($c.added|ForEach-Object {$_.name}) -join ',') -ne 'KeepLog'){throw 'added value not listed'}

# The hang detector's values have their own check and are skipped either way.
$det=$skip[0]
$saved=$base.Clone();$saved[$det]=1;$live=$base.Clone()
$c=Compare-KmdCapturedParameters -Saved (Saved $saved) -Live (Live $live) -Skip $skip
if($c.differs.Count -or @($c.added).Count){throw 'hang detector value compared'}

# Values of several kinds: a multi-string equal and changed, a string compared as before.
$s=[ordered]@{List=[ordered]@{value=@('a','b');kind='MultiString'};Name=[ordered]@{value='x';kind='String'}}|ConvertTo-Json -Depth 4|ConvertFrom-Json
$c=Compare-KmdCapturedParameters -Saved $s -Live ([ordered]@{List=@{value=[string[]]@('a','b');kind='MultiString'};Name=@{value='x';kind='String'}})
if($c.differs.Count){throw 'equal multi-string reported'}
$c=Compare-KmdCapturedParameters -Saved $s -Live ([ordered]@{List=@{value=[string[]]@('a','c');kind='MultiString'};Name=@{value='x';kind='String'}})
if(($c.differs -join ',') -ne 'List'){throw 'changed multi-string missed'}

# The names match what the KMD writes: no operator setting is in the list.
foreach($n in @('CuMode','CuDisableWgp','CuModeConfirmed','CuModePending','DpmMode','DpmMaxMHz','DpmConfirmed','DpmPending','KeepLog','EnableGfx')){
 if($n -in (Get-KmdWrittenParameterNames)){throw "operator/guard value $n treated as telemetry"}
}
'PASS: KMD-written values reported not compared (captured-6-live-0 CuModeLastReason case), operator values changed/missing caught, appear/vanish reported, added listed, detector skipped, multi-string, name list'
