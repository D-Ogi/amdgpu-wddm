param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\durable.ps1"
if(Test-Path -LiteralPath $Out){throw 'Existing fault-control evidence'}
[void][IO.Directory]::CreateDirectory($Out)
$results=@()
# Exercise the real durable restoration helper after each DLL mutation boundary.
# No task, process, driver, registry or lab operation is performed here.
foreach($stop in 0..4){
 $d=Join-Path $Out ('step'+$stop);[void][IO.Directory]::CreateDirectory($d)
 foreach($name in 'umd','icd'){
  [IO.File]::WriteAllText("$d\$name.active",'baseline-'+$name)
  [IO.File]::WriteAllText("$d\$name.candidate",'candidate-'+$name)
  $hash=(Get-FileHash "$d\$name.active").Hash
  Copy-VerifiedDurable "$d\$name.active" "$d\$name.backup" $hash
 }
 if($stop -ge 1){Move-Item -LiteralPath "$d\umd.active" -Destination "$d\umd.original"}
 if($stop -ge 2){Copy-VerifiedDurable "$d\umd.candidate" "$d\umd.active" (Get-FileHash "$d\umd.candidate").Hash}
 if($stop -ge 3){Move-Item -LiteralPath "$d\icd.active" -Destination "$d\icd.original"}
 if($stop -ge 4){Copy-VerifiedDurable "$d\icd.candidate" "$d\icd.active" (Get-FileHash "$d\icd.candidate").Hash}
 foreach($name in 'umd','icd'){
  $baseline=(Get-FileHash "$d\$name.backup").Hash
  $candidate=(Get-FileHash "$d\$name.candidate").Hash
  Restore-DurableBaseline "$d\$name.active" "$d\$name.backup" "$d\$name.original" $baseline $candidate
  if((Get-FileHash "$d\$name.active").Hash -ne $baseline){throw 'Interrupted install failed restoration'}
 }
 $results+=@{stop_after_mutation=$stop;both_baselines_restored=$true}
}
@{cases=$results;lab_used=$false;scope='DLL file mutation boundaries only; no hardware or OS scheduling fault injection'}|ConvertTo-Json -Depth 5
