param([string]$Workspace='P:\bc-250')
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'worker.ps1'
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Worker parse failure'}
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'WaitCheckpoint'},$true)
if(-not $function){throw 'Actual WaitCheckpoint function missing'}
Invoke-Expression $function.Extent.Text
$Out=Join-Path $Workspace ('scratch\tmp\m11-checkpoint-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $Out | Out-Null
$resolved=(Resolve-Path -LiteralPath $Out).ProviderPath
$workspacePath=(Resolve-Path -LiteralPath $Workspace).ProviderPath.TrimEnd('\')+'\'
if(-not $resolved.StartsWith($workspacePath,[StringComparison]::OrdinalIgnoreCase)){throw 'Test path outside workspace'}
Add-Type @'
using System;
using System.IO;
using System.Threading;
public static class M11MarkerTest {
 public static Thread Publish(string path) {
  var t = new Thread(() => {
   Thread.Sleep(150);
   using (File.Open(path,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
    Thread.Sleep(100);
   }
  });
  t.Start(); return t;
 }
}
'@
$checks=0
try {
 $ack=Join-Path $Out 'checkpoint-000001.ack'
 $held=[IO.File]::Open($ack,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
 try {
  $oldFailed=$false
  try {$null=[IO.File]::ReadAllText($ack)} catch [IO.IOException] {$oldFailed=$true}
  if(-not $oldFailed){throw 'Old protocol did not reproduce sharing failure'}
  $checks++
  WaitCheckpoint 1
  if(-not(Test-Path (Join-Path $Out 'checkpoint-000001.request'))){throw 'Request missing'}
  $checks++
 } finally {$held.Dispose()}
 $watch=[Diagnostics.Stopwatch]::StartNew()
 $publisher=[M11MarkerTest]::Publish((Join-Path $Out 'checkpoint-000002.ack'))
 WaitCheckpoint 2
 $publisher.Join()
 if($watch.ElapsedMilliseconds -lt 150 -or $watch.ElapsedMilliseconds -gt 3000){throw 'Wrong-cycle ACK reuse or wait failure'}
 $checks++
 foreach($number in 3..252){
  $ack=Join-Path $Out ('checkpoint-{0:D6}.ack' -f $number)
  $held=[IO.File]::Open($ack,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try {WaitCheckpoint $number} finally {$held.Dispose()}
  $checks++
 }
 "PASS: $checks actual-function checks; old read/write collision reproduced; empty-marker reads do not open ACK files"
} finally {
 # This directory was created by this test, resolved above, and stays inside P:\bc-250.
 $finalPath=(Resolve-Path -LiteralPath $Out).ProviderPath
 if($finalPath -ne $resolved -or -not $finalPath.StartsWith($workspacePath,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup target changed'}
 Remove-Item -LiteralPath $finalPath -Recurse -Force
}
