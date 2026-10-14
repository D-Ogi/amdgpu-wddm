# Host test of the installer half of the CU mode setting (BD-077). Four contracts:
#   1. The installer's -CuMode switch reaches the registry plan as a 'command' write of the value the tester gave.
#   2. An install that does not name -CuMode writes exactly what registry-defaults.json holds for CuMode, and
#      nothing when the table holds no CuMode. An absent value means 24 compute units (driver/kmd/cumode.c).
#   3. docs\testing\INSTALL.md states that same default in a line a reader can act on, so the guide cannot drift
#      away from the table. The expected number comes from the table, not from this file.
#   4. The driver's own fallback to 24 is a driver closure, the way DpmMode 0 is: CuMode 24 next to
#      CuModeClosedReason stays as the driver left it and is reported with its remedy, install.cmd -Repair writes
#      the default again and deletes the record, a 24 the tester chose (no record) stays the tester's, and a value
#      named on the command line takes the record with it. These checks run only while the table's default is 40,
#      because a default equal to the closed value is no closure; under a default of 24 the test proves instead that
#      the closure entry is inert.
#   6. The number of cores a person selected survives an upgrade (owner, 2026-10-14). The release default is the
#      stock 24 again, so the 'update' rule would take a 40 of tester.30 away; CuModeChosen is the record that tells
#      a number a person asked for from a number a release applied, and it has to match the key exactly.
#   5. The control application's Graphics page writes the stock choice out as CuMode = 24 and never deletes the
#      value, so that an install reads it as a choice and not as a key nobody configured; and the release recipe
#      itself refuses a table without a CuMode of 24 or 40.
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
Check ($tableCuMode -eq 24) 'the current release starts with the owner-approved 24 CU default'

"the table holds $(if ($null -eq $tableCuMode) { 'no CuMode default, so the driver runs 24 compute units' } else { "CuMode $tableCuMode" })"

# The plan of an install, from the command line that install.cmd passes to the write decisions. $Restore and
# $After are the two arguments of the preservation path of install.ps1 (around line 1170): $Current is the key as
# it was before the driver package, $After is the key as the package left it, and $Restore names the values
# outside the table that have to go back.
function New-ParameterPlan([hashtable]$Bound, [hashtable]$Current = @{}, [string[]]$Restore = @(), [hashtable]$After = $null, [switch]$Reopen) {
    $inputs = Get-InstallInputs $Bound $null $version
    return , (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $table.legacy_applied.parameters `
        -Current $Current -Explicit $inputs.parameters -Restore $Restore -After $After -Reopen:$Reopen)
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
$kept24=Get-Entry (New-ParameterPlan @{} @{CuMode=24}) 'CuMode'
Check ($kept24.value -eq 24 -and -not $kept24.write) 'an existing stock choice is preserved without a rewrite'

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

# The driver closure of the graphics cores (contract 4). The driver's fallback writes CuModeClosedReason and then
# CuMode 24 (driver/kmd/cumode.c PersistFallback); the installer must not read that 24 as a choice of the tester.
if ($tableCuMode -eq 40) {
    'the driver went back to 24 by itself'
    $closed = @{ CuMode = 24; CuModeClosedReason = 3; CuModeLastReason = 3 }
    $d = Get-Entry (New-ParameterPlan @{} $closed) 'CuMode'
    Check (($d.decision -eq 'driver-closed') -and ([int]$d.value -eq 24) -and -not $d.write -and `
           ($d.closure_record -eq 'CuModeClosedReason') -and ([int]$d.closure_code -eq 3) -and ($d.closure_reason -eq 'unconfirmed')) `
        "CuMode 24 with CuModeClosedReason 3: $($d.decision) ($($d.closure_reason))"
    $text = Format-RegistryPlan (New-ParameterPlan @{} $closed)
    Check (($text -match 'CuMode=24 KEPT \(the driver went back to 24 graphics cores after a start with all 40 that nobody confirmed, CuModeClosedReason 3 unconfirmed; not a setting of the tester; default 40; remedy: install\.cmd -Repair\)')) `
        'the plan text names the closure, its reason code and the remedy, not the tester'

    'a repair reopens it'
    $plan = New-ParameterPlan @{} $closed @('CuModeClosedReason') @{} -Reopen
    $d = Get-Entry $plan 'CuMode'
    Check (($d.decision -eq 'reopened') -and ([int]$d.value -eq 40) -and $d.write -and (@($d.clear) -contains 'CuModeClosedReason')) `
        "a repair writes 40 again and clears the record: $($d.decision) -> $($d.value), clears $(@($d.clear) -join ',')"
    Check ($null -eq (Get-Entry $plan 'CuModeClosedReason')) 'the reopened record is not written back by the preservation path'

    'one start after the fallback'
    # Every later start rewrites CuModeLastReason (here: NONE), and the durable record still names the closure.
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeClosedReason = 3; CuModeLastReason = 0 }) 'CuMode'
    Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'CuModeClosedReason')) `
        "the record outlives the boot that wrote it: $($d.decision)"

    'a 24 the tester chose'
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24 }) 'CuMode'
    Check (($d.decision -eq 'kept') -and ([int]$d.value -eq 24) -and ($null -eq $d.PSObject.Properties['closure'])) `
        "a 24 with no record stays the tester's: $($d.decision)"
    # No legacy record for CuMode: CuModeLastReason 9 (RESTORE_FAILED) is written by a tester's own 24 start whose
    # stock restore did not read back, as well as by a fallback, so it never turns a 24 into a closure.
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeLastReason = 9 }) 'CuMode'
    Check ($d.decision -eq 'kept') "CuModeLastReason 9 alone is no closure: $($d.decision)"
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeClosedReason = 0; CuModeLastReason = 3 }) 'CuMode'
    Check ($d.decision -eq 'kept') "an empty durable record is no closure either: $($d.decision)"

    'a reason the table does not name'
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeClosedReason = 11 }) 'CuMode'
    Check (($d.decision -eq 'driver-closed') -and ($d.closure_reason -eq 'reason 11') -and ($d.closure -eq 'the driver went back to 24 graphics cores itself')) `
        "a later driver's new reason is still a closure: $($d.decision) ($($d.closure_reason))"

    'a record next to a CuMode that still asks for 40'
    # The fallback writes the record first, so a start that died between the two writes leaves this state. The
    # value is not the closed one, so the installer leaves it alone; the next start deletes the record itself.
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 40; CuModeClosedReason = 3 }) 'CuMode'
    Check (($d.decision -eq 'same') -and ([int]$d.value -eq 40)) "a record beside a 40 is no closure: $($d.decision)"

    'an upgrade over the fallback keeps the record for the driver'
    $plan = New-ParameterPlan @{} $closed @('CuModeClosedReason', 'CuModeLastReason') @{}
    $d = Get-Entry $plan 'CuModeClosedReason'
    Check (($null -ne $d) -and ($d.decision -eq 'restored') -and ([int]$d.value -eq 3) -and $d.write) `
        'the record comes back after the driver package has cleared the key, so the next start still sees it'

    'the command line wins over the closure'
    $d = Get-Entry (New-ParameterPlan @{ CuMode = 24 } $closed) 'CuMode'
    Check (($d.decision -eq 'command') -and ([int]$d.value -eq 24)) "-CuMode 24 over a closure: $($d.decision)"
    # The switch is the tester's word about that very value, so the driver's record is stale and goes with the
    # write. Without this, -CuMode 24 over a closure left the record, and the next install reported the tester's
    # own 24 as a step back of the driver with a repair as its remedy (review 2 finding 2).
    Check ((@($d.clear) -contains 'CuModeClosedReason')) "-CuMode 24 over a closure clears the record: clears $(@($d.clear) -join ',')"
    $d40 = Get-Entry (New-ParameterPlan @{ CuMode = 40 } $closed) 'CuMode'
    Check ((@($d40.clear) -contains 'CuModeClosedReason')) '-CuMode 40 over a closure clears the record as well'
    $plan = New-ParameterPlan @{ CuMode = 24 } $closed @('CuModeClosedReason', 'CuModeLastReason') @{}
    $d = Get-Entry $plan 'CuMode'
    Check (($d.decision -eq 'command') -and (@($d.clear) -contains 'CuModeClosedReason')) 'the same through the preservation path'
    Check ($null -eq (Get-Entry $plan 'CuModeClosedReason')) 'the cleared record is not written back by the preservation path'
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeLastReason = 3 }) 'CuMode'
    Check ($d.decision -eq 'kept') "after the switch, the install that follows reads the 24 as the tester's: $($d.decision)"
    $d = Get-Entry (New-ParameterPlan @{ CuMode = 24 } @{ CuMode = 40 }) 'CuMode'
    Check ($null -eq $d.PSObject.Properties['clear']) 'a command line without a closure clears nothing'

    'a 24 the control application wrote'
    # The application writes the stock choice out as CuMode = 24 and takes the record with it, so the plan of the
    # next install reads a choice of the tester and never the key of a machine it never configured (finding 1).
    $d = Get-Entry (New-ParameterPlan @{} @{ CuMode = 24; CuModeLastApplied = 24; CuModeLastReason = 0 }) 'CuMode'
    Check (($d.decision -eq 'kept') -and ([int]$d.value -eq 24) -and -not $d.write) `
        "the 24 of the Graphics page is kept by an install: $($d.decision)"
    $d = Get-Entry (New-ParameterPlan @{} @{ CuModeLastApplied = 24; CuModeLastReason = 0 }) 'CuMode'
    Check (($d.decision -eq 'set') -and ([int]$d.value -eq 40)) `
        "a key with no CuMode is a machine this installer never configured: $($d.decision) $($d.value)"
} else {
    # With the release default back at the stock 24 (owner, 2026-10-14) the driver's own step back to 24 is the
    # number this release installs anyway, so there is nothing for a repair to reopen. The closure entry has to be
    # inert, not merely untested: a 'driver-closed' here would hold the install back from writing its own default.
    "the table's default is not 40, so the CuMode closure is not in force"
    $closed = @{ CuMode = 24; CuModeClosedReason = 3; CuModeLastReason = 3 }
    foreach ($reopen in $false, $true) {
        $d = Get-Entry (New-ParameterPlan @{} $closed -Reopen:$reopen) 'CuMode'
        Check (($d.decision -notin @('driver-closed', 'reopened')) -and ($null -eq $d.PSObject.Properties['closure'])) `
            "CuMode 24 with CuModeClosedReason 3 under a default of $tableCuMode, repair $($reopen): $($d.decision), no closure"
    }
    $d = Get-Entry (New-ParameterPlan @{ CuMode = 40 } $closed) 'CuMode'
    Check (($d.decision -eq 'command') -and ([int]$d.value -eq 40)) "-CuMode 40 over that state still writes 40: $($d.decision)"
}

'a number of cores a person chose stays over an upgrade (owner, 2026-10-14)'
# The release default is 24 now, so the 'update' rule would take a 40 (or a chosen set of cores) away at every
# upgrade from tester.30, whose own default was that 40. CuModeChosen is what tells the two apart: the control
# application and install.cmd -CuMode write it, the driver never does, and it has to match the key exactly.
$t30 = $table.defaults.parameters | ConvertTo-Json -Depth 6 | ConvertFrom-Json
$t30.CuMode = 40
function New-UpgradePlan([hashtable]$Current, [hashtable]$Bound = @{}) {
    $inputs = Get-InstallInputs $Bound $null $version
    return , (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $t30 -Current $Current `
        -Explicit (Get-ExplicitParameters $inputs.parameters $Current))
}
$d = Get-Entry (New-UpgradePlan @{ CuMode = 40 }) 'CuMode'
Check (($d.decision -eq 'update') -and ([int]$d.value -eq $tableCuMode)) "a 40 that tester.30 applied as its default -> $tableCuMode : $($d.decision)"
$d = Get-Entry (New-UpgradePlan @{ CuMode = 40; CuModeChosen = 40 }) 'CuMode'
Check (($d.decision -eq 'chosen') -and ([int]$d.value -eq 40) -and -not $d.write) "a 40 a person chose stays: $($d.decision)"
$d = Get-Entry (New-UpgradePlan @{ CuMode = 40; CuDisableWgp = 0x84218; CuModeChosen = 0x8421828 }) 'CuMode'
Check (($d.decision -eq 'chosen') -and ([int]$d.value -eq 40) -and -not $d.write) "30 of the 40 cores, chosen in the control application: $($d.decision)"
$d = Get-Entry (New-UpgradePlan @{ CuMode = 40; CuDisableWgp = 0x210; CuModeChosen = 0x8421828 }) 'CuMode'
Check ($d.decision -eq 'update') "a record that no longer describes the key is stale: $($d.decision)"
$plan = New-UpgradePlan @{ CuMode = 24; CuDisableWgp = 0x84218 } @{ CuMode = 40 }
Check (((Get-Entry $plan 'CuMode').decision -eq 'command') -and ([int](Get-Entry $plan 'CuModeChosen').value -eq 0x8421828)) `
    "install.cmd -CuMode 40 records the pair the key will hold: 0x$(('{0:X}' -f [int](Get-Entry $plan 'CuModeChosen').value))"

'the control application writes the stock choice out as 24'
# Contract 5 (review 2 finding 1): with CuMode in the defaults table, an absent value is no longer "the default"
# but "write the release default". The Graphics page therefore writes 24 and never deletes the value, or every
# later install would take the cores of a tester who chose the firmware's number back to 40.
$cuApp = Join-Path $Repo 'tools\win\amdgpu_wddm_control\src\CuMode.cs'
$cuReg = Join-Path $Repo 'tools\win\amdgpu_wddm_control\src\CuRegistry.cs'
if ((Test-Path -LiteralPath $cuApp) -and (Test-Path -LiteralPath $cuReg)) {
    $appText = Get-Content -LiteralPath $cuApp -Raw
    $regText = Get-Content -LiteralPath $cuReg -Raw
    Check ($appText.Contains('step("write-24", "CuMode", Stock)')) 'the setter writes CuMode = 24 for the stock choice'
    Check (-not $appText.Contains('"remove-mode"')) 'the setter has no step that deletes CuMode'
    Check ($appText.Contains('step("remove-closed", "CuModeClosedReason", 0)')) "the setter takes the driver's record away with the chosen 24"
    Check ($appText.Contains('step("write-chosen", "CuModeChosen", chosen)')) 'the setter records the number as the choice of a person, so an upgrade keeps it'
    Check ($appText.Contains('step("remove-chosen", "CuModeChosen", 0)')) "'reset to defaults' gives the number back to the installed release"
    Check ($regText.Contains('name != "CuDisableWgp" && name != "CuModeConfirmed" && name != "CuModeClosedReason" && name != "CuModeChosen"')) `
        'the checked registry removes four values and CuMode is not one of them'
    Check ($regText.Contains('(name == "CuMode" && (value == CuMode.Full || value == CuMode.Stock))')) 'the checked registry allows CuMode 24 and 40 only'
    Check ($regText.Contains('(name == "CuModeChosen" && CuMode.EncodedRequest(value))')) 'and CuModeChosen only as an encoded request the driver would take'
} else {
    '  the control application is not in this tree, its source checks are skipped'
}

'the release recipe requires the number in the table'
# BD-077 (review 2 finding 4): the number of graphics cores of an install belongs to a gate the package build runs
# itself, not only to the host tests.
$recipe = Join-Path $Repo 'tools\release\build-release.ps1'
if (Test-Path -LiteralPath $recipe) {
    $recipeText = Get-Content -LiteralPath $recipe -Raw
    Check ($recipeText -match "'DpmMaxMHz',\s*'CuMode'") 'build-release.ps1 requires parameters.CuMode in the table'
    Check ($recipeText.Contains("not 24 or 40")) 'build-release.ps1 refuses a CuMode default that is neither 24 nor 40'
} else {
    '  the release recipe is not in this tree, its checks are skipped'
}

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
$m = [regex]::Match($guideText, 'Graphics cores: the driver uses (all 40|\d+ of the 40|\d+) compute units(?: of the GPU| after a normal install)')
Check $m.Success 'the guide holds the line "Graphics cores: the driver uses <n> of the 40 compute units of the GPU"'
if ($m.Success) {
    $said = $(if ($m.Groups[1].Value -eq 'all 40') { 40 } else { [int]($m.Groups[1].Value -split ' ')[0] })
    Check ($said -eq $expectedCus) "the guide names $said compute units and the table gives $expectedCus"
}
Check ($guideText -match 'install\.cmd -CuMode (24|40)') 'the guide names install.cmd -CuMode as the way to change the number'
Check (($guideText -match 'Standard \(24\)' -and $guideText -match 'All \(40\)') -or
    ($guideText -match 'Nagi Control App offers the same choice on its Graphics page')) 'the guide identifies the equivalent control application choice'
Check ($guideText -match 'docs/design/cu-mode\.md|cu-mode\.md') 'the guide points at the CU mode design document'

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'CU mode installer half: all checks passed'
exit 0
