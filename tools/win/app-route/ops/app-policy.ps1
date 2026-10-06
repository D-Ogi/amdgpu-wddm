# LAB (elevated SSH): writes the router's application policy, HKLM\SOFTWARE\amdgpu-wddm\AppRouter, and reads it back.
# The router reads the key at every OpenAdapter call, so the next D3D10/D3D11 adapter opened by any application
# follows it; processes that already opened theirs keep their route until they open another.
#   -Mode cpu                     only Mode=cpu: the application kill switch (lists and paths stay for a later return)
#   -Mode allowlist -Allow a.exe,b.exe [-Deny c.exe]   only the Allow names (minus Deny) on the GPU UMD
#   -Mode gpu-default [-Deny c.exe,d.exe]               every application but Deny (and the router's protected list)
#   -Mode remove                  deletes the key: the router's default, every application on the CPU UMD
# allowlist and gpu-default write every value exactly: GpuUmdPath = <Root>\gpu\<shell> (its hash checked against the
# package), RouteLogDirectory = <Root>\logs, Allow, and Deny with the package's deny_always names. Other values go.
# Receipt: <Root>\receipts\policy-<UTC>.json, also on stdout.
param([Parameter(Mandatory)][ValidateSet('cpu','allowlist','gpu-default','remove')][string]$Mode,
      [string]$Allow='',[string]$Deny='',[string]$Root='C:\BC250\m14\app-route-001')
$ErrorActionPreference='Stop'
. "$Root\ops\durable.ps1"
. "$Root\ops\approute-lib.ps1"
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$r=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');mode=$Mode}
$keyPath='SOFTWARE\amdgpu-wddm\AppRouter'
try {
 $m=Read-AppRouteManifest $Root
 if([string]$m.policy_key -cne $keyPath){throw 'app-route.json names another policy key'}
 $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($keyPath,$false)
 try{$before=Read-AppKeyValues $k;$r.existed=[bool]$k}finally{if($k){$k.Dispose()}}
 $r.before=$before
 if($Mode -eq 'remove'){
  if($r.existed){[Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($keyPath)}
  if([Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($keyPath,$false)){throw 'Key still present after delete'}
  $r.after=$null
 } else {
  $desired=Get-AppPolicyDesired -Mode $Mode -Allow (ConvertTo-AppList $Allow) -Deny (ConvertTo-AppList $Deny) -Manifest $m -Root $Root
  if(!$desired.partial){
   $shell=[string]$desired.values['GpuUmdPath'].value
   if((Get-AppFileSha $shell) -ne [string]$m.gpu.files.($m.gpu.shell)){throw "GPU UMD $shell is not the package's"}
   if(!(Test-Path -LiteralPath $desired.values['RouteLogDirectory'].value -PathType Container)){throw 'Log directory missing; run app-stage.ps1 first'}
  }
  $expected=Get-AppPolicyExpected $desired $before
  $k=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($keyPath)
  try{
   # A full write passes through Mode=cpu: an OpenAdapter between two writes routes to the CPU UMD, never by a
   # half-written list. Mode is written last.
   if(!$desired.partial){
    $k.SetValue('Mode','cpu',[Microsoft.Win32.RegistryValueKind]::String);$k.Flush()
    foreach($n in @($k.GetValueNames())){if(!$desired.values.Contains($n)){$k.DeleteValue($n,$false)}}
   }
   foreach($n in @($desired.values.Keys | Where-Object { $_ -ne 'Mode' }) + @('Mode')){
    $e=$desired.values[$n]
    $value=if($e.kind -ceq 'MultiString'){,([string[]]@($e.value))}else{[string]$e.value}
    $k.SetValue($n,$value,[Microsoft.Win32.RegistryValueKind]$e.kind)
   }
   $k.Flush()
   $after=Read-AppKeyValues $k
  }finally{$k.Dispose()}
  $r.after=$after
  if(!(Test-AppPolicyEqual $expected $after)){throw 'AppRouter readback mismatch'}
 }
 $r.outcome='ok'
} catch {
 $r.outcome='failed';$r.error=[string]$_
}
$json=$r|ConvertTo-Json -Depth 6
$null=New-Item -ItemType Directory -Force -Path "$Root\receipts"
Write-DurableText "$Root\receipts\policy-$stamp.json" $json
$json
if($r.outcome -ne 'ok'){exit 1}
