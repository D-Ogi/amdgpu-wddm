# BD-081 lab step: d3d10probe --trace-entry, x64 and x86, on the installed router (tester.28 UMD, KMD .36). Each run is
# bounded to 60 s. Session 0 (SSH); the probe creates a device only, no window.
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o')) session $((Get-Process -Id $PID).SessionId)"
foreach ($arch in 'x64', 'x86') {
    $exe = "C:\BC250\tmp\bd081\$arch\d3d10probe.exe"
    $out = "C:\BC250\tmp\bd081\trace-$arch.txt"
    $err = "C:\BC250\tmp\bd081\trace-$arch.err"
    "=== $arch $((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 8))"
    $p = Start-Process -FilePath $exe -ArgumentList '--trace-entry' -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    if (-not $p.WaitForExit(60000)) { $p.Kill(); 'KILLED after 60 s' }
    "exit $($p.ExitCode)"
    Get-Content -LiteralPath $out -ErrorAction SilentlyContinue
    Get-Content -LiteralPath $err -ErrorAction SilentlyContinue | ForEach-Object { "stderr: $_" }
}
