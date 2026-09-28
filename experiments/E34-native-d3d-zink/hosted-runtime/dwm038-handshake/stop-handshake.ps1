$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted038'
$task=Get-ScheduledTask -TaskName BC250-G0-Handshake038 -ErrorAction SilentlyContinue
if($task){
 Stop-ScheduledTask -TaskName $task.TaskName
 for($i=0;$i -lt 50 -and (Get-ScheduledTask -TaskName $task.TaskName).State -eq 'Running';$i++){Start-Sleep -Milliseconds 100}
 if((Get-ScheduledTask -TaskName $task.TaskName).State -eq 'Running'){throw 'Handshake controller still running'}
}
foreach($case in 'no-paint','gdi-paint'){
 $expected="$d\$case\redirblt-probe.exe"
 foreach($process in @(Get-Process redirblt-probe -ErrorAction SilentlyContinue)){
  # Hold this process object/handle before comparing its executable path.
  $handle=$process.Handle
  if(!$process.HasExited -and $process.Path -eq $expected){
   $process.Kill()
   if(!$process.WaitForExit(5000)){throw 'Original handshake probe did not terminate'}
  }
 }
}
