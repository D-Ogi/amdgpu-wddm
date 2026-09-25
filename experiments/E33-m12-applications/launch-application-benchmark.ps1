param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-z0-9-]+$')][string]$Run,
 [Parameter(Mandatory=$true)][string]$Package,
 [Parameter(Mandatory=$true)][ValidateSet('Instancing9','Instancing10','AsteroidsVk','Asteroids11','Asteroids12')][string]$Kind,
 [Parameter(Mandatory=$true)][string]$VulkanIcd,
 [Parameter(Mandatory=$true)][string]$Cli,
 [Parameter(Mandatory=$true)][string]$CacheDirectory,
 [ValidateSet('Fresh','Reuse')][string]$CacheMode='Fresh',
 [ValidateRange(30,1800)][int]$TimeoutSeconds=300,
 [switch]$Capture
)
$ErrorActionPreference='Stop'
if(@(Get-ScheduledTask 'BC250-M12-*' | Where-Object {$_.State -in @('Running','Queued')}).Count){throw 'Another M12 task is active; close gallery before benchmark'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user session'}
$out="C:\BC250\m12\$Run"
$taskName="BC250-M12-app-$Run"
if((Test-Path $out) -or (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue)){throw 'Existing run'}
$worker=Join-Path $PSScriptRoot 'application-benchmark-worker.ps1'
foreach($path in @($Package,$VulkanIcd,$Cli,$worker)){
 if(-not(Test-Path -LiteralPath $path)){throw "Missing input: $path"}
 if($path.Contains('"')){throw 'Quote in path'}
}
if(-not [IO.Path]::IsPathRooted($CacheDirectory) -or $CacheDirectory.Contains('"')){throw 'Invalid cache directory'}
$arguments="-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$worker`" -Out `"$out`" -Package `"$Package`" -Kind $Kind -VulkanIcd `"$VulkanIcd`" -Cli `"$Cli`" -TimeoutSeconds $TimeoutSeconds -CacheDirectory `"$CacheDirectory`" -CacheMode $CacheMode"
if($Capture){$arguments+=' -Capture'}
$action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $arguments
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromSeconds($TimeoutSeconds+180)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
@{task=$taskName;kind=$Kind;cache_directory=$CacheDirectory;cache_mode=$CacheMode;capture=[bool]$Capture;worker_sha256=(Get-FileHash $worker).Hash;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$out-launch.json" -Encoding UTF8
Start-ScheduledTask -TaskName $taskName
@{task=$taskName;output=$out} | ConvertTo-Json
