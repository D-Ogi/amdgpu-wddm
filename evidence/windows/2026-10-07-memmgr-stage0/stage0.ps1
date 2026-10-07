# Memory scoping stage 0 (REPORT.md): which adapter reported NON_LOCAL 0 / LOCAL 3.7 GB. d3d12caps on adapter 0 and
# adapter 1 in the same run, each to a durable file, and the KMD's own segment view. No driver change, bounded.
$ErrorActionPreference = 'Continue'
$out = 'C:\BC250\tmp\memmgr-stage0'
$null = New-Item -ItemType Directory -Force $out
$caps = Get-ChildItem C:\BC250 -Recurse -Filter *d3d12caps.exe -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $caps) { 'no d3d12caps.exe under C:\BC250'; exit 2 }
'{0}  {1}' -f (Get-FileHash $caps.FullName).Hash.Substring(0, 8), $caps.FullName
foreach ($a in 0, 1) {
    $f = "$out\caps-adapter$a.json"
    $p = Start-Process -FilePath $caps.FullName -ArgumentList "$a", "`"$f`"" -NoNewWindow -PassThru -RedirectStandardOutput "$out\caps-adapter$a.out" -RedirectStandardError "$out\caps-adapter$a.err"
    if (-not $p.WaitForExit(60000)) { $p.Kill(); "adapter ${a}: killed after 60 s" }
    "adapter $a exit $($p.ExitCode), json $((Get-Item $f -ErrorAction SilentlyContinue).Length) bytes"
    if (Test-Path $f) { Select-String -Path $f -Pattern 'adapter_index|Description|DedicatedVideoMemory|DedicatedSystemMemory|SharedSystemMemory|Budget|CurrentUsage|NON_LOCAL|LOCAL' | Select-Object -First 24 | ForEach-Object { '  ' + $_.Line.Trim() } }
}
'--- vram'
& 'C:\BC250\dpaudio\bc250kmd_cli.exe' vram 2>&1
