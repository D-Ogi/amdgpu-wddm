param([string]$Mode,[string]$Directory,[string]$Tool)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\run-arm.ps1"
if($Mode -eq 'cancel'){exit 41}
Write-DurableText "$Directory\mutation-start.json" '{}'
if($Mode -eq 'fail'){exit 41}
if($Mode -eq 'ok'){
 Write-DurableText "$Directory\candidate-result.json" (@{success=$true;tree_closed=$true;reason='verified'}|ConvertTo-Json)
 exit 0
}
# A nested helper and its child must both belong to the outer watchdog job.
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long](30*[Diagnostics.Stopwatch]::Frequency)
$fixture=[IO.Path]::GetFullPath("$PSScriptRoot\..\kmd168-transition\bounded-child-fixture.ps1")
Invoke-KmdBoundedChild -Tool $Tool -Deadline $deadline -Stdout "$Directory\nested.out" -Stderr "$Directory\nested.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',$fixture,'-Mode','tree','-PidFile',"$Directory\descendant.json",'-Value','nested')|Out-Null
exit 0
