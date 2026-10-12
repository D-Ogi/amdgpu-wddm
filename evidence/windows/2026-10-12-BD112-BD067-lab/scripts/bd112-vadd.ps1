# BD-112 lab step: the two event-order negative controls of vadd (--scale-order before, omit) beside the positive
# control, on the installed tester.28 HIP tools (C:\Program Files\amdgpu-wddm\tools\hip\vadd.exe). Each run is bounded
# to 60 s. Expected: positive 0 mismatches exit 0; before and omit 65535 mismatches each and exit 1.
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$exe = Join-Path $inst 'tools\hip\vadd.exe'
$dir = 'C:\BC250\tmp\bd112'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
"utc $([DateTime]::UtcNow.ToString('o')) session $((Get-Process -Id $PID).SessionId)"
"vadd $exe $((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
"amdhip64 $((Get-FileHash -LiteralPath (Join-Path $inst 'tools\hip\amdhip64.dll') -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash)"
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
"health before $((& $cli health read 2>&1) -join ' ')"
foreach ($case in @(@('positive', @('--expect-compute')), @('before', @('--expect-compute', '--scale-order', 'before')), @('omit', @('--expect-compute', '--scale-order', 'omit')))) {
    $name = $case[0]; $args1 = $case[1]
    $out = Join-Path $dir "$name.txt"
    "=== ${name}: vadd $($args1 -join ' ')"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $args1 -WorkingDirectory (Split-Path $exe) -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $p.Handle   # PS 5.1: without a cached handle ExitCode stays empty
    if (-not $p.WaitForExit(60000)) { $p.Kill(); 'KILLED after 60 s' }
    "exit $($p.ExitCode) wall_s $('{0:F2}' -f $sw.Elapsed.TotalSeconds)"
    Get-Content -LiteralPath $out -ErrorAction SilentlyContinue | Select-Object -Last 12
    Get-Content -LiteralPath "$out.err" -ErrorAction SilentlyContinue | Select-Object -Last 5 | ForEach-Object { "stderr: $_" }
}
"health after $((& $cli health read 2>&1) -join ' ')"
