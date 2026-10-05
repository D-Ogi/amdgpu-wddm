param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Out,[switch]$ExpectDenied)
$ErrorActionPreference='Stop'
if(Test-Path $Out){throw 'Fresh directory required'}
$null=New-Item -ItemType Directory $Out
$fixture=Join-Path $Out 'console fixture.ps1'
Copy-Item "$PSScriptRoot\active-console-fixture.ps1" $fixture
$results=@()
foreach($mode in $(if($ExpectDenied){@('denied')}else{@('ok','fail','orphan','tree')})){
 $seconds=if($mode -eq 'tree'){4}else{8}
 $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long]($seconds*[Diagnostics.Stopwatch]::Frequency)
 $prefix=Join-Path $Out $mode
 $clock=[Diagnostics.Stopwatch]::StartNew()
 $text=& $Exe --active-console $deadline "$prefix.out" "$prefix.err" "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $fixture -Mode $mode -Prefix $prefix | Out-String
 $exit=$LASTEXITCODE;$receipt=$text|ConvertFrom-Json
 $elapsed=$clock.Elapsed.TotalSeconds
 if($ExpectDenied){
  if($exit -ne 125 -or $receipt.launch_error -ne 5 -or (Test-Path "$prefix-root.json")){throw 'Non-SYSTEM console launch was not refused'}
 }else{
  $expected=if($mode -eq 'fail'){126}elseif($mode -eq 'tree'){124}else{0}
  if($exit -ne $expected -or $receipt.job_empty -ne $true -or $receipt.console_session -le 0){throw "Bad console receipt $text"}
  $root=Get-Content "$prefix-root.json" -Raw|ConvertFrom-Json
  if($root.session -ne $receipt.console_session -or $root.pid -ne $receipt.child_pid){throw 'Wrong interactive process/session'}
  foreach($path in @("$prefix-root.json","$prefix-child.json")){
   if(Test-Path $path){
    $saved=Get-Content $path -Raw|ConvertFrom-Json
    $live=Get-Process -Id $saved.pid -ErrorAction SilentlyContinue
    if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $saved.start){throw 'Process survived job closure'}
   }
  }
 }
 if($elapsed -gt $seconds+1){throw 'Deadline exceeded'}
 $results+=@{case=$mode;seconds=$elapsed;exit=$exit;receipt=$receipt}
}
$results|ConvertTo-Json -Depth 6|Set-Content "$Out\results.json"
'PASS active-console launch policy and process tree closure'
