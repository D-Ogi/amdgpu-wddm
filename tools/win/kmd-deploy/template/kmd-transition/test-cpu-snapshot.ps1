$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$script:metadataRead=$false
$source=[pscustomobject]@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=1;InteropLastState=0x303}
$source|Add-Member -MemberType ScriptProperty -Name PSDrive -Value {$script:metadataRead=$true;throw 'Provider graph must not be visited'}
$source|Add-Member -MemberType NoteProperty -Name PSPath -Value 'provider metadata'
$projected=Select-KmdCpuParameters $source
$ready=@{observed=@{parameters=$projected};health=@{generation=[uint64]4294967296;epoch=5};attempts=2}
$watch=[Diagnostics.Stopwatch]::StartNew()
$json=$ready|ConvertTo-Json -Depth 10
$roundtrip=$json|ConvertFrom-Json
if($script:metadataRead -or $json -match 'PSDrive|PSPath|provider metadata'){throw 'Provider metadata escaped projection'}
if($projected.Count -ne 4 -or $roundtrip.observed.parameters.UnconfirmedStarts -ne 1 -or $roundtrip.observed.parameters.InteropLastState -ne 0x303 -or $roundtrip.health.generation -ne 4294967296){throw 'Acceptance values lost'}
$missing=Select-KmdCpuParameters ([pscustomobject]@{EnableGpuPresentBlit=0})
if($null -ne $missing.UnconfirmedStarts -or $null -ne $missing.EnableCddDwmInterop -or $null -ne $missing.InteropLastState){throw 'Missing values became success'}
if($watch.Elapsed.TotalSeconds -gt 2){throw 'Plain snapshot serialization unexpectedly slow'}
'PASS: readiness JSON excludes provider graph, retains health/gates/guard/interop latch, missing stays null'
