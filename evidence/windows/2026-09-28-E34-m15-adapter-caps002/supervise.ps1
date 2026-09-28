$ErrorActionPreference='Stop'
$d=$PSScriptRoot
if(Test-Path "$d\started"){throw 'Existing attempt; inspect only'}
[IO.File]::WriteAllText("$d\started",[DateTime]::UtcNow.ToString('o'))
. "$d\invoke-bounded.ps1"
$clock=[Diagnostics.Stopwatch]::StartNew()
$absolute=[Diagnostics.Stopwatch]::GetTimestamp()+[long](170*[Diagnostics.Stopwatch]::Frequency)
$exe="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
function Step($name,$budget,$file,$extra=@()){
 $deadline=[math]::Min($absolute,[Diagnostics.Stopwatch]::GetTimestamp()+[long]($budget*[Diagnostics.Stopwatch]::Frequency))
 $receipt=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline $deadline -Stdout "$d\$name-out.txt" -Stderr "$d\$name-err.txt" -Executable $exe -Arguments (@('-NoProfile','-ExecutionPolicy','Bypass','-File',$file)+$extra)
 $receipt|ConvertTo-Json -Depth 6|Set-Content "$d\$name-helper.json"
 $child=$receipt.stdout|ConvertFrom-Json
 if($receipt.exit_code -ne 0 -or !$child.job_empty -or !$child.root_exit_observed -or $child.child_exit -ne 0 -or $child.timed_out){throw "$name failed or tree closure not verified"}
}
$result=@{passed=$false;error=$null;postflight=$false}
try{
 Step 'preflight' 15 "$d\preflight171.ps1"
 Step 'engine' 30 "$d\run.ps1"
 $result.passed=$true
}catch{$result.error=$_.Exception.Message}
try{
 Step 'postflight' 15 "$d\preflight171.ps1"
 $a=Get-Content "$d\preflight-out.txt" -Raw|ConvertFrom-Json
 $b=Get-Content "$d\postflight-out.txt" -Raw|ConvertFrom-Json
 if($a.boot -ne $b.boot -or $a.confirmed.generation -ne $b.confirmed.generation -or $a.confirmed.epoch -ne $b.confirmed.epoch -or ($a.dwm|ConvertTo-Json -Depth 5 -Compress) -cne ($b.dwm|ConvertTo-Json -Depth 5 -Compress)){throw 'Baseline identity changed'}
 if(($a.umd_registration|ConvertTo-Json -Compress) -cne ($b.umd_registration|ConvertTo-Json -Compress) -or ($a.icd_registration|ConvertTo-Json -Compress) -cne ($b.icd_registration|ConvertTo-Json -Compress)){throw 'Adapter registration changed'}
 $result.postflight=$true
}catch{$result.passed=$false;$result.error=[string]$result.error+' postflight: '+$_.Exception.Message}
$result.elapsed=$clock.Elapsed.TotalSeconds
$result|ConvertTo-Json|Set-Content "$d\supervisor.json"
$result|ConvertTo-Json
if(!$result.passed){exit 1}
