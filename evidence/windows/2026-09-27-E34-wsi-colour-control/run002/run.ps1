$ErrorActionPreference='Stop'
$out='C:\BC250\m13\wsi-colour002'
$exe="$out\wsi-colour-control.exe"
$icd="$out\vulkan_radeon.dll"
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if(Test-Path "$out\start.json"){throw 'Existing run'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if((Get-FileHash $exe).Hash -ne '228EBEC0A898EDB2454304F59BB38DD2E56A0BF519E9C5D06535446122EC07C3'){throw 'Probe hash'}
if((Get-FileHash $icd).Hash -ne '0CD4A98DD80AE1248CFA1E6D4C1EB650DCF217FA9A840C002EDC4BB7EB0ADB47'){throw 'ICD hash'}
function Snapshot {
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A4 flags=15'){throw 'KMD164 health gate'}
 $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
 if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $kmd=(Get-FileHash $image).Hash
 if($kmd -ne '9B9B99D3F3FA2A32816E71C8754A6BB6349A427C33CCCD1E9B09C08761849743'){throw 'Unexpected KMD SYS'}
 $umd=(Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
 $icd=(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
 if($umd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $icd -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'){throw 'Baseline UMD/ICD mismatch'}
 $os=Get-CimInstance Win32_OperatingSystem
 return @{utc=[DateTime]::UtcNow.ToString('o');boot=$os.LastBootUpTime.ToString('o');health=$health;clock=$clock;kmd=$kmd;umd=$umd;icd=$icd;dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id);free_kib=$os.FreePhysicalMemory;total_kib=$os.TotalVisibleMemorySize}
}

$before=Snapshot
$before | ConvertTo-Json -Depth 5 | Set-Content "$out\before.json"
@{utc=[DateTime]::UtcNow.ToString('o');pid=$PID} | ConvertTo-Json | Set-Content "$out\start.json"
$code=1;$reason='';$p=$null;$captured=$false
try {
 $env:BC250_WSI_CPU_PRESENT='1'
 $env:BC250_WSI_PRESENT_LOG="$out\present.csv"
 $p=Start-Process $exe -ArgumentList $icd -WindowStyle Normal -PassThru -RedirectStandardOutput "$out\stdout.txt" -RedirectStandardError "$out\stderr.txt"
 $handle=$p.Handle
 @{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content "$out\process.json"
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while(-not $p.WaitForExit(100)) {
  if($timer.Elapsed.TotalSeconds -gt 45){throw 'Process deadline'}
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
  $fs=[IO.File]::Open("$out\stdout.txt",[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  try {$reader=New-Object IO.StreamReader($fs);$text=$reader.ReadToEnd()} finally {if($reader){$reader.Dispose()};$fs.Dispose()}
  if(!$captured -and $text -match 'capture_ready') {
   $p.Refresh()
   @($p.Modules | Where-Object {$_.ModuleName -eq 'vulkan_radeon.dll'} | ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}}) | ConvertTo-Json | Set-Content "$out\modules.json"
   & $cli fbdump "$out\primary.bmp" *> "$out\fbdump.log"
   if($LASTEXITCODE -ne 0){throw 'Primary capture failed'}
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$out\screen.png" -TimeoutSec 5
   $captured=$true
  }
 }
 $p.Refresh()
 if($p.ExitCode -ne 0){throw "Probe exit $($p.ExitCode)"}
 if(!$captured){throw 'Capture readiness never observed'}
 $code=0
} catch {$reason=$_.Exception.Message} finally {
 if($p -and !$p.HasExited){$p.Kill();[void]$p.WaitForExit(5000)}
 try {
  $after=Snapshot
  $after | ConvertTo-Json -Depth 5 | Set-Content "$out\after.json"
  if($after.boot -ne $before.boot -or (Compare-Object $before.dwm $after.dwm)){$code=1;$reason+=' Identity changed'}
 } catch {$code=1;$reason+=' Closure: '+$_.Exception.Message}
 @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;reason=$reason;captured=$captured;process_alive=($p -and !$p.HasExited)} | ConvertTo-Json | Set-Content "$out\done.json"
}
exit $code
