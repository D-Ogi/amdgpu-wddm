# M16 step 2 on unit A (2026-10-08): run the clang-built HIP program vadd.exe on our amdhip64.dll.
# Pushed files: C:\BC250\tmp\m16-hip\{amdhip64.dll,vadd.exe}. Bound: the program gets 60 s; the whole script < 120 s.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\tmp\m16-hip'
$root = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name 'InstallRoot').InstallRoot
$cli = Join-Path $root 'tools\bc250kmd_cli.exe'
'utc start {0}' -f [DateTime]::UtcNow.ToString('o')
foreach ($f in 'amdhip64.dll', 'vadd.exe') { '{0} {1} {2}' -f $f, (Get-Item "$dir\$f").Length, (Get-FileHash "$dir\$f").Hash }
'--- before: clock / temperature'; & $cli clock read 2>&1
& $cli log | Out-File -Encoding utf8 "$dir\kmd-log-before.txt"
'--- vadd.exe --expect-compute --wait-total 10000'
$p = Start-Process -FilePath "$dir\vadd.exe" -ArgumentList '--expect-compute', '--wait-total', '10000' -WorkingDirectory $dir `
    -RedirectStandardOutput "$dir\vadd.out.txt" -RedirectStandardError "$dir\vadd.err.txt" -NoNewWindow -PassThru
if (-not $p.WaitForExit(60000)) { 'TIMEOUT after 60 s: the process is left alive on purpose (a submission may be in flight)' }
else { 'exit code {0}' -f $p.ExitCode }
'--- stdout'; Get-Content "$dir\vadd.out.txt"
'--- stderr'; Get-Content "$dir\vadd.err.txt"
'--- after: clock / temperature'; & $cli clock read 2>&1
& $cli log | Out-File -Encoding utf8 "$dir\kmd-log-after.txt"
'--- driver log lines after the run that name a fault, a timeout, a TDR or a submission that did not run'
$before = @(Get-Content "$dir\kmd-log-before.txt")
$after = @(Get-Content "$dir\kmd-log-after.txt")
$new = $after | Where-Object { $before -notcontains $_ }
'{0} new lines' -f $new.Count
$new | Where-Object { $_ -match 'not run|fault|timed? ?out|TDR|reset|lost' -and $_ -notmatch '\b0 (timeouts?|refused|faults?)\b|timeouts 0' }
'utc end {0}' -f [DateTime]::UtcNow.ToString('o')
