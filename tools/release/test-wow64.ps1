# Host test of the 32-bit registration (BD-064) on a computer WITHOUT a BC-250. Windows PowerShell 5.1, like the
# installer:
#   powershell -NoProfile -File tools\release\test-wow64.ps1 -Package <unpacked package folder> -WorkRoot <dir>
# Asserts: every file under payload\wow64 and payload\syswow64 is an x86 image or the ICD manifest, every other payload
# image is x64; manifest.json gives the SysWOW64 and <InstallDir>\wow64 install paths; the x86 D3D9 stub and router
# export their entry points undecorated; the x86 Vulkan manifest names its library relative to itself; verify's
# Test-WowRegistration (common.ps1) passes a complete registration and names each missing or wrong part. The
# registration cases run against a scratch key, HKCU:\Software\amdgpu-wddm-installer-test-wow, removed at the end, and
# files under -WorkRoot. Nothing under HKLM and nothing outside -WorkRoot is written.
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

'payload images: x86 under wow64 and syswow64, x64 elsewhere'
$wowFiles = @(Get-ChildItem -LiteralPath (Join-Path $Package 'payload\wow64') -Recurse -File) + @(Get-ChildItem -LiteralPath (Join-Path $Package 'payload\syswow64') -File)
Check ($wowFiles.Count -eq 9) "9 files under payload\wow64 and payload\syswow64 ($($wowFiles.Count))"
foreach ($f in $wowFiles) {
    if ($f.Extension -in '.json', '.config') { continue }
    $m = Get-PeMachine $f.FullName
    Check ($m -eq 0x14C) ('{0}: x86 (machine 0x{1:X})' -f $f.FullName.Substring($Package.Length + 1), $m)
}
foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $Package 'payload') -Recurse -File | Where-Object { ($_.Extension -in '.dll', '.exe', '.sys') -and ($_.FullName -notmatch '\\payload\\(wow64|syswow64)\\') })) {
    $m = Get-PeMachine $f.FullName
    if ($m -ne 0x8664) { Check $false ('{0}: x64 (machine 0x{1:X})' -f $f.FullName.Substring($Package.Length + 1), $m) }
}
Check $true 'every other payload image is x64 (only failures are listed)'
foreach ($p in 'payload\syswow64\bc250umd.dll', 'payload\wow64\desktop\bc250d3d_router.dll') {
    $x = @(Get-PeExports (Join-Path $Package $p))
    Check ((@('OpenAdapter10', 'OpenAdapter10_2' | Where-Object { $x -notcontains $_ }).Count -eq 0) -and (@($x | Where-Object { $_ -match '@' }).Count -eq 0)) "$p exports OpenAdapter10 and OpenAdapter10_2 undecorated ($($x -join ', '))"
}
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\d3d11\amdgpu_wddm_d3d11.dll'))
Check (($x -contains 'OpenAdapter10_2') -and (@($x | Where-Object { $_ -match '@' }).Count -eq 0)) "wow64\d3d11\amdgpu_wddm_d3d11.dll exports OpenAdapter10_2 undecorated ($($x -join ', '))"
$x = @(Get-PeExports (Join-Path $Package 'payload\wow64\vulkan\vulkan_radeon.dll'))
Check (($x -contains 'vk_icdGetInstanceProcAddr') -and ($x -contains 'vk_icdNegotiateLoaderICDInterfaceVersion')) 'wow64\vulkan\vulkan_radeon.dll exports the ICD entry points undecorated'
$icd = Get-Content -LiteralPath (Join-Path $Package 'payload\wow64\vulkan\radeon_icd.json') -Raw | ConvertFrom-Json
Check ($icd.ICD.library_path -eq '.\vulkan_radeon.dll') "x86 ICD manifest library_path $($icd.ICD.library_path) (relative to the manifest)"
Check ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\wow64\vulkan\vulkan_radeon.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'payload\wow64\d3d11\amdgpu_wddm_radv.dll')).Hash) 'one x86 RADV build for the D3D11 shell and the system ICD'

'manifest.json install paths'
$m = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
$c = @($m.components | Where-Object { $_.package_path -like 'payload/wow64/*' -or $_.package_path -like 'payload/syswow64/*' })
Check ($c.Count -eq 9) "9 x86 components in manifest.json ($($c.Count))"
Check ((@($c | Where-Object { $_.package_path -eq 'payload/syswow64/bc250umd.dll' })[0].install_path) -eq '%SystemRoot%\SysWOW64\bc250umd.dll') 'the x86 stub goes to %SystemRoot%\SysWOW64'
Check ((@($c | Where-Object { $_.package_path -eq 'payload/wow64/d3d11/amdgpu_wddm_d3d11.dll' })[0].install_path) -eq '<InstallDir>\wow64\d3d11\amdgpu_wddm_d3d11.dll') 'the x86 D3D11 shell goes to <InstallDir>\wow64\d3d11'

'verify: Test-WowRegistration against a scratch key and folder'
$key = 'HKCU:\Software\amdgpu-wddm-installer-test-wow'
$root = Join-Path $WorkRoot ('wow64-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    foreach ($k in "$key\class", "$key\khronos", "$key\sw\DesktopRouter", "$key\sw\AppRouter") { [void](New-Item -Path $k -Force) }
    [void][IO.Directory]::CreateDirectory($root)
    # The package's own x86 images in a copy of the install layout; the SysWOW64 stub from the package too.
    $files = @()
    foreach ($rel in 'desktop\bc250d3d_router.dll', 'desktop\bc250d3d.dll', 'd3d11\amdgpu_wddm_d3d11.dll', 'd3d11\amdgpu_wddm_dxvk.dll', 'd3d11\amdgpu_wddm_radv.dll', 'vulkan\vulkan_radeon.dll') {
        $dst = Join-Path $root "wow64\$rel"
        [void][IO.Directory]::CreateDirectory((Split-Path $dst))
        Copy-Item -LiteralPath (Join-Path $Package "payload\wow64\$rel") -Destination $dst
        $files += $dst
    }
    $files = @((Join-Path $Package 'payload\syswow64\bc250umd.dll')) + $files
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
    Check ((@(Get-WowUmdNames $root).Count -eq 3) -and ((Get-WowUmdNames $root)[0] -eq 'bc250umd.dll')) 'UserModeDriverNameWow has the D3D9, D3D10 and D3D11 slots only (no x86 D3D12)'
    # A D3D12 entry appended, a 64-bit image in place of an x86 one, a missing file.
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]]((Get-WowUmdNames $root) + 'x.dll')) -PropertyType MultiString -Force | Out-Null
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 1) -and ($p[0] -match '^UserModeDriverNameWow is')) "a fourth slot is a finding ($($p -join '; '))"
    New-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value ([string[]](Get-WowUmdNames $root)) -PropertyType MultiString -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Package 'payload\desktop\bc250d3d_router.dll') -Destination (Join-Path $root 'wow64\desktop\bc250d3d_router.dll') -Force
    Remove-Item -LiteralPath (Join-Path $root 'wow64\vulkan\vulkan_radeon.dll')
    $p = @(Test-WowRegistration @args0)
    Check (($p.Count -eq 2) -and ($p -match 'bc250d3d_router\.dll is not x86 \(machine 0x8664\)') -and ($p -match 'vulkan_radeon\.dll missing')) "an x64 router and a missing ICD are findings ($($p -join '; '))"
    Set-ItemProperty -LiteralPath "$key\khronos" -Name $icdPath -Value 1
    $p = @(Test-WowRegistration @args0)
    Check (@($p | Where-Object { $_ -match 'khronos has no' }).Count -eq 1) 'a disabled Khronos entry (1) is a finding'
} finally {
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
Check (-not (Test-Path -LiteralPath $key)) 'scratch key removed'

'install and uninstall sources'
$src = [IO.File]::ReadAllText((Join-Path $Package 'installer\install.ps1'))
$un = [IO.File]::ReadAllText((Join-Path $Package 'installer\uninstall.ps1'))
Check (($src -match "Set-StateValueOnce \`$state 'stub_wow_existed'") -and ($un -match "flag = 'stub_wow_existed'")) 'the SysWOW64 stub is kept on uninstall when it was there before the install'
Check (($un -match "'UserModeDriverNameWow'") -and ($un -match "'VulkanDriverNameWow'") -and ($un -match 'KhronosKeyWow')) 'uninstall removes the Wow values and the WOW6432Node Khronos entry'
Check ($src -match "Add-Result '32-bit D3D/Vulkan'") 'verify reports the 32-bit registration'
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
