$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07166'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if(Test-Path "$out\otg-probe-v2.json"){throw 'Probe receipt exists'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$health=& $cli health read | Out-String
if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A6 flags=15'){throw 'Exact166 health required'}
$samples=@()
foreach($i in 0..2){
 $text=& $cli log summary | Out-String
 $code=$LASTEXITCODE
 $text | Set-Content "$out\otg-v2-sample-$i.txt"
 if($code -ne 0){throw 'Summary failed'}
 $times=[regex]::Matches($text,'vsync snapshot: 100ns begin (\d+) end (\d+) valid ([0-9A-Fa-f]+)')
 $registers=[regex]::Matches($text,'vsync snapshot: frame ([0-9A-Fa-f]+) sync ([0-9A-Fa-f]+) control ([0-9A-Fa-f]+) lock ([0-9A-Fa-f]+) position ([0-9A-Fa-f]+)')
 if(!$times.Count -or $times.Count -ne $registers.Count){throw 'Missing or unmatched OTG records'}
 $time=$times[$times.Count-1];$regs=$registers[$registers.Count-1]
 if(!$time.Success -or !$regs.Success -or [Convert]::ToUInt32($time.Groups[3].Value,16) -ne 31){throw 'Incomplete OTG sample'}
 $samples+=@{begin=[uint64]$time.Groups[1].Value;end=[uint64]$time.Groups[2].Value;frame=[Convert]::ToUInt32($regs.Groups[1].Value,16);sync=$regs.Groups[2].Value;control=$regs.Groups[3].Value;lock=$regs.Groups[4].Value;position=$regs.Groups[5].Value}
 if($i -lt 2){Start-Sleep -Milliseconds 250}
}
foreach($i in 0..2){if($samples[$i].end -lt $samples[$i].begin){throw 'Time reversed'}}
foreach($i in 1..2){if($samples[$i].begin -le $samples[$i-1].end -or $samples[$i].frame -eq $samples[$i-1].frame){throw 'No advancing frame/time witness'}}
@{utc=[DateTime]::UtcNow.ToString('o');pass=$true;samples=$samples;scope='CPU baseline OTG read positive control, no GPU desktop claim'}|ConvertTo-Json -Depth 5|Set-Content "$out\otg-probe-v2.json"
Get-Content "$out\otg-probe-v2.json"
