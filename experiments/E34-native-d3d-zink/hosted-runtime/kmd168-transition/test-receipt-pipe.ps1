param([Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-bounded.ps1"
if(Test-Path $Out){throw 'Use a fresh directory'}
[void](New-Item -ItemType Directory $Out)
$pidFile=Join-Path $Out 'holder.pid'
$watch=[Diagnostics.Stopwatch]::StartNew()
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long](2*[Diagnostics.Stopwatch]::Frequency)
$errorText=''
try {
 try {Invoke-KmdBoundedChild -Tool $Fixture -Deadline $deadline -Stdout $pidFile -Stderr "$Out\unused.err" -Executable 'unused' -Arguments @()|Out-Null}
 catch {$errorText=[string]$_}
 $elapsed=$watch.Elapsed.TotalSeconds
 if($errorText -notmatch 'Receipt pipe deadline exceeded'){throw "Expected bounded pipe failure, got: $errorText"}
 if($elapsed -gt 3){throw "Pipe read exceeded deadline tolerance: $elapsed"}
 $holderId=[int](Get-Content $pidFile -Raw)
 $holder=Get-Process -Id $holderId -ErrorAction Stop
 if($holder.HasExited -or $holder.Path -ine [IO.Path]::GetFullPath($Fixture)){throw 'Surviving pipe holder not witnessed'}
 @{seconds=$elapsed;error=$errorText;holder_alive=$true}|ConvertTo-Json|Set-Content "$Out\result.json"
} finally {
 if(Test-Path $pidFile){
  $holder=Get-Process -Id ([int](Get-Content $pidFile -Raw)) -ErrorAction SilentlyContinue
  if($holder -and !$holder.HasExited -and $holder.Path -ieq [IO.Path]::GetFullPath($Fixture)){$holder.Kill();if(!$holder.WaitForExit(2000)){throw 'Fixture cleanup incomplete'}}
 }
}
'PASS: inherited pipe holder survives helper, bounded reader returns, fixture cleaned'
