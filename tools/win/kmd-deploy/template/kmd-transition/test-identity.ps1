$ErrorActionPreference='Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\transition-policy.ps1"
. "$PSScriptRoot\confirmed-present-start.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
# Identity values are well formed and name exactly one promotion.
# A version is 0.7.R.B: R the ABI revision, B the build counter (B >= 1; a release package has its own B). The two
# versions differ; the ABIs differ exactly when the revisions differ.
$versionForm='^0\.7\.(0|[1-9][0-9]*)\.[1-9][0-9]*$'
if($KmdCandidateVersion -notmatch $versionForm -or $KmdRollbackVersion -notmatch $versionForm -or $KmdCandidateVersion -eq $KmdRollbackVersion){throw 'Version identity malformed'}
if($KmdCandidateAbi -notmatch '^0x000700[0-9A-F]{2}$' -or $KmdRollbackAbi -notmatch '^0x000700[0-9A-F]{2}$'){throw 'ABI identity malformed'}
# The revision is encoded in both: 0.7.R.B <-> 0x0007RRRR.
foreach($pair in @(@($KmdCandidateVersion,$KmdCandidateAbi),@($KmdRollbackVersion,$KmdRollbackAbi))){
 if([Convert]::ToInt32($pair[1].Substring(6),16) -ne [int]$pair[0].Split('.')[2]){throw "Version/ABI disagree: $($pair -join ' ')"}
}
# Labels and attempt names carry (R, B), as stage.py label_suffix writes them: R alone for B = 1, R-B otherwise.
function Get-KmdLabelSuffix([string]$Version){$p=$Version.Split('.');if([int]$p[3] -eq 1){'{0:D3}' -f [int]$p[2]}else{'{0:D3}-{1}' -f [int]$p[2],[int]$p[3]}}
if($KmdCandidateLabel -cne ('candidate'+(Get-KmdLabelSuffix $KmdCandidateVersion)) -or $KmdRollbackLabel -cne ('rollback'+(Get-KmdLabelSuffix $KmdRollbackVersion))){throw 'Label identity malformed'}
foreach($h in @($KmdRollbackSysSha256,$KmdRollbackInfSha256,$KmdRollbackCatSha256)){if($h -notmatch '^[0-9A-F]{64}$'){throw 'Rollback pin malformed'}}
# The desktop pins (stage.py desktop_pins): switches 0 or 1, a sorted set of SHA256 that holds the CPU UMD, and the
# router key exactly when the set is router + CPU UMD.
if($KmdDesktopSwitches -notin @('0','1')){throw 'Desktop switches malformed'}
$desktopModules=@($KmdDesktopModules.Split(','))
if(!$desktopModules.Count -or @($desktopModules|Where-Object {$_ -notmatch '^[0-9A-F]{64}$'}).Count -or $KmdDesktopUmdSha256 -notin $desktopModules){throw 'Desktop modules malformed'}
if((@($desktopModules|Sort-Object -Unique) -join ',') -cne $KmdDesktopModules){throw 'Desktop modules not a sorted set'}
if(($KmdDesktopRouterKey -eq '') -ne ($desktopModules.Count -eq 1)){throw 'Router key and module set disagree'}
if($KmdDesktopRouterKey -and ($KmdDesktopRouterKey -notmatch '^SOFTWARE\\[A-Za-z0-9\\-]+$' -or $desktopModules.Count -ne 2)){throw 'Router desktop pins malformed'}
# The guard admits this attempt's own directories only; the examples come from the identity's revisions.
$own='C:\BC250\m15\kmd'+(Get-KmdLabelSuffix $KmdCandidateVersion)+'-deploy001'
$previous='C:\BC250\m15\kmd'+(Get-KmdLabelSuffix $KmdRollbackVersion)+'-deploy001'
if($own -notmatch $KmdDirectoryPattern -or $previous -match $KmdDirectoryPattern -or ($own+'\x') -match $KmdDirectoryPattern -or ($own -replace 'm15','m13') -match $KmdDirectoryPattern){throw 'Directory guard wrong'}
# The ValidateSet literals of Get-KmdTransitionPhases are exactly identity.ps1's modes.
foreach($mode in @($KmdCandidateMode,$KmdSameMode,$KmdDeployMode)){[void](Get-KmdTransitionPhases candidate $mode)}
Must-Reject {Get-KmdTransitionPhases candidate 'deploy173'}
Must-Reject {Get-KmdTransitionPhases candidate 'candidate'}
$declared=(Get-Command Get-KmdTransitionPhases).Parameters['Mode'].Attributes|Where-Object {$_ -is [System.Management.Automation.ValidateSetAttribute]}
if((@($declared.ValidValues|Sort-Object) -join ',') -ne (@($KmdCandidateMode,$KmdSameMode,$KmdDeployMode|Sort-Object) -join ',')){throw 'ValidateSet differs from identity modes'}
# The self-contained present-start gate defaults to the candidate ABI and rejects the rollback's line when the
# revisions differ (two builds of one revision share the ABI; DriverVer and the SYS hash tell them apart).
$line='health abi=1 version={0} flags=15 generation=5 epoch=6 completed=7 age_ms=0 ready_ms=60000'
if(!(Get-ConfirmedPresentStart -Health ($line -f $KmdCandidateAbi) -ElapsedSeconds 0).launch){throw 'Candidate ABI health rejected'}
if($KmdCandidateAbi -ne $KmdRollbackAbi){Must-Reject {Get-ConfirmedPresentStart -Health ($line -f $KmdRollbackAbi) -ElapsedSeconds 0}}
Must-Reject {Get-ConfirmedPresentStart -Health ($line -f '0x0007FFFF') -ElapsedSeconds 0}
if(!(Get-ConfirmedPresentStart -Health ($line -f $KmdRollbackAbi) -ElapsedSeconds 0 -Abi $KmdRollbackAbi).launch){throw 'Explicit ABI ignored'}
# No version, ABI, package label, hash or lab path literal outside identity.ps1 (tests aside): the template
# serves every KMD revision, and each attempt's generated identity.ps1 is its only version-specific file.
$stale='(?i)0x0007[0-9A-F]{4}|0\.7\.[0-9]+\.[0-9]|\b(candidate|rollback|deploy|same)[0-9]{3}\b|m13\\|kmd1(?!68)[0-9]{2}|(pre|post)flight[0-9]|exact1[0-9]{2}|[0-9A-F]{64}|resource-close|desktop-umd[0-9]|wsi-final'
$allowed=@{
 'transition-policy.ps1'="param([ValidateSet('candidate','restore')][string]`$Arm,[ValidateSet('rehearsal','same','deploy')][string]`$Mode)"
}
$exempt=@('identity.ps1')
$hits=@()
foreach($file in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File){
 if($file.Name -in $exempt -or $file.Name -like 'test-*.ps1'){continue}
 $n=0
 foreach($text in [IO.File]::ReadAllLines($file.FullName)){
  $n++
  if($text -notmatch $stale){continue}
  if($allowed.ContainsKey($file.Name) -and $text.Trim() -ceq $allowed[$file.Name]){continue}
  $hits+="$($file.Name):$n"
 }
}
if($hits.Count){throw "Literal outside identity.ps1: $($hits -join ', ')"}
'PASS: identity well formed, version/ABI agree, modes match ValidateSet, present-start ABI default, no stray literals'
