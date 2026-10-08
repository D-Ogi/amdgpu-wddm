# Confirm the current KMD start (health flags 7 -> 15) on a fresh witness; heartbeat must be running.
# The installed release is the only KMD client (owner, 2026-10-08); InstallRoot is what its installer wrote.
$cli = Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 50) {
    $h = & $cli health read
    if ($h -match 'flags=15 ') { "already confirmed: $h"; exit 0 }
    if ($h -match 'flags=7 generation=(\d+) epoch=(\d+) .* age_ms=(\d+) ready_ms=(\d+)' -and [UInt64]$Matches[3] -lt 5000 -and [UInt64]$Matches[4] -ge 60000) {
        "read: $h"; & $cli health confirm $Matches[1] $Matches[2]; "confirm exit $LASTEXITCODE"; break
    }
    Start-Sleep -Milliseconds 250
}
"after: $(& $cli health read)"
