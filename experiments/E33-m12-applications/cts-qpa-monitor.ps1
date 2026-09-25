# Incremental QPA state used by the prospective batch runner. No process launch here.
function New-CtsQpaMonitor([string[]]$Cases,[long]$NowMs=0) {
 $expected=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
 foreach($case in $Cases){if(-not $expected.Add($case)){throw "Duplicate expected case: $case"}}
 if(-not $expected.Count){throw 'Empty CTS batch'}
 return @{Expected=$expected;Remaining=[Collections.Generic.HashSet[string]]::new($expected,[StringComparer]::Ordinal);Tail='';Case=$null;Status=$null;LastTransitionMs=$NowMs;Finished=0;Counts=@{Pass=0;NotSupported=0}}
}
function Add-CtsQpaText($State,[string]$Text,[long]$NowMs) {
 $text=$State.Tail+$Text
 $last=$text.LastIndexOf("`n")
 if($last -lt 0){$State.Tail=$text;return}
 $State.Tail=$text.Substring($last+1)
 foreach($raw in $text.Substring(0,$last).Split("`n")){
  $line=$raw.TrimEnd("`r")
  if($line -match '^#beginTestCaseResult (\S+)$'){
   $name=$Matches[1]
   if($State.Case){throw "Nested case start: $name"}
   if(-not $State.Remaining.Contains($name)){throw "Unexpected or repeated case: $name"}
   $State.Case=$name;$State.Status=$null;$State.LastTransitionMs=$NowMs
   @{event='begin';case=$name;observed_ms=$NowMs}
  }elseif($line -match '^\s*<Result StatusCode="([^"]+)">'){
   if(-not $State.Case -or $State.Status){throw 'Unpaired or multiple QPA result'}
   $State.Status=$Matches[1]
   if($State.Status -notin @('Pass','NotSupported')){throw "CTS $($State.Status): $($State.Case)"}
  }elseif($line -eq '#endTestCaseResult'){
   if(-not $State.Case -or -not $State.Status){throw 'Case ended without result'}
   $null=$State.Remaining.Remove($State.Case)
   $State.Finished++;$State.Counts[$State.Status]++;$State.LastTransitionMs=$NowMs
   @{event='end';case=$State.Case;status=$State.Status;observed_ms=$NowMs}
   $State.Case=$null;$State.Status=$null
  }elseif($line -match '^#terminateTestCaseResult'){
   throw "CTS terminated: $($State.Case): $line"
  }
 }
}
function Assert-CtsQpaDeadline($State,[long]$NowMs,[long]$LimitMs=45000) {
 # Covers startup, active case and inter-case/shutdown stalls, not just output silence.
 # Shader/image text does not reset this clock.
 if($NowMs-$State.LastTransitionMs -ge $LimitMs){throw "CTS transition timeout: $($State.Case)"}
}
function Assert-CtsQpaComplete($State) {
 if($State.Case -or $State.Tail.Trim() -or $State.Remaining.Count){throw 'Incomplete CTS batch'}
}
