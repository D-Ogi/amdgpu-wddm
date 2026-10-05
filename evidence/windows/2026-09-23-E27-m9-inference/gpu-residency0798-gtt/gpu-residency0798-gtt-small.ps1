param([string]$Name='gtt64k',[string]$Bytes='65536',[string]$Heap='gtt')
$ErrorActionPreference='Stop'
$out='C:\BC250\m9\gpu-residency0798-v4'
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
Get-FileHash "$out\gpu-residency-probe.exe" | Format-List
& C:\BC250\m8\bc250kmd_cli.exe log summary > "$out\$Name-before.log"
if(Test-Path "$out\$Name.out"){throw 'Output already exists'}
@"
@echo off
"$out\gpu-residency-probe.exe" $Bytes $Heap > "$out\$Name.out" 2> "$out\$Name.err"
echo %ERRORLEVEL% > "$out\$Name.exit"
"@ | Set-Content "$out\$Name.cmd" -Encoding ASCII
$p=Start-Process cmd.exe -ArgumentList "/c $out\$Name.cmd" -WindowStyle Hidden -PassThru

do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){Stop-Process -Id $p.Id;throw 'Temperature limit'}
 $p.Refresh()
}while(-not $p.HasExited)
$p.WaitForExit()
$result=Get-Content "$out\$Name.exit"
Get-Content "$out\$Name.out" -Tail 35
& C:\BC250\m8\bc250kmd_cli.exe log summary > "$out\$Name-after.log"
'gpu_probe_exit='+$result
