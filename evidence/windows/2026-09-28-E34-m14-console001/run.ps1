param([string]$ManifestSha256)
$ErrorActionPreference='Stop';$d=$PSScriptRoot
. "$d\durable.ps1"
. "$d\verify-stage.ps1"
$null=Assert-KmdStage $d $ManifestSha256
$clock=[Diagnostics.Stopwatch]::StartNew();$result=@{status='failed';error=$null}
try{
 $before=& "$d\preflight171.ps1"|Out-String;Write-DurableText "$d\before.json" $before
 & "$d\test-active-console.ps1" -Exe "$d\bounded-child.exe" -Out "$d\cases" | Out-File "$d\test.log"
 $after=& "$d\preflight171.ps1"|Out-String;Write-DurableText "$d\after.json" $after
 $b=$before|ConvertFrom-Json;$a=$after|ConvertFrom-Json
 if($b.boot -ne $a.boot -or $b.confirmed.generation -ne $a.confirmed.generation -or $b.confirmed.epoch -ne $a.confirmed.epoch){throw 'Baseline changed'}
 $result.status='passed'
}catch{$result.error=$_.Exception.Message}
$result.elapsed=$clock.Elapsed.TotalSeconds
Write-DurableText "$d\result.json" ($result|ConvertTo-Json)
if($result.status -ne 'passed'){exit 1}
