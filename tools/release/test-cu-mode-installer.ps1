# Host test of the installer half of the CU mode setting (BD-077). Three contracts:
#   1. The installer's -CuMode switch reaches the registry plan as a 'command' write of the value the tester gave.
#   2. An install that does not name -CuMode writes exactly what registry-defaults.json holds for CuMode, and
#      nothing when the table holds no CuMode. An absent value means 24 compute units (driver/kmd/cumode.c).
#   3. docs\testing\INSTALL.md states that same default in a line a reader can act on, so the guide cannot drift
#      away from the table. The expected number comes from the table, not from this file.
# Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-cu-mode-installer.ps1 [-Installer <package>\installer]
# Pure: nothing under HKLM or HKCU is read or written, and no file is changed.
param(
    [string]$Installer = (Join-Path $PSScriptRoot 'installer'),
    [string]$Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Get-Entry($Plan, [string]$Name) { return (@($Plan) | Where-Object { $_.name -eq $Name } | Select-Object -First 1) }

$table = Get-Content -LiteralPath (Join-Path $Installer 'registry-defaults.json') -Raw | ConvertFrom-Json
$version = '0.0.0.0-test'

# What the table says about CuMode, and what the driver then runs. The driver reads an absent CuMode as 24
# (driver/kmd/cumode.c, "CuMode 24 or 40, absent = 24").
$tableCuMode = $null
if ($table.defaults.parameters.PSObject.Properties['CuMode']) { $tableCuMode = [int]$table.defaults.parameters.CuMode }
$expectedCus = 24
if ($null -ne $tableCuMode) { $expectedCus = $tableCuMode }

"the table holds $(if ($null -eq $tableCuMode) { 'no CuMode default, so the driver runs 24 compute units' } else { "CuMode $tableCuMode" })"

# The plan of an install, from the command line that install.cmd passes to the write decisions. $Restore and
# $After are the two arguments of the preservation path of install.ps1 (around line 1170): $Current is the key as
# it was before the driver package, $After is the key as the package left it, and $Restore names the values
# outside the table that have to go back.
function New-ParameterPlan([hashtable]$Bound, [hashtable]$Current = @{}, [string[]]$Restore = @(), [hashtable]$After = $null) {
    $inputs = Get-InstallInputs $Bound $null $version
    return , (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $table.legacy_applied.parameters `
        -Current $Current -Explicit $inputs.parameters -Restore $Restore -After $After)
}

'an install that does not name -CuMode'
$plan = New-ParameterPlan @{}
$e = Get-Entry $plan 'CuMode'
if ($null -eq $tableCuMode) {
    Check ($null -eq $e) 'the plan holds no CuMode entry, so the value stays absent and the driver runs 24'
} else {
    Check (($null -ne $e) -and ($e.decision -eq 'set') -and ([int]$e.value -eq $tableCuMode) -and $e.write) `
        "the plan writes CuMode $tableCuMode as 'set'"
}

'an install that names -CuMode 40'
$plan = New-ParameterPlan @{ CuMode = 40 }
$e = Get-Entry $plan 'CuMode'
Check (($null -ne $e) -and ($e.decision -eq 'command') -and ([int]$e.value -eq 40) -and $e.write) `
    "-CuMode 40 reaches the plan as a 'command' write of 40"

'an install that names -CuMode 24 over a 40 that the tester set before'
$plan = New-ParameterPlan @{ CuMode = 24 } @{ CuMode = 40 }
$e = Get-Entry $plan 'CuMode'
Check (($null -ne $e) -and ($e.decision -eq 'command') -and ([int]$e.value -eq 24) -and $e.write) `
    "-CuMode 24 reaches the plan as a 'command' write of 24"

# The path unit A lost CuMode 40 to (the lab-cu40 regression inside BD-077): the driver package's AddReg clears
# the service key, so the value only survives because the installer snapshots the key first and writes the names
# outside its table back. The scenario therefore goes through -Restore and -After, not through absence alone.
'an upgrade keeps the CU choice of the tester'
$plan = New-ParameterPlan @{} @{ CuMode = 40; CuModeConfirmed = 1 } @('CuMode', 'CuModeConfirmed') @{}
$e = Get-Entry $plan 'CuMode'
if ($null -eq $tableCuMode) {
    Check (($null -ne $e) -and ($e.decision -eq 'restored') -and ([int]$e.value -eq 40) -and $e.write) `
        "a CuMode 40 of the tester comes back as 'restored' after the driver package has cleared the key"
} else {
    Check (($null -ne $e) -and ($e.decision -in @('same', 'kept'))) "a CuMode 40 of the tester is '$($e.decision)'"
}
$e = Get-Entry $plan 'CuModeConfirmed'
Check (($null -ne $e) -and ($e.decision -eq 'restored') -and ([int]$e.value -eq 1) -and $e.write) `
    "the confirmation mark CuModeConfirmed comes back with it, so the next start is not pending"

# -CuMode 0 is the sentinel of install.ps1 for "the switch was not used": it must not reach the plan as a write.
'the -CuMode 0 sentinel'
$plan = New-ParameterPlan @{ CuMode = 0 }
$e = Get-Entry $plan 'CuMode'
Check (($null -eq $e) -or ([int]$e.value -ne 0)) '-CuMode 0 never writes a 0 into the key'

'install.ps1 accepts 0, 24 and 40 only'
$installText = Get-Content -LiteralPath (Join-Path $Installer 'install.ps1') -Raw
Check ($installText -match '\[ValidateSet\(0,\s*24,\s*40\)\]\[int\]\$CuMode') 'the switch is declared with ValidateSet(0, 24, 40)'

'docs\testing\INSTALL.md states the default'
$guide = Join-Path $Repo 'docs\testing\INSTALL.md'
Check (Test-Path -LiteralPath $guide) "the guide is at $guide"
$guideText = Get-Content -LiteralPath $guide -Raw
# The one line the gate reads. It must name the number of compute units of an install that does not use -CuMode.
# Two forms are accepted, so that the sentence stays plain English for either number: "24 of the 40 compute units
# of the GPU" and "all 40 compute units of the GPU".
$m = [regex]::Match($guideText, 'Graphics cores: the driver uses (all 40|\d+ of the 40) compute units of the GPU')
Check $m.Success 'the guide holds the line "Graphics cores: the driver uses <n> of the 40 compute units of the GPU"'
if ($m.Success) {
    $said = $(if ($m.Groups[1].Value -eq 'all 40') { 40 } else { [int]($m.Groups[1].Value -split ' ')[0] })
    Check ($said -eq $expectedCus) "the guide names $said compute units and the table gives $expectedCus"
}
Check ($guideText -match 'install\.cmd -CuMode (24|40)') 'the guide names install.cmd -CuMode as the way to change the number'
Check (($guideText -match 'Standard \(24\)') -and ($guideText -match 'All \(40\)')) 'the guide names both choices of the control application'
Check ($guideText -match 'docs/design/cu-mode\.md|cu-mode\.md') 'the guide points at the CU mode design document'

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'CU mode installer half: all checks passed'
exit 0
