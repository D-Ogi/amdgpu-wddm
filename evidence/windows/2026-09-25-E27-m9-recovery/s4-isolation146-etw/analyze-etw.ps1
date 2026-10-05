$ErrorActionPreference='Stop'
$env:TEMP='P:\bc-250\scratch\tmp'
$env:TMP=$env:TEMP
$env:_NT_SYMBOL_PATH='srv*P:\bc-250\scratch\symbols*https://msdl.microsoft.com/download/symbols'
$base='P:\bc-250\scratch\m9\s4-isolation146'
$trace=Join-Path $base 'stutter.etl'
$xperf='C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
if ((Get-Item -LiteralPath $trace).Length -ne 124780544) {throw 'Unexpected trace length; not complete'}
& $xperf -i $trace -a tracestats -detail -ao "$base\etw-stats.txt" -a dpcisr -summary -ao "$base\etw-dpcisr.txt" -a process -thread -image -ao "$base\etw-process.txt" -a profile -detail -ao "$base\etw-profile.txt"
if($LASTEXITCODE -ne 0){throw "xperf failed $LASTEXITCODE"}

& $xperf -i $trace -o "$base\etw-events.csv" -a dumper *> "$base\dump-run.log"
if($LASTEXITCODE -ne 0){throw "dump failed $LASTEXITCODE"}
