# One runner owns one marker file. This helper does not imply GPU completion.
function Request-AuditCheckpoint {
 [CmdletBinding()]
 param(
  [Parameter(Mandatory=$true)][string]$MarkerPath,
  [Parameter(Mandatory=$true)][string]$LogPath,
  [Parameter(Mandatory=$true)][UInt64]$Marker,
  [Parameter(Mandatory=$true)][int]$ProcessId,
  [Parameter(Mandatory=$true)][datetime]$ProcessStartUtc,
  [Parameter(Mandatory=$true)][Diagnostics.Stopwatch]$TrialClock,
  [Parameter(Mandatory=$true)][double]$DeadlineSeconds,
  [int]$TimeoutMilliseconds=4000
 )
 if($Marker -eq 0 -or $TimeoutMilliseconds -le 0){throw 'Invalid checkpoint request'}
 $remaining=($DeadlineSeconds-$TrialClock.Elapsed.TotalSeconds)*1000
 if($remaining -le 0){throw 'Trial deadline reached before checkpoint'}
 $timeout=[Math]::Min($remaining,$TimeoutMilliseconds)
 $process=Get-Process -Id $ProcessId -ErrorAction Stop
 if($process.StartTime.ToUniversalTime() -ne $ProcessStartUtc.ToUniversalTime()){throw 'Checkpoint process identity changed'}
 if(Test-Path -LiteralPath $MarkerPath){
  $prior=[UInt64]::Parse([IO.File]::ReadAllText($MarkerPath).Trim(),[Globalization.CultureInfo]::InvariantCulture)
  if($Marker -le $prior){throw 'Checkpoint must increase'}
 }
 # Open before publishing, so even an immediate acknowledgement is retained.
 $stream=[IO.File]::Open($LogPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
 $reader=$null
 $temporary=$MarkerPath+'.pending-'+[guid]::NewGuid().ToString('N')
 try {
  $offset=$stream.Length
  [void]$stream.Seek($offset,[IO.SeekOrigin]::Begin)
  $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::ASCII,$false,4096)
  $bytes=[Text.Encoding]::ASCII.GetBytes($Marker.ToString([Globalization.CultureInfo]::InvariantCulture))
  $file=New-Object IO.FileStream($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
  try {$file.Write($bytes,0,$bytes.Length);$file.Flush($true)} finally {$file.Dispose()}
  if(Test-Path -LiteralPath $MarkerPath){[IO.File]::Replace($temporary,$MarkerPath,[NullString]::Value)}
  else {[IO.File]::Move($temporary,$MarkerPath)}
  $publishedQpc=[Diagnostics.Stopwatch]::GetTimestamp()
  $wait=[Diagnostics.Stopwatch]::StartNew()
  $buffer=New-Object char[] 16384
  $pending=''
  $observed=0
  while($wait.Elapsed.TotalMilliseconds -lt $timeout -and $TrialClock.Elapsed.TotalSeconds -lt $DeadlineSeconds){
   $process.Refresh()
   if($process.HasExited){throw 'Checkpoint process exited'}
   if($stream.Length -lt $offset){throw 'Checkpoint log was truncated'}
   $count=$reader.Read($buffer,0,$buffer.Length)
   if($count -eq 0){Start-Sleep -Milliseconds 20;continue}
   $observed+=$count
   if($observed -gt 8MB){throw 'Checkpoint diagnostic byte budget exceeded'}
   $pending+=[string]::new($buffer,0,$count)
   while(($newline=$pending.IndexOf([char]10)) -ge 0){
    if($newline -gt 16384){throw 'Oversized checkpoint diagnostic line'}
    $text=$pending.Substring(0,$newline)
    $pending=$pending.Substring($newline+1)
    if($text -match '^BC250 audit lifetime event=checkpoint .* marker=([0-9]+) '){
     if([UInt64]$Matches[1] -eq $Marker){
      if($text -notmatch ' pending=0 '){throw 'Pending map request at checkpoint'}
      return [pscustomobject]@{marker=$Marker;pid=$ProcessId;process_start_utc=$ProcessStartUtc.ToUniversalTime().ToString('o');utc=[DateTime]::UtcNow.ToString('o');trial_seconds=$TrialClock.Elapsed.TotalSeconds;log_start_offset=$offset;published_qpc=$publishedQpc;ack_qpc=[Diagnostics.Stopwatch]::GetTimestamp();qpc_frequency=[Diagnostics.Stopwatch]::Frequency;observed_chars=$observed;line=$text}
     }
    }
   }
   if($pending.Length -gt 16384){throw 'Oversized checkpoint diagnostic line'}
  }
  throw ("Checkpoint acknowledgement deadline exceeded: observed_chars={0} log_start_offset={1} pending_chars={2} published_qpc={3}" -f $observed,$offset,$pending.Length,$publishedQpc)
 } finally {
  if($reader){$reader.Dispose()}else{$stream.Dispose()}
  if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}
 }
}
