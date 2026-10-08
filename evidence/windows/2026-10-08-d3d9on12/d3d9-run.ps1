# D3D9 through D3D9On12 on unit A, x64 and x86 (2026-10-08). Runs the staged d3d9probe builds in the interactive
# session (session 0 cannot create a D3D9 device, K200) as one-shot scheduled tasks of the logged-on user, each mode
# bounded to 40 s, and records DWM, the KMD start confirmation and the KMD log fault lines before and after.
# Staged by the operator: C:\BC250\tmp\d3d9probe\b1008-x64\d3d9probe.exe and ...\b1008-x86\d3d9probe.exe.
# Writes nothing to the registry; the task is removed after each run. Under 2 minutes in all.
param([string]$X64Hash = 'C0CED039', [string]$X86Hash = 'DA42A183', [int]$Frames = 200, [int]$Draws = 100)
$ErrorActionPreference = 'Stop'
$base = 'C:\BC250\tmp\d3d9probe'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$task = 'D3D9 probe run'
function State([string]$when) {
    "== $when $([DateTime]::UtcNow.ToString('HH:mm:ss'))Z"
    "dwm $((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')"
    & $cli health read 2>&1 | Select-Object -First 1
    $l = & $cli log 2>&1
    'kmd log: GPU FAULT {0}, fence timeout/reset {1}' -f @($l | Select-String 'GPU FAULT').Count, @($l | Select-String 'HARDWARE FENCE TIMEOUT|ResetEngine node').Count
}
State 'before'
$user = (Get-CimInstance Win32_ComputerSystem).UserName
foreach ($arch in 'x64', 'x86') {
    $exe = Join-Path $base "b1008-$arch\d3d9probe.exe"
    $want = if ($arch -eq 'x64') { $X64Hash } else { $X86Hash }
    $have = (Get-FileHash -LiteralPath $exe).Hash.Substring(0, 8)
    if ($have -ne $want) { throw "$exe is $have, expected $want" }
    foreach ($mode in 'default', 'on12') {
        $dir = Split-Path -Parent $exe
        $out = Join-Path $dir "$mode.txt"; $log = Join-Path $dir "$mode-console.txt"; $cmd = Join-Path $dir "$mode.cmd"
        Remove-Item -Force $out, $log -ErrorAction SilentlyContinue
        Set-Content -LiteralPath $cmd -Encoding ASCII -Value @('@echo off',
            ('"' + $exe + '" --mode ' + $mode + ' --frames ' + $Frames + ' --draws ' + $Draws + ' --out "' + $out + '" > "' + $log + '" 2>&1'),
            ('>> "' + $log + '" echo exit %ERRORLEVEL%'), 'exit /b %ERRORLEVEL%')
        if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $task -Confirm:$false }
        $action = New-ScheduledTaskAction -Execute $cmd -WorkingDirectory $dir
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 45) -MultipleInstances IgnoreNew
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
        $t0 = [DateTime]::UtcNow
        try {
            Start-ScheduledTask -TaskName $task
            Start-Sleep -Milliseconds 500
            $until = (Get-Date).AddSeconds(40)
            do { Start-Sleep -Milliseconds 500; $t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue } while ($t -and $t.State -eq 'Running' -and (Get-Date) -lt $until)
            if ($t -and $t.State -eq 'Running') { Stop-ScheduledTask -TaskName $task; "$arch $mode stopped at the bound" }
        } finally { Unregister-ScheduledTask -TaskName $task -Confirm:$false }
        "=== $arch $mode ($('{0:0.0}' -f ([DateTime]::UtcNow - $t0).TotalSeconds) s, exe $have)"
        if (Test-Path -LiteralPath $out) { Get-Content -LiteralPath $out } else { 'no result file' }
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log | Select-Object -Last 2 }
    }
}
State 'after'
