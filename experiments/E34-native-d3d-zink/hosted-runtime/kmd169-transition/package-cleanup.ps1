function Select-KmdCandidatePackages {
 param([array]$Packages,[string]$ExpectedInfHash,[string]$ActiveInf)
 if($ExpectedInfHash -notmatch '^[A-Fa-f0-9]{64}$'){throw 'Invalid candidate INF hash'}
 foreach($package in $Packages){
  if($package.sha256 -ne $ExpectedInfHash){continue}
  if($package.name -notmatch '^oem[0-9]+\.inf$'){throw 'Unexpected published INF name'}
  if($package.name -ieq $ActiveInf){throw 'Candidate package is still active'}
  $package
 }
}
function Get-KmdPublishedPackages {
 # System driver INF files only, never owner data. Runs on the lab in a bounded phase.
 @(Get-ChildItem -LiteralPath "$env:windir\INF" -Filter 'oem*.inf' -File | ForEach-Object {
  @{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
 })
}
