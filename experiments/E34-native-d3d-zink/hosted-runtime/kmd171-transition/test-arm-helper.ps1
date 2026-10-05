param([Parameter(Mandatory)][string]$Tool,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\run-arm.ps1"
if(Test-Path $Out){throw 'Use a fresh output directory'}
[void](New-Item -ItemType Directory $Out)
$fixture=[IO.Path]::GetFullPath("$PSScriptRoot\..\kmd168-transition\bounded-child-fixture.ps1")
$results=@()
foreach($mode in @('ok','fail','tree')){
 $seconds=if($mode -eq 'tree'){3}else{6}
 $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long]($seconds*[Diagnostics.Stopwatch]::Frequency)
 $result=Invoke-KmdBoundedChild -Tool $Tool -Deadline $deadline -Stdout "$Out\$mode.out" -Stderr "$Out\$mode.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',$fixture,'-Mode',$mode,'-PidFile',"$Out\$mode-pid.json",'-Value','closure-control')
 $closure=Get-KmdChildClosure $result
 $expected=if($mode -eq 'ok'){'success'}else{'failed-closed'}
 if($closure -ne $expected){throw "Unexpected closure for $mode : $closure"}
 if($mode -eq 'tree'){
  $identity=Get-Content "$Out\$mode-pid.json" -Raw|ConvertFrom-Json
  $live=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
  if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Child remains alive'}
 }
 $results+=@{mode=$mode;closure=$closure;helper=$result}
}
Write-DurableText "$Out\results.json" ($results|ConvertTo-Json -Depth 8)
'PASS: actual helper success, failure and timed-out descendant closure'
