# Host test of the payload directories an install copies (BD-183). The field failure this covers: the rc5 lab
# package stopped on 26200 at the step that keeps the installed files from being removed at the next restart,
# with Cannot find path '...\files\payload\toolssystem-tuningcontrol'. The DriverStore layout selects one
# directory, PowerShell unrolled that one-element list to the string 'tools', and the two `+=` that follow
# concatenated three names into one. So every DriverStore install broke, at that step and in the copy loop.
#
# Four contracts of Get-InstallPayloadDirs:
#   1. The DriverStore layout with both optional trees gives three separate entries, in copy order.
#   2. The DriverStore layout alone gives one entry and is still a list, not a string: that is the defect.
#   3. The older layout keeps its six trees and the optional ones follow them.
#   4. The entries are directory names a path can be built from: no name holds a separator, and each one joins
#      to the payload directory of the package it comes from.
# Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-install-dirs.ps1 [-Installer <package>\installer]
# Pure: no registry, no file and no device is read or changed.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

$store = Get-InstallPayloadDirs -DriverStoreUmd $true -HasSystemTuning $true -HasControl $true
Check ($store -is [array]) 'the DriverStore layout gives a list'
Check ($store.Count -eq 3) "three entries for the DriverStore layout with both optional trees (got $($store.Count))"
Check ((@($store) -join ',') -ceq 'tools,system-tuning,control') "the entries are tools, system-tuning, control in copy order (got $(@($store) -join ','))"

$only = Get-InstallPayloadDirs -DriverStoreUmd $true -HasSystemTuning $false -HasControl $false
Check ($only -is [array]) 'one directory is still a list, not the string the unrolled list became'
Check ($only.Count -eq 1 -and $only[0] -ceq 'tools') 'the one entry is tools, whole'
$grown = Get-InstallPayloadDirs -DriverStoreUmd $true -HasSystemTuning $false -HasControl $false
$grown += 'system-tuning'
Check ($grown.Count -eq 2 -and $grown[1] -ceq 'system-tuning') 'a later += adds an entry instead of concatenating a string'

$legacy = Get-InstallPayloadDirs -DriverStoreUmd $false -HasSystemTuning $true -HasControl $true
Check ((@($legacy) -join ',') -ceq 'd3d12,desktop,d3d11,vulkan,wow64,tools,system-tuning,control') `
    "the older layout keeps its six trees and the optional ones follow (got $(@($legacy) -join ','))"
$plain = Get-InstallPayloadDirs -DriverStoreUmd $false -HasSystemTuning $false -HasControl $false
Check ($plain.Count -eq 6) "six trees without the optional ones (got $($plain.Count))"

foreach ($set in @(, $store) + @(, $legacy)) {
    foreach ($d in $set) {
        Check ($d -cnotmatch '[\\/]' -and $d.Length -gt 0) "'$d' is a directory name, not a path"
        $joined = Join-Path 'C:\p\files\payload' $d
        Check ($joined -ceq ('C:\p\files\payload\' + $d)) "'$d' joins to one directory under payload"
    }
}

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'install payload directories: all checks passed'
exit 0
