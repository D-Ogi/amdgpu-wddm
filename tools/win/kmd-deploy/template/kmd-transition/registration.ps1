# Graphics registration in the adapter's software (class) key: every UserModeDriver*, Vulkan*, OpenGL*, OpenCL*,
# D3D*, DXGI* and DirectX* value, captured before the transition and written back after each install in its
# captured kind, complete, then compared entry by entry. An install writes its own UserModeDriverName (171: three
# bc250umd.dll entries); the deployed list has four (the fourth is the D3D12 slot), and only this rewrite keeps it.
# Values of these families that an install adds are removed, so the key ends as captured, absence included.
# Key is a Microsoft.Win32.RegistryKey or a test double with GetValueNames/GetValueKind/GetValue/SetValue/
# DeleteValue/Flush.
function Test-KmdGraphicsRegistrationName {
 param([string]$Name)
 return ($Name -match '^(UserModeDriver|Vulkan|OpenGL|OpenCL|D3D|DXGI|DirectX)')
}
function Get-KmdMapEntries {
 param($Map)
 if($null -eq $Map){return}
 if($Map -is [Collections.IDictionary]){foreach($k in @($Map.Keys)){[pscustomobject]@{Name=[string]$k;Value=$Map[$k]}}}
 else{foreach($p in $Map.PSObject.Properties){[pscustomobject]@{Name=$p.Name;Value=$p.Value}}}
}
function ConvertTo-KmdRegistrationEntry {
 param([string]$Name,$Entry)
 if($null -eq $Entry){throw "Missing registration entry: $Name"}
 $kind=[string]$Entry.kind
 switch -CaseSensitive ($kind){
  'MultiString' {
   $items=@($Entry.value)
   foreach($item in $items){if($item -isnot [string]){throw "Invalid multi-string item: $Name"}}
   return @{kind=$kind;value=[string[]]$items}
  }
  {$_ -in @('String','ExpandString')} {
   if($Entry.value -isnot [string]){throw "Invalid string registration: $Name"}
   return @{kind=$kind;value=[string]$Entry.value}
  }
  'DWord' {
   if($null -eq $Entry.value -or $Entry.value -isnot [ValueType]){throw "Invalid DWORD registration: $Name"}
   return @{kind=$kind;value=[int]$Entry.value}
  }
  default {throw "Unsupported graphics registration kind: $Name $kind"}
 }
}
function Read-KmdGraphicsRegistration {
 param([Parameter(Mandatory)]$Key)
 $result=[ordered]@{}
 foreach($name in @($Key.GetValueNames()|Where-Object {Test-KmdGraphicsRegistrationName $_}|Sort-Object)){
  $kind=[string]$Key.GetValueKind($name)
  # No environment expansion: the stored text is what gets written back.
  $raw=$Key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
  # Other kinds are read as names only: an install-added one can still be removed, and a captured one is
  # refused by Assert-KmdGraphicsRegistrationBaseline before anything changes.
  $value=switch -CaseSensitive ($kind){
   'MultiString' {,([string[]]@($raw))}
   'String' {[string]$raw}
   'ExpandString' {[string]$raw}
   'DWord' {[int]$raw}
   default {$null}
  }
  $result[$name]=@{kind=$kind;value=$value}
 }
 return $result
}
function Test-KmdRegistrationEntryEqual {
 param($Expected,$Actual)
 if($Expected.kind -cne $Actual.kind){return $false}
 if($Expected.kind -ceq 'MultiString'){
  $e=@($Expected.value);$a=@($Actual.value)
  if($e.Count -ne $a.Count){return $false}
  for($i=0;$i -lt $e.Count;$i++){if([string]$e[$i] -cne [string]$a[$i]){return $false}}
  return $true
 }
 if($Expected.kind -ceq 'DWord'){return ([int]$Expected.value -eq [int]$Actual.value)}
 return ([string]$Expected.value -ceq [string]$Actual.value)
}
# Pure comparison of two captures (receipt or fresh read): same names, same kinds, every entry equal.
function Assert-KmdGraphicsRegistration {
 param($Expected,$Observed)
 $e=[ordered]@{};foreach($x in Get-KmdMapEntries $Expected){$e[$x.Name]=ConvertTo-KmdRegistrationEntry $x.Name $x.Value}
 $o=[ordered]@{};foreach($x in Get-KmdMapEntries $Observed){$o[$x.Name]=ConvertTo-KmdRegistrationEntry $x.Name $x.Value}
 # Value names are case-insensitive in the registry; contents are compared exactly.
 $en=@($e.Keys|Sort-Object);$on=@($o.Keys|Sort-Object)
 if(($en -join '|') -ne ($on -join '|')){throw "Graphics registration names differ: expected [$($en -join ',')] observed [$($on -join ',')]"}
 foreach($name in $en){if(!(Test-KmdRegistrationEntryEqual $e[$name] $o[$name])){throw "Graphics registration mismatch: $name"}}
}
# Admission of a capture: both lists the CPU desktop uses exist and hold no empty entry.
function Assert-KmdGraphicsRegistrationBaseline {
 param($Registration)
 $r=[ordered]@{};foreach($x in Get-KmdMapEntries $Registration){
  if(!(Test-KmdGraphicsRegistrationName $x.Name)){throw "Unexpected registration name: $($x.Name)"}
  $r[$x.Name]=ConvertTo-KmdRegistrationEntry $x.Name $x.Value
 }
 foreach($name in @('UserModeDriverName','VulkanDriverName')){
  if(!$r.Contains($name)){throw "Missing graphics registration: $name"}
  $items=@($r[$name].value)
  if(!$items.Count -or @($items|Where-Object {[string]::IsNullOrWhiteSpace([string]$_)}).Count){throw "Invalid graphics registration: $name"}
 }
 return $r
}
function Set-KmdGraphicsRegistration {
 param([Parameter(Mandatory)]$Key,[Parameter(Mandatory)]$Saved)
 $desired=Assert-KmdGraphicsRegistrationBaseline $Saved
 $before=Read-KmdGraphicsRegistration $Key
 $removed=@()
 foreach($name in @($before.Keys)){if(!$desired.Contains($name)){$Key.DeleteValue($name,$false);$removed+=$name}}
 foreach($name in @($desired.Keys)){
  $Key.SetValue($name,$desired[$name].value,[Microsoft.Win32.RegistryValueKind]$desired[$name].kind)
 }
 $Key.Flush()
 $after=Read-KmdGraphicsRegistration $Key
 Assert-KmdGraphicsRegistration $desired $after
 return @{before=$before;removed=$removed;after=$after}
}
# Files the two CPU-desktop lists name, hashed; a bare name is the System32 copy an INF put there.
function Get-KmdRegistrationFiles {
 param($Registration,[string]$System32="$env:windir\System32")
 $r=Assert-KmdGraphicsRegistrationBaseline $Registration
 $files=[ordered]@{}
 foreach($name in @('UserModeDriverName','VulkanDriverName')){
  foreach($item in @($r[$name].value)){
   $path=if([IO.Path]::IsPathRooted($item)){$item}else{Join-Path $System32 $item}
   if($files.Contains($path)){continue}
   $files[$path]=if(Test-Path -LiteralPath $path -PathType Leaf){(Get-FileHash -LiteralPath $path).Hash}else{'missing'}
  }
 }
 return $files
}
function Assert-KmdRegistrationFiles {
 param($Expected,$Observed)
 $e=@(Get-KmdMapEntries $Expected);$o=@{};foreach($x in Get-KmdMapEntries $Observed){$o[$x.Name]=[string]$x.Value}
 if(!$e.Count -or $e.Count -ne $o.Count){throw 'Registration file set changed'}
 foreach($x in $e){if(!$o.ContainsKey($x.Name) -or $o[$x.Name] -cne [string]$x.Value){throw "Registration file changed: $($x.Name)"}}
}
