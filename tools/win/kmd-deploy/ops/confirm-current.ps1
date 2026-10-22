# Confirm the current KMD start (health flags 7 -> 15) on a fresh witness; heartbeat must be running.
# The installed release is the only KMD client (2026-10-08); InstallRoot is what its installer wrote.
$cli = Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 50) {
    $h = & $cli health read
    # 31 and 23: QUIET (16) beside the start bits; it stands in for a fresh completion, as in the KMD's CONFIRM.
    if ($h -match 'flags=(15|31) ') { "already confirmed: $h"; exit 0 }
    if ($h -match 'flags=(7|23) generation=(\d+) epoch=(\d+) .* age_ms=(\d+) ready_ms=(\d+)' -and ($Matches[1] -eq '23' -or [UInt64]$Matches[4] -lt 5000) -and [UInt64]$Matches[5] -ge 60000) {
        "read: $h"; & $cli health confirm $Matches[2] $Matches[3]; "confirm exit $LASTEXITCODE"; break
    }
    Start-Sleep -Milliseconds 250
}
"after: $(& $cli health read)"
