param([Parameter(Mandatory)][string]$Directory,
 [ValidateRange(1,64)][int]$Sequence,
 [ValidateSet('create-device','create-queue','copy','status','exit','abort')][string]$Command,
 [switch]$Abort)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($Directory).TrimEnd('\')
if(!(Test-Path -LiteralPath $root -PathType Container)){throw 'IPC directory does not exist'}
if(Test-Path -LiteralPath (Join-Path $root 'session.json')){throw 'Session already terminal'}
if($Abort){$name='abort.request';$text='abort'}
else{
 if(!$Sequence -or !$Command){throw 'Sequence and Command required'}
 if($Sequence -gt 1 -and !(Test-Path -LiteralPath (Join-Path $root ('result-{0:d6}.json' -f ($Sequence-1))))){throw 'Previous command is not complete'}
 $name='command-{0:d6}.txt' -f $Sequence;$text="$Sequence $Command`n"
}
$destination=Join-Path $root $name
if(Test-Path -LiteralPath $destination){throw 'Command exists; never overwrite'}
$temporary=$destination+'.tmp'
$stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try{$bytes=[Text.Encoding]::ASCII.GetBytes($text);$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
[IO.File]::Move($temporary,$destination)
@{queued=$name;sequence=$Sequence;command=$Command;abort=[bool]$Abort}|ConvertTo-Json
