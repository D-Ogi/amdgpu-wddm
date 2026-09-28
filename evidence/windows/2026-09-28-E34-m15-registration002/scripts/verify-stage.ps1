function Assert-KmdStage {
 param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$Expected)
 $root=[IO.Path]::GetFullPath($Directory).TrimEnd('\')+'\'
 $manifestPath=Join-Path $root 'stage-manifest.json'
 if((Get-FileHash -LiteralPath $manifestPath).Hash -ne $Expected){throw 'Stage manifest identity mismatch'}
 $manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
 if($manifest.schema -ne 1 -or !$manifest.files.PSObject.Properties.Count){throw 'Invalid stage manifest'}
 foreach($entry in $manifest.files.PSObject.Properties){
  if($entry.Name -notmatch '^[a-zA-Z0-9_./-]+$' -or $entry.Name -match '(^|/)\.\.(/|$)' -or [IO.Path]::IsPathRooted($entry.Name)){throw 'Unsafe manifest path'}
  $path=[IO.Path]::GetFullPath((Join-Path $root $entry.Name))
  if(!$path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Manifest path escaped stage'}
  if((Get-FileHash -LiteralPath $path).Hash -ne $entry.Value){throw "Stage file identity mismatch: $($entry.Name)"}
 }
 return $manifest
}
