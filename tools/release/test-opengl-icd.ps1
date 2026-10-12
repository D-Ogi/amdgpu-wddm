# Host test of the OpenGL ICD registration (BD-076, GitHub issue #8) on a computer WITHOUT a BC-250. Windows
# PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-opengl-icd.ps1 -Installer <package>\installer [-Package <package>]
# Asserts: the release switch of manifest.json (register on and off, the package paths, the value type, the version
# and the flags, and the refusals of a malformed switch); the six values the installer writes, with their types and
# the paths under the install root; verify's Test-OpenGlRegistration against a complete and an incomplete
# registration; the ICD file check (machine, the Drv entry points, a file that is no image); the restore plan of the
# uninstaller (a value from before the install comes back, a value that was not there is removed, a value somebody
# else changed is kept, and without a record only a value that names the install root is ours); and that install.ps1
# and uninstall.ps1 carry the steps. With -Package the real ICD files of that package are checked too.
# The registration cases run against a scratch key, HKCU:\Software\amdgpu-wddm-installer-test-gl, removed at the end,
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
$sz = Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table @{ register = $true; value_type = 'REG_SZ'; version = 2; flags = 3 })) -PackageRoot $null
Check (($sz.value_type -eq 'REG_SZ') -and ($sz.version -eq 2) -and ($sz.flags -eq 3)) 'the switch can ask for REG_SZ, another version and other flags'
foreach ($bad in @(
    @{ what = 'a package path outside payload'; t = @{ register = $true; package_path = 'opengl/amdgpu_wddm_gl.dll' } }
    @{ what = 'a 64-bit path in the Wow slot'; t = @{ register = $true; package_path_wow = 'payload/opengl/amdgpu_wddm_gl.dll' } }
    @{ what = 'an unknown value type'; t = @{ register = $true; value_type = 'REG_DWORD' } }
    @{ what = 'version 0'; t = @{ register = $true; version = 0 } })) {
    $threw = $false
    try { [void](Get-OpenGlReleaseSwitch -Manifest (New-Switch (New-Table $bad.t)) -PackageRoot $null) } catch { $threw = $true }
    Check $threw "the switch refuses $($bad.what)"
}

'the six values the installer writes'
$root = 'C:\Program Files\amdgpu-wddm'
$plan = @(Get-OpenGlRegistrationPlan -Switch $on -InstallRoot $root)
Check ((@($plan | ForEach-Object { $_.name }) -join ',') -eq 'OpenGLDriverName,OpenGLVersion,OpenGLFlags,OpenGLDriverNameWow,OpenGLVersionWow,OpenGLFlagsWow') "six values in order ($(@($plan | ForEach-Object { $_.name }) -join ', '))"
Check ((@($plan | Where-Object { $_.name -eq 'OpenGLDriverName' })[0].value -join '|') -eq "$root\opengl\amdgpu_wddm_gl.dll") 'OpenGLDriverName names the x64 ICD under the install root'
Check ((@($plan | Where-Object { $_.name -eq 'OpenGLDriverNameWow' })[0].value -join '|') -eq "$root\wow64\opengl\amdgpu_wddm_gl.dll") 'OpenGLDriverNameWow names the x86 ICD under the install root'
Check ((@($plan | Where-Object { $_.type -eq 'MultiString' }).Count -eq 2) -and (@($plan | Where-Object { $_.type -eq 'DWord' }).Count -eq 4)) 'two REG_MULTI_SZ name values and four REG_DWORD numbers'
$planSz = @(Get-OpenGlRegistrationPlan -Switch $sz -InstallRoot $root)
Check (@($planSz | Where-Object { $_.type -eq 'String' }).Count -eq 2) 'with REG_SZ the two name values are single strings'
Check ((Format-OpenGlRegistrationPlan $plan) -match 'OpenGLDriverName = \[C:\\Program Files\\amdgpu-wddm\\opengl\\amdgpu_wddm_gl\.dll\]; OpenGLVersion = 1; OpenGLFlags = 1; OpenGLDriverNameWow = ') "the dry run line names every value ($(Format-OpenGlRegistrationPlan $plan))"

'the ICD file check (machine, the Drv entry points)'
Check (@($script:OpenGlDrvExports).Count -eq 12) "12 Drv entry points in the contract ($(@($script:OpenGlDrvExports) -join ', '))"
$work = Join-Path $env:TEMP ('amdgpu-wddm-gl-test-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$key = 'HKCU:\Software\amdgpu-wddm-installer-test-gl'
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
    Check (@($p | Where-Object { $_ -match 'does not export DrvValidateVersion' }).Count -eq 1) "an image without the Drv entry points is a finding ($($p -join '; '))"
    $p = @(Test-OpenGlIcdFile -Path $host64 -Machine 0x14C)
    Check (@($p | Where-Object { $_ -match 'is machine 0x8664, expected 0x14C' }).Count -eq 1) "the wrong machine is a finding ($($p -join '; '))"

    'verify: Test-OpenGlRegistration against a scratch key'
    [void](New-Item -Path "$key\class" -Force)
    $args0 = @{ InstallRoot = $work; ClassKey = "$key\class"; Switch = $on }
    $p = @(Test-OpenGlRegistration @args0)
    Check ((@($p | Where-Object { $_ -match ' is missing$' }).Count -eq 6) -and (@($p | Where-Object { $_ -match 'missing or not an image' }).Count -eq 2)) "nothing registered: six values missing and both files missing ($($p.Count) findings)"
    $workPlan = @(Get-OpenGlRegistrationPlan -Switch $on -InstallRoot $work)
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
Check ($src -match 'throw "manifest\.json registers the OpenGL ICD, but this package has no') 'install.ps1 refuses a package that registers the ICD without carrying it'
Check (($src -match 'Set-StateValueOnce \$state ''opengl_icd_previous''') -and ($src -match 'Set-StateValue \$state ''opengl_icd''')) 'install.ps1 records the values from before the install and the values it writes'
Check ($src -match 'Write-OpenGlRegistration -ClassKey \$classKey -Plan \$glPlan') 'install.ps1 writes the registration into the GPU software key'
Check (($src -match 'Write-OpenGlRestore -ClassKey \$classKey -Plan \$glRestore') -and ($src -match 'Remove-PathOrSchedule \$glDir')) 'install.ps1 rolls the registration and the files back when this release does not register the ICD'
$posKeys = $src.IndexOf('Write-OpenGlRestore -ClassKey $classKey')
$posFiles = $src.IndexOf('Remove-PathOrSchedule $glDir')
Check (($posKeys -gt 0) -and ($posKeys -lt $posFiles)) 'the rollback removes the values before the files they name'
Check ($src -match "Add-Result 'OpenGL ICD'") 'verify reports the OpenGL ICD'
Check ($un -match 'Write-OpenGlRestore -ClassKey \$key -Plan \$glPlan') 'uninstall.ps1 restores or removes the six values'
if ($fail) { "$fail check(s) failed"; exit 1 }
if ($Package) {
    'the ICD files of this package'
    $mf = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
    $s = Get-OpenGlReleaseSwitch -Manifest $mf -PackageRoot $Package
    if (-not $s.register) { '  SKIP this release registers no OpenGL ICD' }
    else {
        foreach ($f in @(@{ p = $s.source; m = 0x8664; n = $s.package_path }, @{ p = $s.source_wow; m = 0x14C; n = $s.package_path_wow })) {
            $p = @(Test-OpenGlIcdFile -Path $f.p -Machine $f.m)
            Check ($p.Count -eq 0) "$($f.n): machine and every Drv entry point ($($p -join '; '))"
        }
    }
    if ($fail) { "$fail check(s) failed"; exit 1 }
}
'all checks passed'
exit 0
