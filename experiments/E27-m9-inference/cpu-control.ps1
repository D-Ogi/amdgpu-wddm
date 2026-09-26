# Run cpu-control.cmd after copying it and the matching binaries/models to the lab.
$ErrorActionPreference='Stop'
$proc=Start-Process $env:ComSpec -ArgumentList '/d /c C:\BC250\m9\cpu-control.cmd' -WindowStyle Hidden -PassThru
$handle=$proc.Handle
if (-not $proc.WaitForExit(120000)) {
 & taskkill /PID $proc.Id /T /F | Out-Null
 throw 'CPU control deadline'
}
$proc.WaitForExit()
if ($proc.ExitCode -ne 0) { throw "CPU control exit $($proc.ExitCode)" }
