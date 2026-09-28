$ErrorActionPreference='Stop'
. "$PSScriptRoot\pnp-idle.ps1"
Assert-KmdPnpIdleResult @{wait_status=[uint32]0}
foreach($bad in @($null,@{},@{wait_status='0'},@{wait_status=[uint32]258},@{wait_status=[uint32]::MaxValue},@{wait_status=[uint32]1})){
 $rejected=$false
 try{Assert-KmdPnpIdleResult $bad}catch{$rejected=$true}
 if(!$rejected){throw 'Unknown/busy PnP state accepted'}
}
'PASS: explicit WAIT_OBJECT_0 only; pending/error/missing/untyped rejected'
