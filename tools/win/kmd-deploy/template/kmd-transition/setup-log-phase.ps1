param([Parameter(Mandatory)][ValidateSet('Enable','Restore')][string]$Mode,
 [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\setup-log.ps1"
$Directory=[IO.Path]::GetFullPath($Directory)
if($Directory -notmatch $KmdDirectoryPattern){throw 'Unexpected directory'}
$b=Get-Content "$Directory\boundary.json" -Raw|ConvertFrom-Json
$elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-[long]$b.qpc)/[double]$b.frequency
if($b.frequency -ne [Diagnostics.Stopwatch]::Frequency -or $elapsed -lt 0 -or $elapsed -ge 175){throw 'Logging phase deadline invalid'}
$backup="$Directory\setup-log-before.json"
if($Mode -eq 'Enable'){
 if(Test-Path $backup){throw 'Logging already attempted'}
 Write-DurableText "$Directory\setup-log-offsets-before.json" ((Get-KmdSetupLogOffsets)|ConvertTo-Json -Depth 5)
 Start-KmdSetupLog -Save {param($state);Write-DurableText $backup ($state|ConvertTo-Json)}|Out-Null
}else{
 if(Test-Path $backup){Restore-KmdSetupLog (Get-Content $backup -Raw|ConvertFrom-Json)}
 Write-DurableText "$Directory\setup-log-offsets-after.json" ((Get-KmdSetupLogOffsets)|ConvertTo-Json -Depth 5)
}
Write-DurableText "$Directory\setup-log-$($Mode.ToLowerInvariant())-done.json" (@{mode=$Mode;qpc=[Diagnostics.Stopwatch]::GetTimestamp();state=(Get-KmdSetupLogLevel)}|ConvertTo-Json -Depth 5)
