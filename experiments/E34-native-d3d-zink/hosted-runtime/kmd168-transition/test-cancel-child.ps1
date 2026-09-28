param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
if(Test-Path $Out){throw 'Fresh test directory required'}
$null=New-Item -ItemType Directory $Out
$fixture=Join-Path $Out 'cancel fixture.ps1'
Copy-Item "$PSScriptRoot\cancel-child-fixture.ps1" $fixture
$results=@()
foreach($mode in @('before','tree')){
 $prefix=Join-Path $Out $mode
 if($mode -eq 'before'){[IO.File]::WriteAllText($prefix+'.cancel','cancel')}
 $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long](6*[Diagnostics.Stopwatch]::Frequency)
 $clock=[Diagnostics.Stopwatch]::StartNew()
 $text=& $Exe --cancel-file "$prefix.cancel" $deadline "$prefix.out" "$prefix.err" "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -File $fixture -Prefix $prefix|Out-String
 $exit=$LASTEXITCODE;$receipt=$text|ConvertFrom-Json
 if($exit -ne 123 -or !$receipt.cancelled -or !$receipt.job_empty){throw "Bad cancel receipt $text"}
 if($clock.Elapsed.TotalSeconds -gt 6){throw 'Cancellation exceeded deadline'}
 if($mode -eq 'before' -and (Test-Path "$prefix.entered")){throw 'Precancelled child ran user code'}
 if($mode -eq 'tree'){
  $saved=Get-Content "$prefix.child.json" -Raw|ConvertFrom-Json
  $live=Get-Process -Id $saved.pid -ErrorAction SilentlyContinue
  if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $saved.start){throw 'Cancelled descendant survived'}
 }
 $results+=@{case=$mode;receipt=$receipt;seconds=$clock.Elapsed.TotalSeconds}
}
$results|ConvertTo-Json -Depth 6|Set-Content "$Out\results.json"
'PASS cancellation before resume and running descendant tree'
