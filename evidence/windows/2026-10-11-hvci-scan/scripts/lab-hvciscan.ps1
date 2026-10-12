# Read-only HVCI (memory integrity) readiness scan on the lab, plus the current VBS state.
# Runs the Microsoft memory integrity scan tool against the installed drivers. Changes nothing.
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\tmp\hvci-scan'
$exe = Join-Path $dir 'hvciscan_amd64.exe'
"utc " + (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
"exe sha256 " + (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$sig = Get-AuthenticodeSignature -LiteralPath $exe
"exe signature " + $sig.Status + " " + $sig.SignerCertificate.Subject
"== DeviceGuard state"
$dg = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
if ($dg) {
    "VirtualizationBasedSecurityStatus " + $dg.VirtualizationBasedSecurityStatus
    "SecurityServicesConfigured " + ($dg.SecurityServicesConfigured -join ',')
    "SecurityServicesRunning " + ($dg.SecurityServicesRunning -join ',')
    "AvailableSecurityProperties " + ($dg.AvailableSecurityProperties -join ',')
    "RequiredSecurityProperties " + ($dg.RequiredSecurityProperties -join ',')
} else { "Win32_DeviceGuard not available" }
"== our kernel driver"
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='bc250kmd'" -ErrorAction SilentlyContinue
if ($svc) {
    "path " + $svc.PathName + " state " + $svc.State
    $sys = $svc.PathName -replace '^\\\?\?\\', '' -replace '^\\SystemRoot', $env:SystemRoot
    if (Test-Path -LiteralPath $sys) { "sys sha256 " + (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash }
}
"== hvciscan"
$p = Start-Process -FilePath $exe -WorkingDirectory $dir -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $dir 'hvciscan.out') -RedirectStandardError (Join-Path $dir 'hvciscan.err')
"exit " + $p.ExitCode
Get-Content -LiteralPath (Join-Path $dir 'hvciscan.out')
$err = Get-Content -LiteralPath (Join-Path $dir 'hvciscan.err') -ErrorAction SilentlyContinue
if ($err) { "== stderr"; $err }
