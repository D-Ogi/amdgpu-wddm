# Host test of the 32-bit registration (BD-064) on a computer WITHOUT a BC-250. Windows PowerShell 5.1, like the
# installer:
#   powershell -NoProfile -File tools\release\test-wow64.ps1 -Package <unpacked package folder> -WorkRoot <dir>
# Asserts: every file under payload\wow64 is an x86 image or the ICD manifest, every other payload image is x64; the
# package has no D3D9 stub (no payload\system32, no payload\syswow64: D3D9 goes through D3D9On12); manifest.json gives
# the <InstallDir>\wow64 install paths; the x86 router and shells export their entry points undecorated (the x86
# D3D12 shell exactly OpenAdapter12); one x86 RADV build serves the D3D11 and D3D12 shells and the system ICD; the x86
# Vulkan manifest names its library relative to itself; verify's Test-WowRegistration and Test-UmdRegistration (common.ps1) pass a
# complete registration, with the D3D9 slot empty in both views after a registry round trip, and name each missing or
# wrong part, the stub layout of the earlier releases included. The registration cases run against a scratch key,
# HKCU:\Software\amdgpu-wddm-installer-test-wow, removed at the end, and files under -WorkRoot. Nothing under HKLM and
# nothing outside -WorkRoot is written.
param([Parameter(Mandatory)][string]$Package, [Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $Package 'installer\common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
# The undecorated export names of a PE image (x86 or x64), read from its export directory.
function Get-PeExports([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($b, 60)
    $magic = [BitConverter]::ToUInt16($b, $pe + 24)
    $dd = $pe + 24 + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $rva = [BitConverter]::ToUInt32($b, $dd)
    if (-not $rva) { return @() }
    $nsec = [BitConverter]::ToUInt16($b, $pe + 6)
    $sec = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
    $toOff = {
        param([uint32]$r)
        for ($i = 0; $i -lt $nsec; $i++) {
            $s = $sec + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12); $sz = [BitConverter]::ToUInt32($b, $s + 8); $raw = [BitConverter]::ToUInt32($b, $s + 20)
            if ($r -ge $va -and $r -lt $va + [Math]::Max($sz, [BitConverter]::ToUInt32($b, $s + 16))) { return [int]($r - $va + $raw) }
        }
        throw "RVA $r outside the sections of $Path"
    }
    $ed = & $toOff $rva
    $n = [BitConverter]::ToUInt32($b, $ed + 24)
    $names = & $toOff ([BitConverter]::ToUInt32($b, $ed + 32))
    $out = @()
    for ($i = 0; $i -lt $n; $i++) {
        $o = & $toOff ([BitConverter]::ToUInt32($b, $names + 4 * $i))
        $e = $o; while ($b[$e]) { $e++ }
        $out += [Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    return $out
}

'payload images: x86 under wow64, x64 elsewhere, no D3D9 stub'
Check (-not (Test-Path -LiteralPath (Join-Path $Package 'payload\system32')) -and -not (Test-Path -LiteralPath (Join-Path $Package 'payload\syswow64'))) 'no payload\system32 and no payload\syswow64: the package ships no D3D9 stub'
$wowFiles = @(Get-ChildItem -LiteralPath (Join-Path $Package 'payload\wow64') -Recurse -File)
Check ($wowFiles.Count -eq 11) "11 files under payload\wow64 ($($wowFiles.Count))"
foreach ($f in $wowFiles) {
    if ($f.Extension -in '.json', '.config') { continue }
    $m = Get-PeMachine $f.FullName
    Check ($m -eq 0x14C) ('{0}: x86 (machine 0x{1:X})' -f $f.FullName.Substring($Package.Length + 1), $m)
}
foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $Package 'payload') -Recurse -File | Where-Object { ($_.Extension -in '.dll', '.exe', '.sys') -and ($_.FullName -notmatch '\\payload\\wow64\\') })) {
    $m = Get-PeMachine $f.FullName
    if ($m -ne 0x8664) { Check $false ('{0}: x64 (machine 0x{1:X})' -f $f.FullName.Substring($Package.Length + 1), $m) }
}
Check $true 'every other payload image is x64 (only failures are listed)'
foreach ($p in @('payload\wow64\desktop\bc250d3d_router.dll')) {
    $x = @(Get-PeExports (Join-Path $Package $p))
    Check ((@('OpenAdapter10', 'OpenAdapter10_2' | Where-Object { $x -notcontains $_ }).Count -eq 0) -and (@($x | Where-Object { $_ -match '@' }).Count -eq 0)) "$p exports OpenAdapter10 and OpenAdapter10_2 undecorated ($($x -join ', '))"
}
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\d3d11\amdgpu_wddm_d3d11.dll'))
Check (($x -contains 'OpenAdapter10_2') -and (@($x | Where-Object { $_ -match '@' }).Count -eq 0)) "wow64\d3d11\amdgpu_wddm_d3d11.dll exports OpenAdapter10_2 undecorated ($($x -join ', '))"
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\vulkan\vulkan_radeon.dll'))
Check (($x -contains 'vk_icdGetInstanceProcAddr') -and ($x -contains 'vk_icdNegotiateLoaderICDInterfaceVersion')) 'wow64\vulkan\vulkan_radeon.dll exports the ICD entry points undecorated'
$icd = Get-Content -LiteralPath (Join-Path $Package 'payload\wow64\vulkan\radeon_icd.json') -Raw | ConvertFrom-Json
Check ($icd.ICD.library_path -eq '.\vulkan_radeon.dll') "x86 ICD manifest library_path $($icd.ICD.library_path) (relative to the manifest)"
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\d3d12\amdgpu_wddm_d3d12.dll'))
Check (($x -join ',') -eq 'OpenAdapter12') "wow64\d3d12\amdgpu_wddm_d3d12.dll exports exactly OpenAdapter12, undecorated ($($x -join ', '))"
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\d3d12\amdgpu_wddm_vkd3d.dll'))
Check (($x -join ',') -eq 'Bc250Vkd3dEngineGetFuncs') "wow64\d3d12\amdgpu_wddm_vkd3d.dll exports exactly Bc250Vkd3dEngineGetFuncs ($($x -join ', '))"
$radv = @('vulkan\vulkan_radeon.dll', 'd3d11\amdgpu_wddm_radv.dll', 'd3d12\amdgpu_wddm_radv.dll' | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $Package "payload\wow64\$_")).Hash })
Check (@($radv | Select-Object -Unique).Count -eq 1) 'one x86 RADV build for the D3D11 and D3D12 shells and the system ICD'

'manifest.json install paths'
$m = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
$c = @($m.components | Where-Object { $_.package_path -like 'payload/wow64/*' })
Check ($c.Count -eq 11) "11 x86 components in manifest.json ($($c.Count))"
$sys = @($m.components | Where-Object { ([string]$_.install_path -like '%SystemRoot%*') -or ([string]$_.package_path -match '^payload/(system32|syswow64)/') -or ([string]$_.package_path -like '*bc250umd*') })
Check ($sys.Count -eq 0) "no component installs into System32 or SysWOW64, no D3D9 stub ($(@($sys | ForEach-Object { $_.package_path }) -join ', '))"
Check ((@($c | Where-Object { $_.package_path -eq 'payload/wow64/d3d11/amdgpu_wddm_d3d11.dll' })[0].install_path) -eq '<InstallDir>\wow64\d3d11\amdgpu_wddm_d3d11.dll') 'the x86 D3D11 shell goes to <InstallDir>\wow64\d3d11'
Check ((@($c | Where-Object { $_.package_path -eq 'payload/wow64/d3d12/amdgpu_wddm_d3d12.dll' })[0].install_path) -eq '<InstallDir>\wow64\d3d12\amdgpu_wddm_d3d12.dll') 'the x86 D3D12 shell goes to <InstallDir>\wow64\d3d12'

'verify: Test-WowRegistration against a scratch key and folder'
$key = 'HKCU:\Software\amdgpu-wddm-installer-test-wow'
$root = Join-Path $WorkRoot ('wow64-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    foreach ($k in "$key\class", "$key\khronos", "$key\sw\DesktopRouter", "$key\sw\AppRouter") { [void](New-Item -Path $k -Force) }
    [void][IO.Directory]::CreateDirectory($root)
    # The package's own x86 images in a copy of the install layout.
    $files = @()
    foreach ($rel in 'desktop\bc250d3d_router.dll', 'desktop\bc250d3d.dll', 'd3d11\amdgpu_wddm_d3d11.dll', 'd3d11\amdgpu_wddm_dxvk.dll', 'd3d11\amdgpu_wddm_radv.dll',
        'd3d12\amdgpu_wddm_d3d12.dll', 'd3d12\amdgpu_wddm_vkd3d.dll', 'd3d12\amdgpu_wddm_radv.dll', 'vulkan\vulkan_radeon.dll') {
        $dst = Join-Path $root "wow64\$rel"
        [void][IO.Directory]::CreateDirectory((Split-Path $dst))
        Copy-Item -LiteralPath (Join-Path $Package "payload\wow64\$rel") -Destination $dst
        $files += $dst
    }
    $icdPath = Join-Path $root 'wow64\vulkan\radeon_icd.json'
    $args0 = @{ InstallRoot = $root; ClassKey = "$key\class"; KhronosKey = "$key\khronos"; SoftwareKey = "$key\sw"; Files = $files }
    $p = @(Test-WowRegistration @args0)
    Check ($p.Count -eq 5) "nothing registered: 5 registry findings, the files are in place ($($p.Count): $($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]](Get-WowUmdNames $root)) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\class" -Name VulkanDriverNameWow -Value ([string[]]@($icdPath)) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\khronos" -Name $icdPath -Value 0 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\sw\DesktopRouter" -Name CpuUmdPathWow -Value (Join-Path $root 'wow64\desktop\bc250d3d.dll') -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\sw\AppRouter" -Name GpuUmdPathWow -Value (Join-Path $root 'wow64\d3d11\amdgpu_wddm_d3d11.dll') -PropertyType String -Force | Out-Null
    $p = @(Test-WowRegistration @args0)
    Check ($p.Count -eq 0) "complete registration: no finding ($($p -join '; '))"
    $wowUmd = @(Get-WowUmdNames $root)
    Check (($wowUmd.Count -eq 4) -and ($wowUmd[0] -eq '') -and ($wowUmd[1] -eq (Join-Path $root 'wow64\desktop\bc250d3d_router.dll')) -and ($wowUmd[2] -eq $wowUmd[1]) -and ($wowUmd[3] -eq (Join-Path $root 'wow64\d3d12\amdgpu_wddm_d3d12.dll'))) "UserModeDriverNameWow: D3D9 empty, the x86 router twice, the x86 D3D12 shell ($(Format-UmdNames $wowUmd))"
    $back = @((Get-ItemProperty -LiteralPath "$key\class").UserModeDriverNameWow)
    Check (($back.Count -eq 4) -and ($back[0] -eq '') -and ($back[1] -eq $wowUmd[1]) -and ($back[3] -eq $wowUmd[3])) "the empty D3D9 slot survives the registry round trip (UserModeDriverNameWow read back: $(Format-UmdNames $back))"
    # The layout of the releases up to tester.17, with the stub in the D3D9 slot, is a finding.
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]](@('bc250umd.dll') + $wowUmd[1..3])) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 1) -and ($p[0] -match '^UserModeDriverNameWow is bc250umd\.dll')) "the stub in the D3D9 slot is a finding ($($p -join '; '))"
    # The layout without an x86 D3D12 slot (three entries), a fifth entry, a 64-bit image in place of an x86 one, a
    # missing file.
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]]$wowUmd[0..2]) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 1) -and ($p[0] -match '^UserModeDriverNameWow is')) "a value without the x86 D3D12 slot is a finding ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]]($wowUmd + 'x.dll')) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 1) -and ($p[0] -match '^UserModeDriverNameWow is')) "a fifth slot is a finding ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]](Get-WowUmdNames $root)) -PropertyType MultiString -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Package 'payload\desktop\bc250d3d_router.dll') -Destination (Join-Path $root 'wow64\desktop\bc250d3d_router.dll') -Force
    Remove-Item -LiteralPath (Join-Path $root 'wow64\vulkan\vulkan_radeon.dll')
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 2) -and ($p -match 'bc250d3d_router\.dll is not x86 \(machine 0x8664\)') -and ($p -match 'vulkan_radeon\.dll missing')) "an x64 router and a missing ICD are findings ($($p -join '; '))"
    Set-ItemProperty -LiteralPath "$key\khronos" -Name $icdPath -Value 1
    $p = @(Test-WowRegistration @args0)
    Check (@($p | Where-Object { $_ -match 'khronos has no' }).Count -eq 1) 'a disabled Khronos entry (1) is a finding'

    # The 64-bit registration (verify's 'D3D registration'): four slots, the D3D9 slot empty.
    $p = @(Test-UmdRegistration -InstallRoot $root -ClassKey "$key\class")
    Check (($p.Count -eq 2) -and ($p -match '^UserModeDriverName is missing') -and ($p -match '^VulkanDriverName is')) "64-bit: nothing registered: 2 findings ($($p -join '; '))"
    $umd = @(Get-UmdNames $root)
    Check (($umd.Count -eq 4) -and ($umd[0] -eq '') -and ($umd[1] -eq (Join-Path $root 'desktop\bc250d3d_router.dll')) -and ($umd[2] -eq $umd[1]) -and ($umd[3] -eq (Join-Path $root 'd3d12\amdgpu_wddm_d3d12.dll'))) "UserModeDriverName: D3D9 empty, the router twice, the D3D12 shell ($(Format-UmdNames $umd))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverName -Value ([string[]]$umd) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath "$key\class" -Name VulkanDriverName -Value ([string[]]@((Join-Path $root 'vulkan\radeon_icd.json'))) -PropertyType MultiString -Force | Out-Null
    $back = @((Get-ItemProperty -LiteralPath "$key\class").UserModeDriverName)
    Check (($back.Count -eq 4) -and ($back[0] -eq '') -and ($back[3] -eq $umd[3])) "the empty D3D9 slot survives the registry round trip (UserModeDriverName read back: $(Format-UmdNames $back))"
    $p = @(Test-UmdRegistration -InstallRoot $root -ClassKey "$key\class")
    Check ($p.Count -eq 0) "64-bit: complete registration: no finding ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverName -Value ([string[]](@('bc250umd.dll') + $umd[1..3])) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-UmdRegistration -InstallRoot $root -ClassKey "$key\class")
    Check (($p.Count -eq 1) -and ($p[0] -match '^UserModeDriverName is bc250umd\.dll')) "64-bit: the stub in the D3D9 slot is a finding ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverName -Value ([string[]]$umd[1..3]) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-UmdRegistration -InstallRoot $root -ClassKey "$key\class")
    Check ($p.Count -eq 1) "64-bit: a value without the D3D9 slot (three entries) is a finding ($($p -join '; '))"
} finally {
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
Check (-not (Test-Path -LiteralPath $key)) 'scratch key removed'

'install and uninstall sources'
$src = [IO.File]::ReadAllText((Join-Path $Package 'installer\install.ps1'))
$un = [IO.File]::ReadAllText((Join-Path $Package 'installer\uninstall.ps1'))
$common = [IO.File]::ReadAllText((Join-Path $Package 'installer\common.ps1'))
Check (($common -match "SysWOW64\\bc250umd\.dll'\); flag = 'stub_wow_existed'") -and ($src -match "Set-StateValueOnce \`$state \`$s\.flag") -and ($un -match "flag = 'stub_wow_existed'")) 'the SysWOW64 stub of an earlier release is kept on install and uninstall when it was there before the first install'
Check (($src -notmatch 'payload\\(system32|syswow64)') -and ($src -notmatch 'Copy-FileSafe [^\r\n]*bc250umd')) 'install.ps1 copies no D3D9 stub'
Check (($src -match 'foreach \(\$s in @\(Get-LegacyStubPaths\)\) \{\s+if \(\$state\.\(\$s\.flag\)\)') -and ($src -match 'Remove-PathOrSchedule \$s\.path')) 'install.ps1 removes the stub of an earlier release unless it was there before the first install'
Check ($src -match "Add-Result 'D3D registration'") 'verify reports the 64-bit registration'
Check (($un -match "'UserModeDriverNameWow'") -and ($un -match "'VulkanDriverNameWow'") -and ($un -match 'KhronosKeyWow')) 'uninstall removes the Wow values and the WOW6432Node Khronos entry'
Check ($src -match "Add-Result '32-bit D3D/Vulkan'") 'verify reports the 32-bit registration'
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
