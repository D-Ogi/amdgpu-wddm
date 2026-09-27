# Readiness is a separate bounded phase; no animation time is credited here.
function Wait-HostedDwmWitness {
 param([Parameter(Mandatory)][ValidateCount(3,3)][string[]]$ExpectedHashes,[scriptblock]$ReadSample,[scriptblock]$Record,
       [scriptblock]$Milliseconds,[scriptblock]$Delay,[int]$TimeoutMs=30000)
 $begin=& $Milliseconds
 $identity=$null;$attempt=0;$firstCreateUtc=$null
 while($true){
  $sample=& $ReadSample
  $elapsed=(& $Milliseconds)-$begin
  $attempt++
  if($sample.create_success -and !$firstCreateUtc){$firstCreateUtc=$sample.utc}
  $missing=@($ExpectedHashes | Where-Object {$_ -notin $sample.hashes})
  $ready=$sample.identity -and $missing.Count -eq 0 -and $sample.create_success
  $receipt=@{attempt=$attempt;elapsed_ms=$elapsed;sample=$sample;missing=$missing;ready=[bool]$ready;first_create_success_utc=$firstCreateUtc}
  & $Record $receipt
  if($sample.ambiguous){throw ('Ambiguous new DWM identities: '+($sample.candidates | ConvertTo-Json -Compress))}
  if($sample.stop){throw 'Owner STOP during hosted startup'}
  if($identity -and $sample.identity -ne $identity){throw 'DWM identity changed/lost during startup'}
  if($sample.identity -and !$identity){$identity=$sample.identity}
  if($elapsed -ge $TimeoutMs){throw 'Hosted DWM startup witness deadline expired'}
  if($ready){return $receipt}
  & $Delay
 }
}
