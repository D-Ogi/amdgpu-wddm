# Confirm the current KMD start (health flags 7 -> 15) on a fresh witness; heartbeat must be running.
# The installed release is the only KMD client (2026-10-08); InstallRoot is what its installer wrote.
# BD-051: the health reads go through kmd-reader.ps1 in this process (target.py ps pushes it beside this script); a
# CLI start every 250 ms inside the ssh session was one half of the sshd accept stall. Only the confirm, a write
# done once, starts bc250kmd_cli.
$cli = Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
$reader = Join-Path $PSScriptRoot 'kmd-reader.ps1'
if (-not (Test-Path -LiteralPath $reader)) { throw 'kmd-reader.ps1 is not beside this script (target.py ps pushes it)' }
. $reader
$null = Initialize-KmdReader
function Read-HealthLine {
    try { $h = Read-KmdHealth -TimeoutMs 2000 } catch { return 'health refused: ' + $_.Exception.Message }
    if ($null -eq $h) { return 'health lost: ' + (Get-KmdReaderLoss) }
    return $h
}
$sw = [Diagnostics.Stopwatch]::StartNew()
$confirm = $null
while ($sw.Elapsed.TotalSeconds -lt 50) {
    $h = Read-HealthLine
    # 31 and 23: QUIET (16) beside the start bits; it stands in for a fresh completion, as in the KMD's CONFIRM.
    if ($h -match 'flags=(15|31) ') { "already confirmed: $h"; exit 0 }
    if ($h -match 'flags=(7|23) generation=(\d+) epoch=(\d+) .* age_ms=(\d+) ready_ms=(\d+)' -and ($Matches[1] -eq '23' -or [UInt64]$Matches[4] -lt 5000) -and [UInt64]$Matches[5] -ge 60000) {
        $confirm = @($Matches[2], $Matches[3]); "read: $h"; break
    }
    Start-Sleep -Milliseconds 250
}
if ($confirm) { & $cli health confirm $confirm[0] $confirm[1]; "confirm exit $LASTEXITCODE" }
"after: $(Read-HealthLine)"
