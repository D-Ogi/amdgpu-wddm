function ConvertTo-KmdNativeArgument {
 param([AllowEmptyString()][string]$Value)
 $b=New-Object Text.StringBuilder
 [void]$b.Append('"');$slashes=0
 foreach($c in $Value.ToCharArray()){
  if($c -eq '\'){$slashes++;continue}
  $count=if($c -eq '"'){$slashes*2}else{$slashes}
  if($count){[void]$b.Append(('\'*$count))};$slashes=0
  if($c -eq '"'){[void]$b.Append('\')}
  [void]$b.Append($c)
 }
 if($slashes){[void]$b.Append(('\'*($slashes*2)))}
 [void]$b.Append('"');return $b.ToString()
}
function Invoke-KmdBoundedChild {
 param([string]$Tool,[long]$Deadline,[string]$Stdout,[string]$Stderr,[string]$Executable,[string[]]$Arguments,[switch]$ActiveConsole,[string]$CancelFile,[scriptblock]$Monitor)
 if($Monitor -and !$CancelFile){throw 'Monitor requires a cancellation path'}
 $monitorError=$null
 $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())/[double][Diagnostics.Stopwatch]::Frequency
 if($remaining -le 1){throw 'Insufficient remaining child budget'}
 $values=@([string]$Deadline,$Stdout,$Stderr,$Executable)+$Arguments
 if($ActiveConsole){$values=@("--active-console")+$values}
 if($CancelFile){$values=@("--cancel-file",$CancelFile)+$values}
 $si=New-Object Diagnostics.ProcessStartInfo
 $si.FileName=$Tool;$si.UseShellExecute=$false;$si.CreateNoWindow=$true
 $si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
 $si.Arguments=($values|ForEach-Object {ConvertTo-KmdNativeArgument $_}) -join ' '
 $process=New-Object Diagnostics.Process;$process.StartInfo=$si
 try{
  if(!$process.Start()){throw 'Bounded helper did not start'}
  $handle=$process.Handle
  # Drain both pipes concurrently; EOF itself is bounded, not just process exit.
  $stdoutTask=$process.StandardOutput.ReadToEndAsync()
  $stderrTask=$process.StandardError.ReadToEndAsync()
  $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())/[double][Diagnostics.Stopwatch]::Frequency
  $wait=[int][math]::Max(0,[math]::Floor($remaining*1000))
  if($Monitor){
   $nextPoll=0L
   while(!$process.WaitForExit([int][math]::Min(100,$wait))){
    if([Diagnostics.Stopwatch]::GetTimestamp() -ge $Deadline){
     $process.Kill();throw 'Helper deadline exceeded; process-tree closure unconfirmed, recovery required'
    }
    if(!$monitorError -and [Diagnostics.Stopwatch]::GetTimestamp() -ge $nextPoll){
     try{& $Monitor | Out-Null}catch{
      $monitorError=$_.Exception.Message
      # Request orderly Job termination and still collect the empty-job receipt.
      [IO.File]::WriteAllText($CancelFile,'monitor cancelled')
     }
     $nextPoll=[Diagnostics.Stopwatch]::GetTimestamp()+[long](2*[Diagnostics.Stopwatch]::Frequency)
    }
    $wait=[int][math]::Max(0,[math]::Floor(($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())*1000/[double][Diagnostics.Stopwatch]::Frequency))
   }
  }elseif(!$process.WaitForExit($wait)){
   $process.Kill()
   throw 'Helper deadline exceeded; process-tree closure unconfirmed, recovery required'
  }
  $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())/[double][Diagnostics.Stopwatch]::Frequency
  $wait=[int][math]::Max(0,[math]::Floor($remaining*1000))
  $reads=[Threading.Tasks.Task[]]@($stdoutTask,$stderrTask)
  if(![Threading.Tasks.Task]::WaitAll($reads,$wait)){
   throw 'Receipt pipe deadline exceeded; process-tree closure unconfirmed'
  }
  return @{exit_code=$process.ExitCode;stdout=$stdoutTask.Result;stderr=$stderrTask.Result;monitor_error=$monitorError}
 }finally{$process.Dispose()}
}
