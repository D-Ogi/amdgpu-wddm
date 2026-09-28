$ErrorActionPreference='Stop'
$d=$PSScriptRoot
$exe=Join-Path $d 'test-shared-log.exe'
$before=@(Get-Process dwm | Select-Object Id,StartTime)
$result=@()
foreach($mode in 'legacy','default','closed','buffered') {
 $path=Join-Path $d ($mode+'.txt')
 if(Test-Path $path){throw 'Existing log-control output'}
 $output=(& $exe $mode $path | Out-String)
 $code=$LASTEXITCODE
 $expected=0;if($mode -eq 'buffered'){$expected=10}
 if($code -ne $expected){throw "Unexpected exit $mode $code"}
 $bytes=[IO.File]::ReadAllBytes($path)
 $lines=@([IO.File]::ReadAllLines($path))
 if($mode -ne 'buffered') {
  if($lines.Count -ne 1000){throw 'Wrong line count'}
  for($i=0;$i -lt 1000;$i++){if(!$lines[$i].Contains("seq=$i ")){throw 'Wrong sequence'}}
 } elseif($bytes.Length -ne 0){throw 'Buffered data survived forced termination'}
 $result+=@{mode=$mode;exit=$code;stdout=$output;bytes=$bytes.Length;lines=$lines.Count;sha256=(Get-FileHash $path).Hash}
}
$after=@(Get-Process dwm | Select-Object Id,StartTime)
if(($before|ConvertTo-Json -Compress) -ne ($after|ConvertTo-Json -Compress)){throw 'DWM identity changed'}
@{utc=[DateTime]::UtcNow.ToString('o');exe_sha256=(Get-FileHash $exe).Hash;dwm_unchanged=$true;results=$result}|ConvertTo-Json -Depth 5
