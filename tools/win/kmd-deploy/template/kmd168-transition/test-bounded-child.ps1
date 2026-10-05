param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-bounded.ps1"
if(Test-Path $Out){throw 'Use a fresh test directory'}
New-Item -ItemType Directory $Out|Out-Null
$fixture=Join-Path $Out 'child fixture.ps1'
Copy-Item (Join-Path $PSScriptRoot 'bounded-child-fixture.ps1') $fixture
$results=@()
foreach($mode in @('ok','fail','hang','orphan','tree')){
 $seconds=if($mode -in @('hang','tree')){3}else{6}
 $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long]($seconds*[Diagnostics.Stopwatch]::Frequency)
 $pidFile=Join-Path $Out "$mode-pid.json"
 $value='spaces "quoted" trailing\'
 $watch=[Diagnostics.Stopwatch]::StartNew()
 $result=Invoke-KmdBoundedChild -Tool $Exe -Deadline $deadline -Stdout (Join-Path $Out "$mode.out") -Stderr (Join-Path $Out "$mode.err") -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',$fixture,'-Mode',$mode,'-PidFile',$pidFile,'-Value',$value)
 $code=$result.exit_code;$text=$result.stdout;$receipt=$text|ConvertFrom-Json
 $expected=if($mode -eq 'fail'){126}elseif($mode -in @('hang','tree')){124}else{0}
 if($code -ne $expected -or !$receipt.job_empty){throw "$mode incorrect result: $text / $code"}
 if($watch.Elapsed.TotalSeconds -gt $seconds+0.5){throw "$mode exceeded total budget"}
 if($mode -eq 'ok' -and (Get-Content (Join-Path $Out "$mode.out") -Raw).Trim() -ne $value){throw 'Argument quoting failed'}
 if($mode -in @('orphan','tree')){
  $id=Get-Content $pidFile -Raw|ConvertFrom-Json
  $live=Get-Process -Id $id.pid -ErrorAction SilentlyContinue
  if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $id.start){throw 'Descendant survived job termination'}
 }
 $results+=@{case=$mode;exit=$code;seconds=$watch.Elapsed.TotalSeconds;receipt=$receipt}
}
$results|ConvertTo-Json -Depth 6|Set-Content (Join-Path $Out 'results.json')
'PASS: success/quoting, child error, timeout, orphan cleanup, timed-out tree'
