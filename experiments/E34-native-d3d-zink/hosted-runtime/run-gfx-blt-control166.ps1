param([Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedExeSha,
 [Parameter(Mandatory)][ValidatePattern('^C:\\BC250\\m13\\gfx-blt-control[0-9]{3}$')][string]$OutDir,
 [ValidateSet('single-plan','dirty-list')][string]$Mode='single-plan')
$ErrorActionPreference='Stop'
$out=$OutDir
$exe=Join-Path $out 'gfx-blt-control.exe'
if(Test-Path (Join-Path $out 'start.json')){throw 'Existing run: inspect, never overwrite or restart'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if((Get-FileHash $exe).Hash -ne $ExpectedExeSha){throw 'Control artifact mismatch'}
if(@(Get-Process witcher3,deqp-vk,vkcube,gfx-blt-control -ErrorAction SilentlyContinue).Count){throw 'Another GPU test is active'}
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
function Snapshot {
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A6 flags=15'){throw 'KMD166 health gate'}
 $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
 if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $kmd=(Get-FileHash $image).Hash
 if($kmd -ne 'AA77E8B34ADF76D90688F146837CDBECB747AB875C75AB0BED00262BE8C936DD'){throw 'Unexpected KMD SYS'}
 $umd=(Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
 $icd=(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
 if($umd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $icd -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'){throw 'Baseline UMD/ICD mismatch'}
 $os=Get-CimInstance Win32_OperatingSystem
 return @{utc=[DateTime]::UtcNow.ToString('o');boot=$os.LastBootUpTime.ToString('o');health=$health;clock=$clock;kmd=$kmd;umd=$umd;icd=$icd;dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id);free_kib=$os.FreePhysicalMemory;total_kib=$os.TotalVisibleMemorySize}
}
$before=Snapshot
$before | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'before.json')
& $cli log summary | Out-File (Join-Path $out 'before-driver.log')
if($LASTEXITCODE -ne 0){throw 'Pre-test counters failed'}
@{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;exe_sha256=$ExpectedExeSha} | ConvertTo-Json | Set-Content (Join-Path $out 'start.json')
$code=125;$reason='';$p=$null
try {
 $p=Start-Process -FilePath $exe -ArgumentList $(if($Mode -eq 'dirty-list'){'--run-list'}else{'--run'}) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
 $processHandle=$p.Handle
 @{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'process.json')
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while(-not $p.WaitForExit(1000)){
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){$reason='Owner STOP';break}
  if($watch.Elapsed.TotalSeconds -ge 150){$reason='Control timeout';break}
  $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
  if($LASTEXITCODE -ne 0 -or $clock -notmatch 'temperature_mc=(\d+)' -or [int]$Matches[1] -ge 85000){$reason='Thermal/telemetry stop';break}
 }
 if($reason){Stop-Process -Id $p.Id -ErrorAction SilentlyContinue;if(-not $p.WaitForExit(5000)){throw 'Control still active; inspect handle'}}
 $p.Refresh();$code=if($reason){124}else{$p.ExitCode}
 if($code -ne 0){throw "Control exit=$code $reason"}
 $stdout=[IO.File]::ReadAllText((Join-Path $out 'stdout.txt'))
 if($stdout -notmatch ('(?m)^COPY_MODE '+[regex]::Escape($Mode)+'\r?$')){throw 'Control mode mismatch'}
 if(([regex]::Matches($stdout,'(?m)^COPY_RESULT PASS ')).Count -ne 5 -or ([regex]::Matches($stdout,'(?m)^COPY_RESIDENCY[^\r\n]*PASS')).Count -ne 30 -or $stdout -notmatch 'GFX_BLT_CONTROL PASS' -or $stdout -match 'COPY_RESIDENCY[^\r\n]*FAIL'){throw 'Missing content/residency PASS'}
 $after=Snapshot
 $after | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'after.json')
 if($after.boot -ne $before.boot -or (Compare-Object $before.dwm $after.dwm)){throw 'Unexpected boot/DWM change'}
 $code=0
} catch {
 $reason=$_.Exception.Message;$code=1
 $_ | Out-String | Set-Content (Join-Path $out 'worker-error.txt')
} finally {
 if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -ErrorAction SilentlyContinue;[void]$p.WaitForExit(5000)}
 & $cli log summary | Out-File (Join-Path $out 'after-driver.log')
 if($LASTEXITCODE -ne 0){$code=1;$reason+=' Post-test counter read failed'}
 @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;reason=$reason;process_alive=($p -and -not $p.HasExited)} | ConvertTo-Json | Set-Content (Join-Path $out 'done.json')
}
exit $code
