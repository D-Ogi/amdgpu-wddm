$ErrorActionPreference='Stop'
$out='C:\BC250\m9\resume146-s4'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) {throw 'Owner STOP requested'}
$ready=Get-Content "$out\ready.txt" -Raw
if($ready -notmatch '(?m)^pid=(\d+)'){throw 'No ready PID'}
$probeId=[int]$Matches[1]
$p=Get-Process -Id $probeId
if($p.ProcessName -ne 'gpu-residency-probe'){throw 'Original probe absent'}
$before=Get-Content "$out\processes-before.json" -Raw | ConvertFrom-Json
$expected=@($before | Where-Object {$_.ProcessName -eq 'gpu-residency-probe'})
if($expected.Count -ne 1 -or $expected[0].Id -ne $probeId){throw 'PID changed'}
$health=& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read | Out-String
$health
if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x00070092 flags=(7|15) '){throw 'GPU/display not ready'}
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe log | Out-File "$out\resumed-driver.log"
if($LASTEXITCODE -ne 0){throw 'No resumed driver log'}
Select-String -Path "$out\resumed-driver.log" -Pattern 'power:|retained resume' | ForEach-Object {$_.Line}
if(Test-Path "$out\release.txt"){throw 'Release already exists'}
[IO.File]::WriteAllText("$out\release.txt",[string]$probeId,[Text.Encoding]::ASCII)
'released_original_pid='+$probeId
$deadline=(Get-Date).AddSeconds(100)
while(-not (Test-Path "$out\probe.exit")){
 Start-Sleep -Seconds 1
 if((Get-Date) -ge $deadline){Stop-ScheduledTask BC250-M9-Resume146Probe;throw 'Postresume readback deadline; no retry'}
}
$code=[int](Get-Content "$out\probe.exit")
Get-Content "$out\probe.out" -Tail 22
'probe_exit='+$code
if($code -ne 0){throw 'Retained content failed; no retry'}
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe log summary | Out-File "$out\after-driver.log"
'probe_completed'
