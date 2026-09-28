$ErrorActionPreference='Stop'
. 'C:\BC250\m13\kmd169-stage005\kmd168-transition\invoke-bounded.ps1'
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+30*[Diagnostics.Stopwatch]::Frequency
$r=Invoke-KmdBoundedChild -Tool 'C:\BC250\m13\kmd169-stage005\bounded-child.exe' -Deadline $deadline -Stdout "$PSScriptRoot\inspection.stdout" -Stderr "$PSScriptRoot\inspection.stderr" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$PSScriptRoot\inspect-store.ps1")
$r|ConvertTo-Json -Compress
Get-Content "$PSScriptRoot\inspection.stdout"
Get-Content "$PSScriptRoot\inspection.stderr"
if($r.exit_code -ne 0){exit 1}
