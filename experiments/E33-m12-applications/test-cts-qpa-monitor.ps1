param([string]$RecordedRun)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\cts-qpa-monitor.ps1"
function Expect-Failure([scriptblock]$Action,[string]$Label){
 $caught=$false
 try{& $Action | Out-Null}catch{$caught=$true}
 if(-not $caught){throw "Expected failure: $Label"}
}
function Record([string]$Name,[string]$Status){
 return "#beginTestCaseResult $Name`n<TestCaseResult>`n <Result StatusCode=`"$Status`">detail</Result>`n</TestCaseResult>`n#endTestCaseResult`n"
}
$a='dEQP-VK.control.a';$b='dEQP-VK.control.b'
$text=(Record $a Pass)+(Record $b NotSupported)
foreach($size in @(1,7,31,4096)){
 $s=New-CtsQpaMonitor @($a,$b) 0
 for($i=0;$i -lt $text.Length;$i+=$size){Add-CtsQpaText $s $text.Substring($i,[Math]::Min($size,$text.Length-$i)) 1 | Out-Null}
 Assert-CtsQpaComplete $s
 if($s.Finished -ne 2 -or $s.Counts.Pass -ne 1 -or $s.Counts.NotSupported -ne 1){throw 'Chunked count mismatch'}
}
$s=New-CtsQpaMonitor @($a,$b) 0
Add-CtsQpaText $s (Record $a Pass) 1 | Out-Null
Expect-Failure {Assert-CtsQpaComplete $s} 'missing case'
Expect-Failure {Add-CtsQpaText $s (Record $a Pass) 2} 'duplicate result'
$s=New-CtsQpaMonitor @($a) 0
Add-CtsQpaText $s "#beginTestCaseResult $a`n" 100 | Out-Null
Add-CtsQpaText $s "<Text>Still printing shaders</Text>`n" 45000 | Out-Null
Expect-Failure {Assert-CtsQpaDeadline $s 45100} 'output must not reset case deadline'
Expect-Failure {Assert-CtsQpaComplete $s} 'unfinished case'
$s=New-CtsQpaMonitor @($a) 0
Expect-Failure {Add-CtsQpaText $s (Record $a Fail) 1} 'failure stops run'
$s=New-CtsQpaMonitor @($a) 0
Expect-Failure {Add-CtsQpaText $s "#beginTestCaseResult $a`n#terminateTestCaseResult Timeout`n" 1} 'termination'
$s=New-CtsQpaMonitor @($a) 0
Expect-Failure {Add-CtsQpaText $s (Record $b Pass) 1} 'unexpected case'
$s=New-CtsQpaMonitor @($a) 0
Add-CtsQpaText $s '#beginTestCaseRes' 1 | Out-Null
Expect-Failure {Assert-CtsQpaComplete $s} 'truncated marker'
if($RecordedRun){
 $cases=[IO.File]::ReadAllLines((Join-Path $RecordedRun 'cases.txt'))
 $s=New-CtsQpaMonitor $cases 0
 foreach($file in (Get-ChildItem $RecordedRun -Filter '*.qpa' | Sort-Object Name)){
  $text=[IO.File]::ReadAllText($file.FullName)
  for($i=0;$i -lt $text.Length;$i+=4093){Add-CtsQpaText $s $text.Substring($i,[Math]::Min(4093,$text.Length-$i)) 1 | Out-Null}
 }
 Assert-CtsQpaComplete $s
 $expected=@(Get-Content (Join-Path $RecordedRun 'cases.jsonl') | ForEach-Object {$_ | ConvertFrom-Json})
 foreach($status in @('Pass','NotSupported')){
  if($s.Counts[$status] -ne @($expected | Where-Object status -eq $status).Count){throw 'Recorded status mismatch'}
 }
 Write-Output "Recorded QPA replay: $($s.Finished) results match"
}
Write-Output 'QPA monitor controls passed; no GPU process started'
