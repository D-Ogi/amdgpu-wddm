# This frozen transition protocol restores schema-1 registry captures. A shared
# state package must use its package installer instead. Failed discovery refuses.
function Get-KmdLegacyPackageSchema {
 param([string]$Inf)
 if($Inf -match '(?i)DefaultsV2|AmdgpuStateCatalog|amdgpu-state\.bin'){return 2}
 if($Inf -match '(?m)^HKR,\s*Parameters,\s*EnableHangBugcheck,\s*0x00010001,\s*0\s*$'){return 1}
 throw 'Unknown package state schema'
}
function Get-KmdLegacyStateEvidence {
 param([string]$Directory)
 $pins=Get-Content -LiteralPath (Join-Path $Directory 'package-hashes.json') -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
 $packages=@($pins.PSObject.Properties)
 if($packages.Count -ne 2){throw 'Malformed package state pins'}
 $schemas=@();$sysHashes=@()
 foreach($package in $packages){
  if($package.Name -notmatch '^(candidate|rollback)[0-9]+(-[0-9]+)?$'){throw 'Invalid package label'}
  $folder=Join-Path $Directory $package.Name
  foreach($name in @('bc250kmd.inf','bc250kmd.sys')){
   $hash=[string]$package.Value.$name
   if($hash -notmatch '^[A-Fa-f0-9]{64}$' -or (Get-FileHash -LiteralPath (Join-Path $folder $name) -Algorithm SHA256 -ErrorAction Stop).Hash -ine $hash){throw 'Package state pin mismatch'}
  }
  $schemas+=Get-KmdLegacyPackageSchema (Get-Content -LiteralPath (Join-Path $folder 'bc250kmd.inf') -Raw -ErrorAction Stop)
  $sysHashes+=[string]$package.Value.'bc250kmd.sys'
 }
 # ImagePath is Windows service configuration, not driver state. The active
 # image must be one of the two verified schema-1 packages, even in recovery.
 $image=[string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -Name ImagePath -ErrorAction Stop).ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 if(!$image -or (Get-FileHash -LiteralPath $image -Algorithm SHA256 -ErrorAction Stop).Hash -notin $sysHashes){throw 'Installed image is not a pinned legacy package'}
 return $schemas
}
function Assert-KmdLegacyState {
 param([string]$Directory=(Split-Path -Parent $PSScriptRoot))
 try {
  $schemas=@(Get-KmdLegacyStateEvidence $Directory)
  if($schemas.Count -ne 2 -or @($schemas|Where-Object {$_ -isnot [int] -or $_ -ne 1}).Count){throw 'Shared or unknown state schema'}
 } catch {throw "Legacy transition refused: $($_.Exception.Message). Use the shared-state package installer."}
}
