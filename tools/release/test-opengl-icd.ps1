# Host test of the OpenGL ICD registration (BD-076, GitHub issue #8) on a computer WITHOUT a BC-250. Windows
# PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-opengl-icd.ps1 -Installer <package>\installer [-Package <package>]
# Asserts: the release switch of manifest.json (register on and off, the architecture list, the package paths, the
# value type, the version and the flags, and the refusals of a malformed switch); the values the installer writes,
# three per architecture of the list, with their types and the paths under the install root; verify's
# Test-OpenGlRegistration against a complete and an incomplete registration, for a release that registers both
# architectures and for this release, which registers x64 alone; the ICD file check (machine, the Drv entry points, a
# file that is no image); the restore plan of the uninstaller (a value from before the install comes back, a value
# that was not there is removed, a value somebody else changed is kept, and without a record only a value that names
# the install root is ours); the narrowing step of an install whose architecture list is shorter than the one on the
# computer; and that install.ps1 and uninstall.ps1 carry the steps. With -Package the real ICD files of that package
# are checked too.
# The x86 half is tested either way: this release registers x64 alone (common.ps1 holds the lab measurement behind
# that), and a release that adds "x86" to the list changes nothing else, so the cases of both lists run here.
# The registration cases run against a scratch key, HKCU:\Software\amdgpu-wddm-installer-test-gl-<8 hex>, removed at the end,
# and files under the user's temporary folder, removed at the end. Nothing under HKLM and no other file is written.
param([Parameter(Mandatory)][string]$Installer, [string]$Package)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
# A manifest as ConvertFrom-Json gives it, with one opengl_icd switch.
function New-Switch($Table) {
    if ($null -eq $Table) { return (New-Object psobject) }
    $m = New-Object psobject
    $m | Add-Member -NotePropertyName opengl_icd -NotePropertyValue $Table
    return $m
}
function New-Table([hashtable]$Values) {
    $o = New-Object psobject
    foreach ($k in $Values.Keys) { $o | Add-Member -NotePropertyName $k -NotePropertyValue $Values[$k] }
    return $o
}

'the release switch (manifest.json "opengl_icd")'
$off = Get-OpenGlReleaseSwitch -Manifest (New-Switch $null) -PackageRoot $null
Check (-not $off.register) 'a manifest without the switch registers nothing'
$off = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $false })) -PackageRoot $null
Check (-not $off.register) 'register false registers nothing'
$on = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true })) -PackageRoot $null
Check ($on.register -and ($on.package_path -eq 'payload/opengl/amdgpu_wddm_gl.dll') -and ($on.package_path_wow -eq 'payload/wow64/opengl/amdgpu_wddm_gl.dll')) "register true: the default package paths ($($on.package_path), $($on.package_path_wow))"
Check (($on.payload_dir -eq 'opengl') -and ($on.install_path -eq 'opengl\amdgpu_wddm_gl.dll') -and ($on.install_path_wow -eq 'wow64\opengl\amdgpu_wddm_gl.dll')) "the install paths under the install root ($($on.install_path), $($on.install_path_wow))"
Check (($on.value_type -eq 'REG_MULTI_SZ') -and ($on.version -eq 1) -and ($on.flags -eq 1)) "the defaults: $($on.value_type), version $($on.version), flags $($on.flags)"
# The architecture list. A switch that names none gets this release's own answer, x64 alone, and with it the
# installer writes the three x64 values and no Wow value at all. The x86 half of every function stays in use and
# stays tested: a later release that ships a 32-bit Vulkan loader adds "x86" to the list and changes nothing else.
Check ((@($on.architectures) -join ',') -eq 'x64') "a switch without an architecture list registers this release's own list ($(@($on.architectures) -join ', '))"
Check ($on.register_x64 -and (-not $on.register_x86)) 'x64 is registered, x86 is not'
$both = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64', 'x86') })) -PackageRoot $null
Check ((@($both.architectures) -join ',') -eq 'x64,x86') "the list can ask for both architectures ($(@($both.architectures) -join ', '))"
Check ($both.register_x64 -and $both.register_x86) 'with both in the list both are registered'
$x86only = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x86') })) -PackageRoot $null
Check (((@($x86only.architectures) -join ',') -eq 'x86') -and (-not $x86only.register_x64) -and $x86only.register_x86) 'the list can ask for x86 alone'
$rev = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x86', 'x64') })) -PackageRoot $null
Check ((@($rev.architectures) -join ',') -eq 'x64,x86') "the list comes back in the installer's own order, whatever order the package wrote it in ($(@($rev.architectures) -join ', '))"
$sz = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64', 'x86'); value_type = 'REG_SZ'; version = 2; flags = 3 })) -PackageRoot $null
Check (($sz.value_type -eq 'REG_SZ') -and ($sz.version -eq 2) -and ($sz.flags -eq 3)) 'the switch can ask for REG_SZ, another version and other flags'
foreach ($bad in @(
    @{ what = 'a package path outside payload'; t = @{ register = $true; package_path = 'opengl/amdgpu_wddm_gl.dll' } }
    @{ what = 'a 64-bit path in the Wow slot'; t = @{ register = $true; package_path_wow = 'payload/opengl/amdgpu_wddm_gl.dll' } }
    @{ what = 'an unknown value type'; t = @{ register = $true; value_type = 'REG_DWORD' } }
    @{ what = 'version 0'; t = @{ register = $true; version = 0 } }
    @{ what = 'an empty architecture list'; t = @{ register = $true; architectures = @() } }
    @{ what = 'an architecture it does not know'; t = @{ register = $true; architectures = @('x64', 'arm64') } }
    @{ what = 'an architecture named twice'; t = @{ register = $true; architectures = @('x64', 'x64') } })) {
    $threw = $false
    try { [void](Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table $bad.t)) -PackageRoot $null) } catch { $threw = $true }
    Check $threw "the switch refuses $($bad.what)"
}

'the values the installer writes: three per architecture of the list'
$root = 'C:\Program Files\amdgpu-wddm'
$plan = @(Get-OpenGlRegistrationPlan -Switch $on -InstallRoot $root)
Check ((@($plan | ForEach-Object { $_.name }) -join ',') -eq 'OpenGLDriverName,OpenGLVersion,OpenGLFlags') "x64 alone: three values in order ($(@($plan | ForEach-Object { $_.name }) -join ', '))"
Check (@($plan | Where-Object { $_.name -like '*Wow' }).Count -eq 0) 'x64 alone: no Wow value is written, so a 32-bit program keeps the renderer of Windows itself'
Check ((@($plan | Where-Object { $_.name -eq 'OpenGLDriverName' })[0].value -join '|') -eq "$root\opengl\amdgpu_wddm_gl.dll") 'OpenGLDriverName names the x64 ICD under the install root'
Check ((Format-OpenGlRegistrationPlan $plan) -eq "OpenGLDriverName = [$root\opengl\amdgpu_wddm_gl.dll]; OpenGLVersion = 1; OpenGLFlags = 1") "the dry run line of this release names its three values ($(Format-OpenGlRegistrationPlan $plan))"
$planX86 = @(Get-OpenGlRegistrationPlan -Switch $x86only -InstallRoot $root)
Check ((@($planX86 | ForEach-Object { $_.name }) -join ',') -eq 'OpenGLDriverNameWow,OpenGLVersionWow,OpenGLFlagsWow') "x86 alone: the three Wow values and nothing else ($(@($planX86 | ForEach-Object { $_.name }) -join ', '))"
Check ((@($planX86 | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].value -join '|') -eq "$root\wow64\opengl\amdgpu_wddm_gl.dll") 'OpenGLDriverNameWow names the x86 ICD under the install root'
$planBoth = @(Get-OpenGlRegistrationPlan -Switch $both -InstallRoot $root)
Check ((@($planBoth | ForEach-Object { $_.name }) -join ',') -eq 'OpenGLDriverName,OpenGLVersion,OpenGLFlags,OpenGLDriverNameWow,OpenGLVersionWow,OpenGLFlagsWow') "both: six values in order ($(@($planBoth | ForEach-Object { $_.name }) -join ', '))"
Check ((@($planBoth | Where-Object { $_.type -eq 'MultiString' }).Count -eq 2) -and (@($planBoth | Where-Object { $_.type -eq 'DWord' }).Count -eq 4)) 'both: two REG_MULTI_SZ name values and four REG_DWORD numbers'
$planSz = @(Get-OpenGlRegistrationPlan -Switch $sz -InstallRoot $root)
Check (@($planSz | Where-Object { $_.type -eq 'String' }).Count -eq 2) 'with REG_SZ the two name values are single strings'
Check ((Format-OpenGlRegistrationPlan $planBoth) -match 'OpenGLDriverName = \[C:\\Program Files\\amdgpu-wddm\\opengl\\amdgpu_wddm_gl\.dll\]; OpenGLVersion = 1; OpenGLFlags = 1; OpenGLDriverNameWow = ') "the dry run line names every value of a both-architecture release ($(Format-OpenGlRegistrationPlan $planBoth))"
# The files a release has to carry, and the values it has to take away again: one row per architecture of the list.
$fOn = @(Get-OpenGlArchFiles -Switch $on)
Check ((@($fOn).Count -eq 1) -and ($fOn[0].arch -eq 'x64') -and ($fOn[0].machine -eq 0x8664) -and ($fOn[0].package_path -eq $on.package_path)) "x64 alone: one file row, the x64 image ($($fOn[0].package_path), machine 0x$('{0:X}' -f $fOn[0].machine))"
$fBoth = @(Get-OpenGlArchFiles -Switch $both)
Check ((@($fBoth).Count -eq 2) -and ((@($fBoth | ForEach-Object { $_.arch }) -join ',') -eq 'x64,x86') -and ($fBoth[1].machine -eq 0x14C)) 'both: two file rows, x64 then x86'
Check ((@(Get-OpenGlUnregisteredValueNames -Switch $on) -join ',') -eq 'OpenGLDriverNameWow,OpenGLVersionWow,OpenGLFlagsWow') "x64 alone: the three Wow values are the ones an install takes away again ($(@(Get-OpenGlUnregisteredValueNames -Switch $on) -join ', '))"
Check ((@(Get-OpenGlUnregisteredValueNames -Switch $x86only) -join ',') -eq 'OpenGLDriverName,OpenGLVersion,OpenGLFlags') 'x86 alone: the three x64 values are the ones an install takes away again'
Check (@(Get-OpenGlUnregisteredValueNames -Switch $both).Count -eq 0) 'both: nothing to take away'

'the ICD file check (machine, the Drv entry points)'
Check (@($script:OpenGlDrvExports).Count -eq 19) "19 Drv entry points in the contract ($(@($script:OpenGlDrvExports) -join ', '))"
Check (@('DrvCopyContext', 'DrvDescribeLayerPlane', 'DrvPresentBuffers', 'DrvRealizeLayerPalette', 'DrvSwapLayerBuffers', 'DrvGetLayerPaletteEntries', 'DrvSetLayerPaletteEntries' | Where-Object { $script:OpenGlDrvExports -notcontains $_ }).Count -eq 0) 'the contract holds the seven entry points beyond the twelve a context needs, which every shipping ICD exports'
Check ($script:OpenGlDrvExports -notcontains 'DrvSwapMultipleBuffers') 'DrvSwapMultipleBuffers is not required: Windows names it, and the shipping NVIDIA ICD does not export it either'
$work = Join-Path $env:TEMP ('amdgpu-wddm-gl-test-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
# The scratch key has a suffix of its own, so two dry runs at the same time (the two flavours of a package run)
# never write, read back or remove each other's key.
$key = 'HKCU:\Software\amdgpu-wddm-installer-test-gl-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    [void][IO.Directory]::CreateDirectory((Join-Path $work 'opengl'))
    [void][IO.Directory]::CreateDirectory((Join-Path $work 'wow64\opengl'))
    $notImage = Join-Path $work 'opengl\amdgpu_wddm_gl.dll'
    [IO.File]::WriteAllText($notImage, 'not a PE image')
    $p = @(Test-OpenGlIcdFile -Path $notImage -Machine 0x8664)
    Check (($p.Count -eq 1) -and ($p[0] -match 'missing or not an image')) "a file that is no image is a finding ($($p -join '; '))"
    Remove-Item -LiteralPath $notImage -Force
    $p = @(Test-OpenGlIcdFile -Path $notImage -Machine 0x8664)
    Check (($p.Count -eq 1) -and ($p[0] -match 'missing or not an image')) 'a missing file is a finding'
    # A real image that is not an ICD: the installer's own PowerShell host. It exports no Drv entry point, and it is
    # the right machine, so the finding has to be the export list and nothing else.
    $host64 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $p = @(Test-OpenGlIcdFile -Path $host64 -Machine 0x8664)
    Check ((@($p | Where-Object { $_ -match 'does not export DrvCopyContext' }).Count -eq 1) -and (@($script:OpenGlDrvExports | Where-Object { $p[0] -notmatch $_ }).Count -eq 0)) "an image without the Drv entry points is a finding that names all 19 ($($p -join '; '))"
    $p = @(Test-OpenGlIcdFile -Path $host64 -Machine 0x14C)
    Check (@($p | Where-Object { $_ -match 'is machine 0x8664, expected 0x14C' }).Count -eq 1) "the wrong machine is a finding ($($p -join '; '))"

    'the payload a release may carry (Get-OpenGlPayloadProblems)'
    # One rule for build-release.ps1 and for the package gates: the file of every architecture of the list, and no
    # file of an architecture outside it. The second half is not symmetry: the installer copies the wow64 tree whole,
    # so a carried file of an unregistered architecture is installed and deleted again in one run while manifest.json
    # carries a component row naming it. Two fake package roots, files only (the switch probes paths, not images).
    $pkgBoth = Join-Path $work 'pkg-both'
    $pkgX64 = Join-Path $work 'pkg-x64'
    foreach ($r in @($pkgBoth, $pkgX64)) { [void][IO.Directory]::CreateDirectory((Join-Path $r 'payload\opengl')); [IO.File]::WriteAllText((Join-Path $r 'payload\opengl\amdgpu_wddm_gl.dll'), 'x') }
    [void][IO.Directory]::CreateDirectory((Join-Path $pkgBoth 'payload\wow64\opengl'))
    [IO.File]::WriteAllText((Join-Path $pkgBoth 'payload\wow64\opengl\amdgpu_wddm_gl.dll'), 'x')
    $swX64inX64 = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64') })) -PackageRoot $pkgX64
    Check ((@(Get-OpenGlPayloadProblems -Switch $swX64inX64)).Count -eq 0) 'x64 registered and the x64 file carried: the payload matches the list'
    $swBothinBoth = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64', 'x86') })) -PackageRoot $pkgBoth
    Check ((@(Get-OpenGlPayloadProblems -Switch $swBothinBoth)).Count -eq 0) 'both registered and both files carried: the payload matches the list'
    # The negative controls, one for each half of the rule.
    $swBothinX64 = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64', 'x86') })) -PackageRoot $pkgX64
    $pb = @(Get-OpenGlPayloadProblems -Switch $swBothinX64)
    Check (($pb.Count -eq 1) -and ($pb[0] -match 'the package has no payload/wow64/opengl/amdgpu_wddm_gl\.dll') -and ($pb[0] -match 'drop that architecture from opengl_icd\.architectures')) "a list that names x86 without the x86 file is a finding ($($pb -join '; '))"
    $swX64inBoth = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x64') })) -PackageRoot $pkgBoth
    $pb = @(Get-OpenGlPayloadProblems -Switch $swX64inBoth)
    Check (($pb.Count -eq 1) -and ($pb[0] -match 'carries payload/wow64/opengl/amdgpu_wddm_gl\.dll') -and ($pb[0] -match "add 'x86' to that list, or drop the files row")) "a package that carries an x86 ICD it does not register is a finding, and the finding names both ways out ($($pb -join '; '))"
    $swX86inX64 = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; architectures = @('x86') })) -PackageRoot $pkgX64
    $pb = @(Get-OpenGlPayloadProblems -Switch $swX86inX64)
    Check (($pb.Count -eq 2) -and (@($pb | Where-Object { $_ -match 'has no payload/wow64/opengl' }).Count -eq 1) -and (@($pb | Where-Object { $_ -match "carries payload/opengl/amdgpu_wddm_gl\.dll" }).Count -eq 1)) "both halves of the rule are reported together ($($pb -join '; '))"
    # The bisect lever (register false) is held to neither rule: it registers nothing, takes an earlier install away,
    # and its files rows stay as they are.
    $swOffBoth = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $false; architectures = @('x64') })) -PackageRoot $pkgBoth
    Check ((@(Get-OpenGlPayloadProblems -Switch $swOffBoth)).Count -eq 0) 'register false: a package that carries both files is no finding'

    'verify: Test-OpenGlRegistration against a scratch key'
    # The six-value scenario first, against a switch that lists both architectures, so that the x86 half of verify
    # stays measured while this release registers x64 alone. The x64-only scenario of this release follows it.
    [void](New-Item -Path "$key\class" -Force)
    $args0 = @{ InstallRoot = $work; ClassKey = "$key\class"; Switch = $both }
    $p = @(Test-OpenGlRegistration @args0)
    Check ((@($p | Where-Object { $_ -match ' is missing$' }).Count -eq 6) -and (@($p | Where-Object { $_ -match 'missing or not an image' }).Count -eq 2)) "nothing registered: six values missing and both files missing ($($p.Count) findings)"
    $workPlan = @(Get-OpenGlRegistrationPlan -Switch $both -InstallRoot $work)
    Write-OpenGlRegistration -ClassKey "$key\class" -Plan $workPlan
    # The two ICD files: a copy of a real x64 and x86 image each. They are no ICD, so the file findings stay two; the
    # value findings have to be gone.
    Copy-Item -LiteralPath $host64 -Destination (Join-Path $work 'opengl\amdgpu_wddm_gl.dll')
    Copy-Item -LiteralPath (Join-Path $env:windir 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe') -Destination (Join-Path $work 'wow64\opengl\amdgpu_wddm_gl.dll')
    $p = @(Test-OpenGlRegistration @args0)
    Check ((@($p | Where-Object { $_ -match '^OpenGL' }).Count -eq 0) -and (@($p | Where-Object { $_ -match 'does not export' }).Count -eq 2)) "a complete registration leaves no registry finding ($($p -join '; '))"
    $back = (Get-ItemProperty -LiteralPath "$key\class")
    Check ((@($back.OpenGLDriverName) -join '|') -eq (Join-Path $work 'opengl\amdgpu_wddm_gl.dll')) 'OpenGLDriverName survives the registry round trip'
    Check (($back.OpenGLVersion -eq 1) -and ($back.OpenGLFlags -eq 1) -and ($back.OpenGLVersionWow -eq 1) -and ($back.OpenGLFlagsWow -eq 1)) 'the four numbers read back as 1'
    Check ((Get-Item -LiteralPath "$key\class").GetValueKind('OpenGLDriverName') -eq 'MultiString') 'OpenGLDriverName is a REG_MULTI_SZ'
    Check ((Get-Item -LiteralPath "$key\class").GetValueKind('OpenGLVersion') -eq 'DWord') 'OpenGLVersion is a REG_DWORD'
    Set-ItemProperty -LiteralPath "$key\class" -Name OpenGLFlagsWow -Value 2
    $p = @(Test-OpenGlRegistration @args0)
    Check (@($p | Where-Object { $_ -match '^OpenGLFlagsWow is 2, expected 1' }).Count -eq 1) "a changed flag value is a finding ($($p -join '; '))"
    Remove-ItemProperty -LiteralPath "$key\class" -Name OpenGLDriverNameWow
    $p = @(Test-OpenGlRegistration @args0)
    Check (@($p | Where-Object { $_ -eq 'OpenGLDriverNameWow is missing' }).Count -eq 1) 'a missing Wow name is a finding: a 32-bit OpenGL application would get no driver'

    'verify: this release registers x64 alone'
    # What verify reports on a tester's computer for this release: the three x64 values and the x64 file, and nothing
    # about the 32-bit half. The negative controls are in the same block: a 32-bit value that belongs to somebody
    # else is not a finding, and a missing 32-bit file is not one either, because no value of ours names it.
    [void](New-Item -Path "$key\x64" -Force)
    $args64 = @{ InstallRoot = $work; ClassKey = "$key\x64"; Switch = $on }
    $p = @(Test-OpenGlRegistration @args64)
    Check ((@($p | Where-Object { $_ -match ' is missing$' }).Count -eq 3) -and (@($p | Where-Object { $_ -match 'missing or not an image' }).Count -eq 0)) "nothing registered: the three x64 values are missing and no Wow value is asked for ($($p -join '; '))"
    Check (@($p | Where-Object { $_ -match 'Wow' }).Count -eq 0) 'no finding names a Wow value'
    $plan64 = @(Get-OpenGlRegistrationPlan -Switch $on -InstallRoot $work)
    Write-OpenGlRegistration -ClassKey "$key\x64" -Plan $plan64
    $p = @(Test-OpenGlRegistration @args64)
    Check ((@($p | Where-Object { $_ -match '^OpenGL' }).Count -eq 0) -and (@($p | Where-Object { $_ -match 'does not export' }).Count -eq 1)) "a complete x64 registration leaves no registry finding, and only the x64 file is read ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\x64" -Name OpenGLDriverNameWow -Value ([string[]]@('C:\Windows\System32\foreign.dll')) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-OpenGlRegistration @args64)
    Check (@($p | Where-Object { $_ -match 'Wow' }).Count -eq 0) "another vendor's 32-bit registration on the same key is no finding of ours ($($p -join '; '))"
    Remove-Item -LiteralPath (Join-Path $work 'wow64\opengl\amdgpu_wddm_gl.dll') -Force
    $p = @(Test-OpenGlRegistration @args64)
    Check (@($p | Where-Object { $_ -match 'wow64' }).Count -eq 0) 'a missing 32-bit ICD file is no finding: this release names none'
    Copy-Item -LiteralPath (Join-Path $env:windir 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe') -Destination (Join-Path $work 'wow64\opengl\amdgpu_wddm_gl.dll')

    'the restore plan of the uninstaller'
    $prev = @(
        [pscustomobject]@{ name = 'OpenGLDriverName'; present = $true; previous = @('C:\Windows\System32\otherdriver.dll') }
        [pscustomobject]@{ name = 'OpenGLVersion'; present = $true; previous = 7 }
        [pscustomobject]@{ name = 'OpenGLFlags'; present = $false; previous = $null }
        [pscustomobject]@{ name = 'OpenGLDriverNameWow'; present = $false; previous = $null }
        [pscustomobject]@{ name = 'OpenGLVersionWow'; present = $false; previous = $null }
        [pscustomobject]@{ name = 'OpenGLFlagsWow'; present = $false; previous = $null })
    $wrote = @($workPlan | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } })
    [void](New-Item -Path "$key\restore" -Force)
    Write-OpenGlRegistration -ClassKey "$key\restore" -Plan $workPlan
    $now = Read-RegistryValues "$key\restore"
    $rp = @(Get-OpenGlRestorePlan -Previous $prev -Wrote $wrote -Current $now -InstallRoot $work)
    Check ((@($rp | Where-Object { $_.name -eq 'OpenGLDriverName' })[0].action -eq 'restore') -and (@($rp | Where-Object { $_.name -eq 'OpenGLVersion' })[0].action -eq 'restore')) 'a value that was there before the install is restored'
    Check ((@($rp | Where-Object { $_.name -eq 'OpenGLFlags' })[0].action -eq 'remove') -and (@($rp | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].action -eq 'remove')) 'a value that was not there before the install is removed'
    Write-OpenGlRestore -ClassKey "$key\restore" -Plan $rp
    $after = Read-RegistryValues "$key\restore"
    Check ((@($after['OpenGLDriverName']) -join '|') -eq 'C:\Windows\System32\otherdriver.dll') 'the old OpenGLDriverName is back'
    Check ($after['OpenGLVersion'] -eq 7) 'the old OpenGLVersion is back'
    Check ((-not $after.ContainsKey('OpenGLFlags')) -and (-not $after.ContainsKey('OpenGLDriverNameWow')) -and (-not $after.ContainsKey('OpenGLVersionWow')) -and (-not $after.ContainsKey('OpenGLFlagsWow'))) 'the four values this release added are gone'
    # A value somebody else changed after the install belongs to whoever wrote it.
    Write-OpenGlRegistration -ClassKey "$key\restore" -Plan $workPlan
    Set-ItemProperty -LiteralPath "$key\restore" -Name OpenGLFlags -Value 3
    $rp = @(Get-OpenGlRestorePlan -Previous $prev -Wrote $wrote -Current (Read-RegistryValues "$key\restore") -InstallRoot $work)
    Check ((@($rp | Where-Object { $_.name -eq 'OpenGLFlags' })[0].action -eq 'keep')) 'a value that is not the one this release wrote is kept'
    # No record at all (an older release, or a state file that is gone): a name value that names the install root is
    # ours, anything else is kept.
    $noRecord = @{ OpenGLDriverName = @((Join-Path $work 'opengl\amdgpu_wddm_gl.dll')); OpenGLDriverNameWow = @('C:\Windows\System32\foreign.dll') }
    $rp = @(Get-OpenGlRestorePlan -Previous @() -Wrote @() -Current $noRecord -InstallRoot $work)
    Check ((@($rp | Where-Object { $_.name -eq 'OpenGLDriverName' })[0].action -eq 'remove') -and (@($rp | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].action -eq 'keep')) 'without a record only a value that names the install root is ours'
    Check ((@($rp | Where-Object { $_.action -eq 'none' }).Count -eq 4)) 'a value that is not there needs no change'
    Check ((Format-OpenGlRestorePlan @(Get-OpenGlRestorePlan -Previous @() -Wrote @() -Current @{} -InstallRoot $work)) -eq 'nothing of ours') 'an empty key: nothing of ours'
    # Without a record the four numbers go with the name value of their own half, so an uninstall is symmetrical with
    # the install: all six values of our own registration go, not the two name values alone. A number alone is kept.
    $noRecord2 = @{ OpenGLDriverName = @((Join-Path $work 'opengl\amdgpu_wddm_gl.dll')); OpenGLVersion = 1; OpenGLFlags = 1
        OpenGLDriverNameWow = @((Join-Path $work 'wow64\opengl\amdgpu_wddm_gl.dll')); OpenGLVersionWow = 1; OpenGLFlagsWow = 1 }
    $rp2 = @(Get-OpenGlRestorePlan -Previous @() -Wrote @() -Current $noRecord2 -InstallRoot $work)
    Check ((@($rp2 | Where-Object { $_.action -eq 'remove' }).Count -eq 6)) "without a record, all six values of our own registration go ($(@($rp2 | ForEach-Object { "$($_.name)=$($_.action)" }) -join ' '))"
    $noRecord3 = @{ OpenGLDriverName = @('C:\Windows\System32\foreign.dll'); OpenGLVersion = 1; OpenGLFlags = 3 }
    $rp3 = @(Get-OpenGlRestorePlan -Previous @() -Wrote @() -Current $noRecord3 -InstallRoot $work)
    Check ((@($rp3 | Where-Object { $_.action -eq 'keep' }).Count -eq 3)) "a foreign name value keeps the numbers of its own half ($(@($rp3 | Where-Object { $_.action -ne 'none' } | ForEach-Object { "$($_.name)=$($_.action)" }) -join ' '))"
    # The line the operator and the evidence file read. Inside a switch statement $_ is the switch's own input, so a
    # formatter that reads $_.name there prints a line with no value name in it at all.
    $fmt = Format-OpenGlRestorePlan @([pscustomobject]@{ name = 'OpenGLDriverName'; action = 'restore'; value = @('C:\Windows\System32\otherdriver.dll'); kind = 'MultiString'; why = 'the value from before the install' },
        [pscustomobject]@{ name = 'OpenGLVersion'; action = 'restore'; value = 7; kind = 'DWord'; why = 'the value from before the install' },
        [pscustomobject]@{ name = 'OpenGLFlags'; action = 'remove'; value = $null; kind = $null; why = 'there was none before the install' },
        [pscustomobject]@{ name = 'OpenGLDriverNameWow'; action = 'keep'; value = 'foreign.dll'; kind = $null; why = 'no record of this release writing it' },
        [pscustomobject]@{ name = 'OpenGLVersionWow'; action = 'none'; value = $null; kind = $null; why = 'absent' })
    Check ($fmt -eq 'OpenGLDriverName back to [C:\Windows\System32\otherdriver.dll] (MultiString); OpenGLVersion back to 7 (DWord); remove OpenGLFlags; OpenGLDriverNameWow kept (no record of this release writing it)') "the restore line names every value, its new value and its type ($fmt)"
    Check ((@('OpenGLDriverName', 'OpenGLVersion', 'OpenGLFlags', 'OpenGLDriverNameWow' | Where-Object { $fmt -match [regex]::Escape($_) }).Count -eq 4) -and ($fmt -notmatch 'OpenGLVersionWow')) 'the restore line leaves out the values it does not touch'

    'an install that narrows the architecture list takes the other half away again'
    # The case a release like this one has to survive: a computer where a release of ours registered both
    # architectures, and this release registers x64 alone. The three x64 values are written again, and the three Wow
    # values of the earlier install go, with the value from before that install coming back where there was one.
    # Without this the Wow registration would name a file this package does not carry, and a 32-bit OpenGL program
    # would get no driver at all instead of the renderer of Windows itself.
    [void](New-Item -Path "$key\narrow" -Force)
    New-ItemProperty -LiteralPath "$key\narrow" -Name OpenGLDriverNameWow -Value ([string[]]@('C:\Windows\System32\otherdriver.dll')) -PropertyType MultiString -Force | Out-Null
    # The record is taken with the install root, as install.ps1 takes it: the 32-bit value on this key is another
    # driver's, so it does not name our install root and stays in the record, and the step below puts it back.
    $prevNarrow = @(Get-OpenGlValueRecord -ClassKey "$key\narrow" -InstallRoot $work)
    Check ((@($prevNarrow | Where-Object { $_.present }).Count -eq 1) -and ((@($prevNarrow | Where-Object { $_.present })[0].name) -eq 'OpenGLDriverNameWow')) 'the record of this key holds the one value that was there, which is not ours'
    Write-OpenGlRegistration -ClassKey "$key\narrow" -Plan $workPlan          # the earlier release: both architectures
    $wroteNarrow = @($workPlan | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } })
    $offNames = @(Get-OpenGlUnregisteredValueNames -Switch $on)
    $rpN = @(Get-OpenGlRestorePlan -Previous $prevNarrow -Wrote $wroteNarrow -Current (Read-RegistryValues "$key\narrow") -InstallRoot $work -Names $offNames)
    Check ((@($rpN).Count -eq 3) -and (@($rpN | Where-Object { $_.name -like '*Wow' }).Count -eq 3)) "the plan covers the three Wow values and nothing else ($(@($rpN | ForEach-Object { $_.name }) -join ', '))"
    Check (((@($rpN | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].action) -eq 'restore') -and ((@($rpN | Where-Object { $_.name -eq 'OpenGLVersionWow' })[0].action) -eq 'remove')) "the 32-bit name value from before the install comes back, the numbers go ($(@($rpN | ForEach-Object { "$($_.name)=$($_.action)" }) -join ' '))"
    Write-OpenGlRestore -ClassKey "$key\narrow" -Plan $rpN
    $afterN = Read-RegistryValues "$key\narrow"
    Check ((@($afterN['OpenGLDriverNameWow']) -join '|') -eq 'C:\Windows\System32\otherdriver.dll') 'the 32-bit registration from before the install is back on the key'
    Check ((-not $afterN.ContainsKey('OpenGLVersionWow')) -and (-not $afterN.ContainsKey('OpenGLFlagsWow'))) 'the 32-bit numbers of the earlier install are gone'
    Check (((@($afterN['OpenGLDriverName']) -join '|') -eq (Join-Path $work 'opengl\amdgpu_wddm_gl.dll')) -and ($afterN['OpenGLVersion'] -eq 1) -and ($afterN['OpenGLFlags'] -eq 1)) 'the three x64 values of this release are untouched by that step'
    # The negative control: the same narrowing on a key where the 32-bit registration is somebody else's leaves it.
    [void](New-Item -Path "$key\narrow-foreign" -Force)
    New-ItemProperty -LiteralPath "$key\narrow-foreign" -Name OpenGLDriverNameWow -Value ([string[]]@('C:\Windows\System32\foreign.dll')) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\narrow-foreign" -Name OpenGLVersionWow -Value 1 -PropertyType DWord -Force | Out-Null
    $rpF = @(Get-OpenGlRestorePlan -Previous @() -Wrote @() -Current (Read-RegistryValues "$key\narrow-foreign") -InstallRoot $work -Names $offNames)
    Check (@($rpF | Where-Object { $_.action -eq 'keep' }).Count -eq 2) "a 32-bit registration that is not ours is kept by the narrowing step ($(Format-OpenGlRestorePlan $rpF))"
    # And on a computer that never had a 32-bit registration of ours the step has nothing to do.
    [void](New-Item -Path "$key\narrow-clean" -Force)
    Write-OpenGlRegistration -ClassKey "$key\narrow-clean" -Plan $plan64
    $rpC = @(Get-OpenGlRestorePlan -Previous @() -Wrote @($plan64 | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } }) -Current (Read-RegistryValues "$key\narrow-clean") -InstallRoot $work -Names $offNames)
    Check (@($rpC | Where-Object { $_.action -ne 'none' }).Count -eq 0) "a fresh install of this release has nothing to take away ($(Format-OpenGlRestorePlan $rpC))"

    'the restore keeps the registry type of each value'
    # Another vendor's registration, or a repair tool's, may use any registry type. A restore that guesses the type
    # from the .NET type of the recorded value turns a REG_EXPAND_SZ path into a literal REG_SZ one, four bytes into
    # four strings, and a REG_QWORD above 2^31 into an error.
    [void](New-Item -Path "$key\types" -Force)
    New-ItemProperty -LiteralPath "$key\types" -Name OpenGLDriverName -Value 'atio6axx.dll' -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\types" -Name OpenGLDriverNameWow -Value '%SystemRoot%\System32\x.dll' -PropertyType ExpandString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\types" -Name OpenGLVersion -Value ([byte[]]@(1, 0, 0, 0)) -PropertyType Binary -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\types" -Name OpenGLFlags -Value ([long]4294967299) -PropertyType QWord -Force | Out-Null
    $kinds0 = @{}
    foreach ($n in 'OpenGLDriverName', 'OpenGLDriverNameWow', 'OpenGLVersion', 'OpenGLFlags') { $kinds0[$n] = [string](Get-Item -LiteralPath "$key\types").GetValueKind($n) }
    $rec = @(Get-OpenGlValueRecord -ClassKey "$key\types")
    Check ((@($rec | Where-Object { $_.present -and $_.kind }).Count -eq 4) -and ((@($rec | Where-Object { $_.name -eq 'OpenGLFlags' })[0].kind) -eq 'QWord')) "the record of a key carries the registry type of each value ($(@($rec | Where-Object { $_.present } | ForEach-Object { "$($_.name)=$($_.kind)" }) -join ' '))"
    # The record survives the installer state file, which is JSON.
    $recJson = @((($rec | ConvertTo-Json -Depth 6) | ConvertFrom-Json))
    Write-OpenGlRegistration -ClassKey "$key\types" -Plan $workPlan
    $rpT = @(Get-OpenGlRestorePlan -Previous $recJson -Wrote @($workPlan | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } }) -Current (Read-RegistryValues "$key\types") -InstallRoot $work)
    Write-OpenGlRestore -ClassKey "$key\types" -Plan $rpT
    $kinds1 = @{}
    foreach ($n in 'OpenGLDriverName', 'OpenGLDriverNameWow', 'OpenGLVersion', 'OpenGLFlags') { $kinds1[$n] = [string](Get-Item -LiteralPath "$key\types").GetValueKind($n) }
    Check ((@('OpenGLDriverName', 'OpenGLDriverNameWow', 'OpenGLVersion', 'OpenGLFlags' | Where-Object { $kinds1[$_] -ne $kinds0[$_] }).Count -eq 0)) "every value comes back with the type it had ($(@($kinds1.Keys | Sort-Object | ForEach-Object { "$_=$($kinds1[$_])" }) -join ' '))"
    Check (((Get-ItemProperty -LiteralPath "$key\types" -Name OpenGLDriverNameWow).OpenGLDriverNameWow -ne '%SystemRoot%\System32\x.dll') -and ($kinds1['OpenGLDriverNameWow'] -eq 'ExpandString')) 'the expandable path is expandable again, not a literal REG_SZ'
    Check (([long](Get-ItemProperty -LiteralPath "$key\types" -Name OpenGLFlags).OpenGLFlags) -eq 4294967299) 'a REG_QWORD above 2^31 comes back whole, instead of throwing in the uninstall step'
    Check (((Get-ItemProperty -LiteralPath "$key\types" -Name OpenGLVersion).OpenGLVersion -join ',') -eq '1,0,0,0') 'the four bytes are four bytes, not four strings'

    'the record of the values from before the install leaves out a registration of ours'
    # A record is "the values from before the first install of ours", so a value of ours can never be in it. The test
    # is the install root the name value carries, the same one the restore plan uses without a record. Without this a
    # computer whose installer state is gone - a deleted C:\ProgramData\amdgpu-wddm\installer, a repair after a state
    # reset - would hand the narrowing step and the uninstaller our own earlier registration as "the value from
    # before the install", they would put it back, and the file it names goes in the same run: a program of that
    # architecture would then get no OpenGL driver and not even the software renderer of Windows (BD-076 review).
    [void](New-Item -Path "$key\record" -Force)
    Write-OpenGlRegistration -ClassKey "$key\record" -Plan $workPlan      # a release of ours that registered both
    $recOurs = @(Get-OpenGlValueRecord -ClassKey "$key\record" -InstallRoot $work)
    Check ((@($recOurs | Where-Object { $_.present }).Count -eq 0) -and (@($recOurs | Where-Object { $_.ours }).Count -eq 6)) "a registration of ours is no value from before the install ($(@($recOurs | ForEach-Object { "$($_.name)=$(if ($_.present) { 'present' } else { 'ours' })" }) -join ' '))"
    # Without the install root the same key is recorded as it stands: a caller that only reads the key gets every
    # value, which is what the record did before this test existed.
    Check ((@(Get-OpenGlValueRecord -ClassKey "$key\record" | Where-Object { $_.present }).Count -eq 6)) 'without an install root every value of the key is recorded as it stands'
    # One half ours, the other half somebody else's: the foreign half stays in the record, with its registry type, so
    # the uninstaller still puts it back.
    [void](New-Item -Path "$key\record-mixed" -Force)
    Write-OpenGlRegistration -ClassKey "$key\record-mixed" -Plan $plan64
    New-ItemProperty -LiteralPath "$key\record-mixed" -Name OpenGLDriverNameWow -Value '%SystemRoot%\System32\foreign.dll' -PropertyType ExpandString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\record-mixed" -Name OpenGLVersionWow -Value 7 -PropertyType DWord -Force | Out-Null
    $recMixed = @(Get-OpenGlValueRecord -ClassKey "$key\record-mixed" -InstallRoot $work)
    Check ((@($recMixed | Where-Object { $_.present }).Count -eq 2) -and ((@($recMixed | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].kind) -eq 'ExpandString') -and (@($recMixed | Where-Object { ($_.name -like 'OpenGL*Wow') -and $_.ours }).Count -eq 0)) "a foreign half stays in the record with its type, our own half does not ($(@($recMixed | Where-Object { $_.present } | ForEach-Object { "$($_.name)=$($_.kind)" }) -join ' '))"
    Check ((@($recMixed | Where-Object { ($_.name -notlike '*Wow') -and $_.ours }).Count -eq 3)) 'the two numbers of our own half go with their name value'
    # The scenario end to end: our own x64 and x86 registration on the key, no record of what wrote it, this
    # x64-only release installed over it. The three Wow values have to go, not come back.
    $recLost = @(Get-OpenGlValueRecord -ClassKey "$key\record" -InstallRoot $work)
    $rpLost = @(Get-OpenGlRestorePlan -Previous $recLost -Wrote @() -Current (Read-RegistryValues "$key\record") -InstallRoot $work -Names $offNames)
    Check ((@($rpLost | Where-Object { $_.action -eq 'remove' }).Count -eq 3) -and (@($rpLost | Where-Object { $_.action -eq 'restore' }).Count -eq 0)) "with the state file gone the 32-bit registration of ours is removed, never restored ($(Format-OpenGlRestorePlan $rpLost))"
    Write-OpenGlRestore -ClassKey "$key\record" -Plan $rpLost
    $afterLost = Read-RegistryValues "$key\record"
    Check ((@($script:OpenGlValueNames | Where-Object { ($_ -like '*Wow') -and $afterLost.ContainsKey($_) }).Count -eq 0)) 'no 32-bit value is left naming the file that run deletes'
    Check ((@($afterLost['OpenGLDriverName']) -join '|') -eq (Join-Path $work 'opengl\amdgpu_wddm_gl.dll')) 'the x64 registration is untouched by that step'
    # And the uninstaller of that install takes its own three x64 values away instead of restoring them.
    $rpLostUn = @(Get-OpenGlRestorePlan -Previous $recLost -Wrote @($plan64 | ForEach-Object { [pscustomobject]@{ name = $_.name; value = $_.value } }) -Current $afterLost -InstallRoot $work)
    Check ((@($rpLostUn | Where-Object { $_.action -eq 'remove' }).Count -eq 3) -and (@($rpLostUn | Where-Object { $_.action -eq 'none' }).Count -eq 3) -and (@($rpLostUn | Where-Object { $_.action -eq 'restore' }).Count -eq 0)) "the uninstaller of that install removes its own three values ($(Format-OpenGlRestorePlan $rpLostUn))"

    'the display class keys the uninstaller works from'
    # The uninstaller finds the registration by what a key holds, not by the device: the GPU can be missing, disabled
    # or doubled, and the files go whatever the device looks like.
    [void](New-Item -Path "$key\empty" -Force)
    Check ((@(Get-OpenGlRegisteredClassKeys -PreferredKey "$key\types") | Where-Object { $_ -eq "$key\types" }).Count -eq 1) 'a key that carries an OpenGL value is in the list'
    Check ((@(Get-OpenGlRegisteredClassKeys -PreferredKey "$key\empty") | Where-Object { $_ -eq "$key\empty" }).Count -eq 0) 'a key that carries none is not'

    'the action of a run'
    Check ((Get-OpenGlAction -Switch $on) -eq 'register') 'register true: register'
    Check ((Get-OpenGlAction -Switch $off -PresentValues @() -DirPresent $false) -eq 'none') 'register false and nothing there: none'
    Check ((Get-OpenGlAction -Switch $off -PresentValues @('OpenGLDriverName')) -eq 'rollback') 'register false and a value of an earlier install: rollback'
    Check ((Get-OpenGlAction -Switch $off -DirPresent $true) -eq 'rollback') 'register false and the files of an earlier install: rollback'
} finally {
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
Check ((-not (Test-Path -LiteralPath $key)) -and (-not (Test-Path -LiteralPath $work))) 'scratch key and folder removed'

'install and uninstall sources'
$src = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
$un = [IO.File]::ReadAllText((Join-Path $Installer 'uninstall.ps1'))
Check ($src -match 'Get-OpenGlReleaseSwitch -Manifest \$script:Manifest -PackageRoot \$package') 'install.ps1 takes the switch from the manifest of the package'
Check ($src -match 'throw "manifest\.json registers the OpenGL ICD for \$\(@\(\$gl\.architectures\) -join '' and ''\), but this package has no') 'install.ps1 refuses a package that registers an architecture without carrying its file'
Check ($src -match 'Get-OpenGlArchFiles -Switch \$gl\) \| Where-Object \{ -not \$_\.present \}') 'the refusal walks the architecture list, so a package that lists x86 without the x86 file is refused'
Check ($src -match 'if \(\$gl\.register_x64 -and -not \$driverStoreUmd\) \{ \$dirs \+= \$gl\.payload_dir \}') 'only a legacy x64 OpenGL package copies its ICD to Program Files'
Check (($src -match 'Get-OpenGlUnregisteredValueNames -Switch \$gl') -and ($src -match '-Names \$glOff')) 'install.ps1 takes away the values of an architecture it does not register'
# The record of the release installed here, read before this run replaces it: it is the only exact answer to
# whether the values of the other architecture are ours.
$posWroteRead = $src.IndexOf('$glWroteBefore = @()')
$posWroteSet = $src.IndexOf("Set-StateValue `$state 'opengl_icd'", $posWroteRead)
Check (($posWroteRead -gt 0) -and ($posWroteRead -lt $posWroteSet) -and ($src -match '-Wrote \$glWroteBefore')) 'that step reads the earlier record before this run replaces it'
Check ($src -match 'Remove-PathOrSchedule \$glDirWow') 'install.ps1 removes the 32-bit ICD of an earlier install when it does not register x86'
Check ($src -match "Add-Info 'OpenGL ICD \(32-bit\)'") 'verify says in one line that 32-bit OpenGL programs keep the renderer of Windows itself'
Check (($src -match 'Set-StateValueOnce \$state ''opengl_icd_previous''') -and ($src -match 'Set-StateValue \$state ''opengl_icd''')) 'install.ps1 records the values from before the install and the values it writes'
Check ($src -match 'Get-OpenGlValueRecord -ClassKey \$classKey -InstallRoot \$InstallRoot') 'that record is taken with the install root, so a registration of ours is never "the value from before the install"'
Check ($src -match 'Write-OpenGlRegistration -ClassKey \$classKey -Plan \$glPlan') 'install.ps1 writes the registration into the GPU software key'
Check (($src -match 'Write-OpenGlRestore -ClassKey \$classKey -Plan \$glRestore') -and ($src -match 'Remove-PathOrSchedule \$glDir')) 'install.ps1 rolls the registration and the files back when this release does not register the ICD'
$posKeys = $src.IndexOf('Write-OpenGlRestore -ClassKey $classKey -Plan $glRestore')
$posFiles = $src.IndexOf('Remove-PathOrSchedule $d')
Check (($posKeys -gt 0) -and ($posKeys -lt $posFiles)) 'the rollback removes the values before the files they name'
$posOffKeys = $src.IndexOf('Write-OpenGlRestore -ClassKey $classKey -Plan $glOffPlan')
$posOffFiles = $src.IndexOf('Remove-PathOrSchedule $glDirWow')
Check (($posOffKeys -gt 0) -and ($posOffKeys -lt $posOffFiles)) 'the narrowing step too removes the values before the file they name'
Check ($src -match "Add-Result 'OpenGL ICD'") 'verify reports the OpenGL ICD'
Check ($un -match 'Write-OpenGlRestore -ClassKey \$glKey -Plan \$glPlan') 'uninstall.ps1 restores or removes the six values'
# The registration must go even when Get-Bc250Device does not return exactly one device (the GPU disabled or hidden
# in Device Manager, a ghost entry, a WMI hiccup), because the files below go whatever the device looks like. The
# OpenGL step is therefore outside the one-device block, and before the Files step.
Check ($un -match 'Get-OpenGlRegisteredClassKeys -PreferredKey') 'uninstall.ps1 finds the registration by what a display class key holds, not by the device'
$posGlWrite = $un.IndexOf('Write-OpenGlRestore -ClassKey $glKey')
$posRoot = $un.IndexOf('Invoke-Change "remove $root"')
# Read from the parse tree, not from the text: every call of the OpenGL functions must sit outside the if statement
# that asks for exactly one device.
$unAst = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Installer 'uninstall.ps1'), [ref]$null, [ref]$null)
$glCalls = @($unAst.FindAll({ param($n) ($n -is [System.Management.Automation.Language.CommandAst]) -and ($n.GetCommandName() -match '^(Get-OpenGlRegisteredClassKeys|Get-OpenGlRestorePlan|Write-OpenGlRestore)$') }, $true))
$guarded = @($glCalls | Where-Object {
    $p = $_.Parent
    $inGuard = $false
    while ($p) {
        if (($p -is [System.Management.Automation.Language.IfStatementAst]) -and ($p.Extent.Text -match 'Get-Bc250Device|\$dev\.Count')) { $inGuard = $true }
        $p = $p.Parent
    }
    $inGuard
})
Check (($glCalls.Count -ge 3) -and ($guarded.Count -eq 0)) "the OpenGL step is outside the one-device guard of the Graphics step ($($glCalls.Count) calls, $($guarded.Count) inside it)"
Check (($posGlWrite -gt 0) -and ($posRoot -gt $posGlWrite)) 'the registration goes before the install root it names is removed'
if ($fail) { "$fail check(s) failed"; exit 1 }
if ($Package) {
    'the ICD files of this package'
    $mf = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
    $s = Get-OpenGlReleaseSwitch -Manifest $mf -PackageRoot $Package
    if (-not $s.register) { '  SKIP this release registers no OpenGL ICD' }
    else {
        "  the architectures of this package: $(@($s.architectures) -join ', ')"
        foreach ($f in @(Get-OpenGlArchFiles -Switch $s)) {
            $p = @(Test-OpenGlIcdFile -Path $f.source -Machine $f.machine)
            Check ($p.Count -eq 0) "$($f.package_path): $($f.arch), machine and every Drv entry point ($($p -join '; '))"
        }
        if (-not $s.register_x86) {
            Check (-not (Test-Path -LiteralPath $s.source_wow)) "this release registers x64 alone, so the package carries no $($s.package_path_wow)"
        }
    }
    if ($fail) { "$fail check(s) failed"; exit 1 }
}
'all checks passed'
exit 0
