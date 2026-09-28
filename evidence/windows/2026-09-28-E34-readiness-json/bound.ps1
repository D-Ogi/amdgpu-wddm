$ErrorActionPreference='Stop'
. 'C:\BC250\m13\kmd169-control166-001\kmd168-transition\invoke-bounded.ps1'
$r=Invoke-KmdBoundedChild -Tool 'C:\BC250\m13\kmd169-control166-001\bounded-child.exe' -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+20*[Diagnostics.Stopwatch]::Frequency) -Stdout "$PSScriptRoot\probe.out" -Stderr "$PSScriptRoot\probe.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$PSScriptRoot\probe.ps1")
$r|ConvertTo-Json -Compress
Get-Content "$PSScriptRoot\probe.out"
Get-Content "$PSScriptRoot\probe.err"
if($r.exit_code -ne 0){exit 1}
