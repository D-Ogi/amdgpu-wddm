$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP'}
$out='C:\BC250\m9\candidate07142\d3d11-before.etl'
if(Test-Path $out){throw 'Evidence exists'}
& logman start BC250Display142 -ets -o $out -p Microsoft-Windows-Direct3D11 0xffffffffffffffff 5 -f bincirc -max 32
if($LASTEXITCODE -ne 0){throw 'ETW start failed'}
try{Start-Sleep -Seconds 5}finally{& logman stop BC250Display142 -ets}
& tracerpt $out -o C:\BC250\m9\candidate07142\d3d11-before.xml -of XML -y
if($LASTEXITCODE -ne 0){throw 'ETW decode failed'}
'capture_complete='+(Get-Date).ToString('s')
