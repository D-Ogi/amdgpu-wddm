# Runs vkmembw on unit A with the KMD's DPM telemetry sampled beside it (one long-lived CLI process, no spawn loop),
# then prints the result and the clock/busy samples. Bounded: the test itself takes seconds, telemetry 60 x 250 ms.
param(
    [string]$Exe = 'C:\BC250\vkmembw\vkmembw.exe',
    # The DPM sampler's CLI (trial-samplers.ps1): `dpm <count> <interval ms>`. The KMD 193 CLI and the
    # candidate07147 CLI have no telemetry command.
    [string]$Cli = 'C:\BC250\dpm\bc250kmd_cli.exe',
    [string]$Tag = 'a',
    [string]$Arguments = ''
)
$ErrorActionPreference = 'Stop'
$out = "C:\BC250\tmp\membw-$Tag.txt"
$tel = "C:\BC250\tmp\membw-$Tag-telemetry.txt"
$t0 = Get-Date
$p = Start-Process -FilePath $Cli -ArgumentList 'dpm', '60', '250' -RedirectStandardOutput $tel -NoNewWindow -PassThru
Start-Sleep -Milliseconds 600
# Through cmd: the ICD writes to stderr, which Windows PowerShell 5.1 turns into error records.
& cmd.exe /c "`"$Exe`" $Arguments > `"$out`" 2>&1"
$code = $LASTEXITCODE
$t1 = Get-Date
if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force }
"exit $code, test {0:N1} s, started {1:HH:mm:ss.fff}Z" -f ($t1 - $t0).TotalSeconds, $t0.ToUniversalTime()
Get-Content $out | Where-Object { $_ -notmatch '^\s+(type|heap) ' }
'--- dpm samples (250 ms)'
Get-Content $tel
