# Host test of firmware-preflight.ps1 (BD-325): the dry-run reading of the installer's GPU firmware preflight line.
# The lines are in the form install.ps1 Add-Check 'GPU firmware' writes them. No network, no files.
#   powershell -File tools\release\test-firmware-preflight.ps1      (Windows PowerShell 5.1 or PowerShell 7)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'firmware-preflight.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
$c = '0123456789abcdef0123456789abcdef01234567'
function Line([string]$Status, [string]$Detail) { "[$Status] GPU firmware   $Detail" }

$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: git.kernel.org, gitlab.com reachable")
Check ($p -and ($p.reachable -join ',') -eq 'git.kernel.org,gitlab.com' -and $p.unreachable -eq '') 'both hosts reachable: [ok], nothing unreachable'

# The r3 cap package dry run of 2026-11-06 (scratch\t38\wddm-b4\capset3\dryrun-lab.run1.log line 568).
$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: git.kernel.org reachable; not reachable: gitlab.com The operation has timed out.")
Check ($p -and ($p.reachable -join ',') -eq 'git.kernel.org' -and $p.unreachable -eq 'gitlab.com The operation has timed out.') 'one host reachable (the BD-325 line): [ok], the other host named'
$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: gitlab.com reachable; not reachable: git.kernel.org 503")
Check ($p -and ($p.reachable -join ',') -eq 'gitlab.com') 'the other host alone: [ok]'

# Negative controls: what the test must still refuse.
Check ($null -eq (Get-FirmwarePreflight (Line 'fail' "no download host answers (git.kernel.org x; gitlab.com y). The installer downloads the GPU firmware"))) 'no host reachable ([fail] line): refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'fail' "download from linux-firmware ${c}: git.kernel.org reachable"))) 'a [fail] status with a host list: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware 0123abc: git.kernel.org reachable"))) 'a commit that is not 40 hex digits: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: example.com reachable"))) 'a host outside the installer list: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: gitlab.com, gitlab.com reachable"))) 'the same host twice: refused'
Check ($null -eq (Get-FirmwarePreflight 'nothing about firmware here')) 'no firmware line: refused'

if ($fail) { "TEST firmware-preflight FAIL $fail"; exit 1 }
'TEST firmware-preflight PASS'
