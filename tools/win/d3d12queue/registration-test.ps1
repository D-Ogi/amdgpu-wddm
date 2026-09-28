$ErrorActionPreference='Stop'
. "$PSScriptRoot\registration.ps1"
class FakeKey {
 [string[]]$Names
 [string]$Kind='MultiString'
 [int]$Writes=0
 [bool]$FailFlush=$false
 FakeKey([string[]]$names){$this.Names=$names.Clone()}
 [string]GetValueKind([string]$name){return $this.Kind}
 [object]GetValue([string]$name){return $this.Names.Clone()}
 [void]SetValue([string]$name,[string[]]$values,[Microsoft.Win32.RegistryValueKind]$kind){$this.Names=$values.Clone();$this.Writes++}
 [void]Flush(){if($this.FailFlush){throw 'Injected post-write failure'}}
}
function Check($value){if(!$value){throw 'Assertion failed'}}
function Refuses([scriptblock]$action){$failed=$false;try{& $action}catch{$failed=$true};Check $failed}
$before=[string[]]@('dx9.dll','cpu10.dll','cpu11.dll')
$p=New-M15RegistrationPlan $before 'MultiString' 'C:\BC250\m15\native001\amdgpu_wddm_d3d12.dll'
$p=$p|ConvertTo-Json -Depth 5|ConvertFrom-Json
Assert-M15RegistrationPlan $p
$key=[FakeKey]::new($before)
Set-M15Registration $key $p Install
Check (Test-M15NamesEqual $key.Names $p.after)
Check ($key.Names[0] -ceq $before[0] -and $key.Names[1] -ceq $before[1] -and $key.Names[2] -ceq $before[2])
Refuses {Set-M15Registration $key $p Install}
Set-M15Registration $key $p Restore
Set-M15Registration $key $p Restore
Check ($key.Writes -eq 2 -and (Test-M15NamesEqual $key.Names $before))
$key.FailFlush=$true
Refuses {Set-M15Registration $key $p Install}
Check (Test-M15NamesEqual $key.Names $p.after)
$key.FailFlush=$false
Set-M15Registration $key $p Restore
Check (Test-M15NamesEqual $key.Names $before)
$key.Names[1]='foreign.dll';$writes=$key.Writes
Refuses {Set-M15Registration $key $p Restore}
Check ($key.Writes -eq $writes -and $key.Names[1] -ceq 'foreign.dll')
$key=[FakeKey]::new($before);$key.Kind='String'
Refuses {Set-M15Registration $key $p Install};Check ($key.Writes -eq 0)
$key.Kind='MultiString';$p.after[0]='tampered.dll'
Refuses {Set-M15Registration $key $p Install};Check ($key.Writes -eq 0)
Refuses {New-M15RegistrationPlan $before 'String' 'C:\BC250\m15\native001\amdgpu_wddm_d3d12.dll'}
Refuses {New-M15RegistrationPlan @('one.dll','two.dll') 'MultiString' 'C:\BC250\m15\native001\amdgpu_wddm_d3d12.dll'}
Refuses {New-M15RegistrationPlan $before 'MultiString' 'amdgpu_wddm_d3d12.dll'}
Refuses {New-M15RegistrationPlan $before 'MultiString' 'C:\BC250\m15\..\amdgpu_wddm_d3d12.dll'}
Write-Output 'Native DX12 registration transaction tests passed (mock key; no registry writes).'
