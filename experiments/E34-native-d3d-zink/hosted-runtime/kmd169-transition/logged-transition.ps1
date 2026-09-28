# RunLogging must launch bounded independent children, not execute registry writes here.
function Invoke-KmdLoggedTransition {
 param([scriptblock]$RunLogging,[scriptblock]$Transition)
 $result=$null
 try {
  if(!(& $RunLogging 'Enable')){throw 'Logging setup unverified; transition not launched'}
  $result=& $Transition
 }catch{
  $result=@{status='recovery-required';reason='transition-or-logging-exception';error=[string]$_;restored=$false;candidate_verified=$false}
 }
 # Not a finally block in a killable installer: the independent supervisor owns this call.
 try{$loggingRestored=[bool](& $RunLogging 'Restore')}catch{$loggingRestored=$false;$result.logging_error=[string]$_}
 $result.logging_restored=$loggingRestored
 if(!$loggingRestored){$result.status='recovery-required';$result.logging_recovery_required=$true}
 return $result
}
