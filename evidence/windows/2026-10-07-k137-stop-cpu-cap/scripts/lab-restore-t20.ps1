# LAB: put the tester.20 KMD (release payload, DriverVer 0.7.216.100, SYS BB58D62A) back over 0.7.216.15 and restart
# Windows (deploy-candidate.ps1). -Check: only list what is staged.
param([switch]$Check)
$dirs = 'C:\BC250\tmp\kmd216-14-release', 'C:\BC250\tmp\t20-kmd', 'C:\BC250\tmp\tester20\payload\kmd'
foreach ($d in $dirs) {
    if (Test-Path "$d\bc250kmd.inf") {
        $sys = Get-ChildItem $d -Filter bc250kmd.sys -ErrorAction SilentlyContinue | Select-Object -First 1
        "staged $d sys $(if ($sys) { (Get-FileHash $sys.FullName).Hash.Substring(0,8) } else { 'none' })"
    } else { "absent $d" }
}
"defaults $(Test-Path C:\BC250\tmp\k137\registry-defaults.json)"
if ($Check) { return }
$inf = $dirs | Where-Object { Test-Path "$_\bc250kmd.inf" } | Select-Object -First 1
if (-not $inf) { 'no staged tester.20 KMD: nothing done'; return }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\BC250\tmp\k137\deploy-candidate.ps1 -Inf "$inf\bc250kmd.inf" -Defaults C:\BC250\tmp\k137\registry-defaults.json 2>&1
