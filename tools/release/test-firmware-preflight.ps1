# Host test of firmware-preflight.ps1 (BD-325): the dry-run reading of the installer's GPU firmware preflight line.
# The lines are in the form install.ps1 Add-Check 'GPU firmware' writes them. The hosts come from the firmware
# url_templates of release-sources.json, the source of the package manifest. No network.
#   powershell -File tools\release\test-firmware-preflight.ps1      (Windows PowerShell 5.1 or PowerShell 7)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'firmware-preflight.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
$c = '0123456789abcdef0123456789abcdef01234567'
function Line([string]$Status, [string]$Detail) { "[$Status] GPU firmware   $Detail" }

# The hosts of the installer's list, read from the manifest data and not from a copy.
$sources = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-sources.json') -Raw | ConvertFrom-Json
$h = Get-FirmwareHosts $sources.firmware
Check (($h -join ',') -eq 'git.kernel.org,gitlab.com') "the url_templates of release-sources.json give the two hosts in order: $($h -join ',')"
$two = Get-FirmwareHosts ([pscustomobject]@{ url_templates = @('https://a.example/x/{path}', 'https://a.example/y/{path}', 'https://b.example:443/{commit}') })
Check (($two -join ',') -eq 'a.example,b.example') "one host per name, without the port: $($two -join ',')"

$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: git.kernel.org, gitlab.com reachable") $h
Check ($p -and ($p.reachable -join ',') -eq 'git.kernel.org,gitlab.com' -and $p.unreachable -eq '') 'both hosts reachable: [ok], nothing unreachable'
Check ($null -eq (Get-FirmwarePreflightWarning $p)) 'both hosts reachable: no WARN line'

# The r3 cap package dry run of 2026-11-06 (scratch\t38\wddm-b4\capset3\dryrun-lab.run1.log line 568).
$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: git.kernel.org reachable; not reachable: gitlab.com The operation has timed out.") $h
Check ($p -and ($p.reachable -join ',') -eq 'git.kernel.org' -and $p.unreachable -eq 'gitlab.com The operation has timed out.') 'one host reachable (the BD-325 line): [ok], the other host named'
Check ((Get-FirmwarePreflightWarning $p) -eq '  WARN preflight: firmware host not reachable from this computer now: gitlab.com The operation has timed out.') 'one host not reachable: the WARN line names it'
$p = Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: gitlab.com reachable; not reachable: git.kernel.org 503") $h
Check ($p -and ($p.reachable -join ',') -eq 'gitlab.com') 'the other host alone: [ok]'
Check ((Get-FirmwarePreflightWarning $p) -like '*WARN preflight*git.kernel.org 503') 'the other host not reachable: the WARN line names it'

# Negative controls: what the test must still refuse.
Check ($null -eq (Get-FirmwarePreflight (Line 'fail' "no download host answers (git.kernel.org x; gitlab.com y). The installer downloads the GPU firmware") $h)) 'no host reachable ([fail] line): refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'fail' "download from linux-firmware ${c}: git.kernel.org reachable") $h)) 'a [fail] status with a host list: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware 0123abc: git.kernel.org reachable") $h)) 'a commit that is not 40 hex digits: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: example.com reachable") $h)) 'a host outside the installer list: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: gitlab.com, gitlab.com reachable") $h)) 'the same host twice: refused'
Check ($null -eq (Get-FirmwarePreflight 'nothing about firmware here' $h)) 'no firmware line: refused'
Check ($null -eq (Get-FirmwarePreflight (Line 'ok  ' "download from linux-firmware ${c}: git.kernel.org reachable") @())) 'no host list (a manifest without url_templates): refused'
Check ($null -eq (Get-FirmwarePreflightWarning $null)) 'no preflight line: no WARN line'

if ($fail) { "TEST firmware-preflight FAIL $fail"; exit 1 }
'TEST firmware-preflight PASS'
