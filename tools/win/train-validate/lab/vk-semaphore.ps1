#Requires -Version 5
# The Win32 semaphore check of the installed Vulkan drivers: tools\win\vksemcheck, one unnamed and one named
# export/import pair of a timeline semaphore across two processes. Installs nothing and changes no setting.
# Generic: it names no train and no package.
#
#   x64 in session 0      this SSH session: the verdict of the 64-bit system ICD, with Local\ names in
#                         \BaseNamedObjects
#   x64 in the interactive session   a one-shot scheduled task of the logged-on user at the Limited run level:
#                         the same check, with Local\ names in \Sessions\<n>\BaseNamedObjects and the loader of
#                         an unelevated process
#   x86, each cell alone  the 32-bit ICD. In 0.7.216.100-tester.27 that is the earlier build, so this is the
#                         negative control: it is reported and decides nothing. Each cell is its own process,
#                         because the named cell of the earlier build can end the process.
#
# The result line that the train validation reads:
#   vk-semaphore x64 failures <n>      (the x64 runs that did not end with exit 0 and no FAIL line)
param(
    [Parameter(Mandatory)][string]$Dir,
    [string]$Out = 'C:\BC250\tmp\train-vksemcheck',
    [ValidateRange(1000, 10000)][int]$WaitMs = 5000
)
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
function Sha8($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 8) } else { 'ABSENT' } }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$out = Join-Path $Out $stamp
New-Item -ItemType Directory -Force $out | Out-Null
"vk-semaphore start $([DateTime]::UtcNow.ToString('o')) out $out"
$x64 = Join-Path $Dir 'vksemcheck-x64.exe'
$x86 = Join-Path $Dir 'vksemcheck-x86.exe'
foreach ($p in $x64, $x86) {
    if (-not (Test-Path -LiteralPath $p)) { "MISSING $p"; exit 2 }
    "client $(Split-Path -Leaf $p) sha256 $((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash)"
}
"system ICD x64   $(Sha8 (Join-Path $inst 'vulkan\vulkan_radeon.dll'))"
foreach ($p in @(Get-ChildItem -LiteralPath $inst -Recurse -Filter 'vulkan_radeon*.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'wow64|x86|32' })) { "ICD $($p.FullName.Substring($inst.Length)) $(Sha8 $p.FullName)" }
Remove-Item Env:\VK_DRIVER_FILES -ErrorAction SilentlyContinue
Remove-Item Env:\VK_ICD_FILENAMES -ErrorAction SilentlyContinue

# One run of the client in this session, with a kill bound. Returns the exit code and the number of FAIL lines.
function Run-Here([string]$Exe, [string]$Name, [string[]]$Extra, [int]$BoundS) {
    $stdout = Join-Path $out "$Name.txt"
    $childLog = Join-Path $out "$Name-child.log"
    $argList = @('--wait-ms', $WaitMs, '--child-log', ('"' + $childLog + '"')) + $Extra
    $p = Start-Process -FilePath $Exe -ArgumentList $argList -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $out "$Name.err")
    $null = $p.Handle  # without the handle taken now, PowerShell 5.1 reports no exit code
    $code = 'killed'
    if ($p.WaitForExit($BoundS * 1000)) { $code = $p.ExitCode } else {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Get-Process -Name 'vksemcheck-*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    $lines = @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue)
    $fails = @($lines | Where-Object { $_ -match '^(\[child\] )?FAIL' }).Count
    return [pscustomobject]@{ Name = $Name; Exit = $code; Fails = $fails; Lines = $lines }
}

function Show($r) {
    "--- $($r.Name): exit $($r.Exit), FAIL lines $($r.Fails)"
    $r.Lines | ForEach-Object { '  ' + $_ }
}

# The same run as a one-shot task of the interactive user. The task runs a small script that this arm writes
# into its own directory, so that the task line carries no quoting of the client's arguments.
function Run-Interactive([string]$Exe, [string]$Name, [int]$BoundS) {
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $user) { return [pscustomobject]@{ Name = $Name; Exit = 'no interactive user'; Fails = 0; Lines = @() } }
    & icacls.exe $out /grant ("${user}:(OI)(CI)M") | Out-Null
    $stdout = Join-Path $out "$Name.txt"
    $childLog = Join-Path $out "$Name-child.log"
    $done = Join-Path $out "$Name.exit"
    $script = Join-Path $out "$Name.ps1"
    @(
        '$ErrorActionPreference = ''Continue'''
        "`$p = Start-Process -FilePath '$Exe' -ArgumentList @('--wait-ms', '$WaitMs', '--child-log', '`"$childLog`"') -PassThru -WindowStyle Hidden -RedirectStandardOutput '$stdout' -RedirectStandardError '$stdout.err'; `$null = `$p.Handle"
        "if (`$p.WaitForExit($(($BoundS - 10) * 1000))) { Set-Content -LiteralPath '$done' -Value `$p.ExitCode } else { Stop-Process -Id `$p.Id -Force; Set-Content -LiteralPath '$done' -Value 'killed' }"
    ) | Set-Content -LiteralPath $script -Encoding UTF8
    $task = 'Train-vksemcheck-' + $stamp
    $action = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument ('--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $script + '"')
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds $BoundS)
    $code = 'not started'
    try {
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
        Start-ScheduledTask -TaskName $task -ErrorAction Stop
        $deadline = (Get-Date).AddSeconds($BoundS)
        while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $done)) { Start-Sleep -Milliseconds 500 }
        $code = if (Test-Path -LiteralPath $done) { (Get-Content -LiteralPath $done -Raw).Trim() } else { 'no exit file' }
    } catch { $code = 'task error: ' + $_.Exception.Message }
    finally {
        Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        Get-Process -Name 'vksemcheck-*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    $removed = -not (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue)
    $lines = @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue)
    $fails = @($lines | Where-Object { $_ -match '^(\[child\] )?FAIL' }).Count
    return [pscustomobject]@{ Name = $Name; Exit = $code; Fails = $fails; Lines = (@("task $task removed: $removed") + $lines) }
}

$s0 = Run-Here $x64 'x64-session0' @() 40
Show $s0
$s1 = Run-Interactive $x64 'x64-interactive' 50
Show $s1
$c1 = Run-Here $x86 'x86-control-unnamed' @('--cells', 'unnamed') 25
Show $c1
$c2 = Run-Here $x86 'x86-control-named' @('--cells', 'named') 25
Show $c2
$leftover = @(Get-Process -Name 'vksemcheck-*' -ErrorAction SilentlyContinue).Count
"leftover client processes $leftover"

$bad = 0
foreach ($r in $s0, $s1) { if ("$($r.Exit)" -ne '0' -or $r.Fails -ne 0) { $bad++ } }
'vk-semaphore x86 control (the earlier 32-bit ICD, decides nothing): unnamed exit {0} FAIL {1}, named exit {2} FAIL {3}' -f `
    $c1.Exit, $c1.Fails, $c2.Exit, $c2.Fails
'vk-semaphore x64 session0 exit {0} FAIL {1}, interactive exit {2} FAIL {3}' -f $s0.Exit, $s0.Fails, $s1.Exit, $s1.Fails
"vk-semaphore x64 failures $bad"
if ($bad -eq 0 -and $leftover -eq 0) { 'vk-semaphore x64: PASS'; exit 0 } else { 'vk-semaphore x64: FAIL'; exit 1 }
