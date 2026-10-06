# Host test of a built package on a computer WITHOUT a BC-250 (the development PC): the dry runs of install and
# uninstall must run every check, refuse cleanly and change nothing. Never run the real install here.
#   pwsh -File tools\release\test-dryrun.ps1 -Package <unpacked package folder>
# PowerShell 7 (headless.ps1). Every child runs through Invoke-Headless: no window, stdin closed, -NonInteractive
# (a Read-Host would fail at once instead of waiting), a time bound. A dry run never elevates, never prompts, never
# restarts and never opens a form.
# Asserts: all installer scripts parse under Windows PowerShell 5.1 and are ASCII; install -DryRun exits 2
# (preflight refusal) with a 'fail' line for the BC-250 GPU; -DryRunIgnoreBoard walks phases 1 and 2; uninstall
# -DryRun finds nothing; %ProgramData%\amdgpu-wddm, HKLM\SOFTWARE\amdgpu-wddm, the RunOnce entry, the task, the
# service, C:\BC250, the install root, the certificate stores and the boot options are the same before and after.
# GPU firmware: not in the package; the dry run prints both URLs and the SHA256 of each file and downloads nothing;
# test-firmware.ps1 downloads the files for real into the scratch folder and checks the refusals; -FirmwareDir dry runs
# with a good and a changed folder. Needs network access to git.kernel.org and gitlab.com.
param([Parameter(Mandatory)][string]$Package, [string]$WorkBase = (Join-Path (Split-Path $Package) 'test-tmp'))  # scratch for test-filesafe.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
# One RunOnce argument as common.ps1 Format-CommandLine writes it (quoted unless plain path characters).
function Format-RunOnceArg([string]$A) { if ($A -match '[^A-Za-z0-9_.:\\/=,+-]') { return '"' + $A + '"' } else { return $A } }
function Get-Footprint {
    [ordered]@{
        programdata  = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramData 'amdgpu-wddm') -Recurse -Force -ErrorAction SilentlyContinue).Count
        software     = Test-Path -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm'
        runonce      = [bool](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'amdgpu-wddm-installer' -ErrorAction SilentlyContinue)
        task         = [bool](Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue)
        service      = [bool](Get-Service -Name bc250kmd -ErrorAction SilentlyContinue)
        bc250        = Test-Path -LiteralPath 'C:\BC250'
        programfiles = Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'amdgpu-wddm')
        stub         = Test-Path -LiteralPath (Join-Path $env:windir 'System32\bc250umd.dll')
        stubwow      = Test-Path -LiteralPath (Join-Path $env:windir 'SysWOW64\bc250umd.dll')
        khronoswow   = @((Get-Item -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers' -ErrorAction SilentlyContinue).Property | Where-Object { $_ -like '*amdgpu-wddm*' }).Count
        certs        = @(Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Subject -like '*amdgpu-wddm*' }).Count
        bootopts     = [string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions
    } | ConvertTo-Json -Compress
}
function Invoke-Ps51([string[]]$ScriptArgs) {
    $r = Invoke-Headless -File $ps51 -Arguments (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File') + $ScriptArgs) -TimeoutSeconds 180
    "  (pid $($r.pid), exit $($r.code), no window)" | Write-Host
    return $r
}
'parse (Windows PowerShell 5.1)'
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-parse51.ps1'), '-Directory', (Join-Path $Package 'installer'))
$r.text
Check ($r.code -eq 0) 'every installer script parses under 5.1'
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Package 'installer') -File) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    Check (-not ($b | Where-Object { $_ -gt 127 } | Select-Object -First 1)) "$($f.Name) is ASCII"
}
$m = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
$inf = [IO.File]::ReadAllText((Join-Path $Package 'payload\kmd\bc250kmd.inf'))
Check ($inf -match ('(?m)^DriverVer\s*=\s*[\d/]+,' + [regex]::Escape([string]$m.kmd_version) + '\s*$')) "INF DriverVer = manifest kmd_version $($m.kmd_version)"
Check ([string]$m.version -like "$($m.kmd_version)-*") "release version $($m.version) carries the driver version"
$before = Get-Footprint

'install -DryRun'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun')
$r.text
Check ($r.code -eq 2) "install -DryRun exit $($r.code) (2 = preflight refusal)"
Check ($r.text -match '\[fail\]\s+BC-250 GPU\s+no device PCI\\VEN_1002&DEV_13FE') 'the BC-250 GPU check fails on this computer'
Check ($r.text -match 'package integrity\s+\d+ files match manifest\.json') 'package integrity passes'
Check ($r.text -match 'Nothing was changed') 'refusal says nothing was changed'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'no change, no elevation'

'install -DryRun -DryRunIgnoreBoard (walks phases 1 and 2, prints every change)'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard')
$r.text
Check ($r.code -eq 0) "walk-through exit $($r.code)"
Check ($r.text -match 'would: bcdedit /set \{current\} testsigning on') 'phase 1 shows the test-signing change'
Check ($r.text -match 'would: pnputil /add-driver') 'phase 2 shows the driver install'
Check ($r.text -match 'would: HKLM:\\SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters: .*DpmMode=1 \(new\); DpmMaxMHz=1500 \(new\)') 'phase 2 shows DPM on, 1500 MHz'
Check ($r.text -match 'Parameters: .*UnconfirmedStarts=0') 'UnconfirmedStarts reset (installer-owned)'
Check ($r.text -match 'previous installer defaults: the defaults of tester\.1 to tester\.7 \(no record\)') 'no AppliedDefaults record on this computer: legacy table'
Check ($r.text -match 'would: .*\\Release: .*AppliedDefaults') 'Release\AppliedDefaults recorded'
Check ($r.text -match "would: scheduled task 'amdgpu-wddm start confirm'") 'phase 2 shows the start-confirm task'
Check ($r.text -match 'would: copy payload\\control') 'phase 2 installs the control application'
Check ($r.text -match 'would: copy licenses\\ and THIRD-PARTY\.md -> .+\\licenses') 'phase 2 installs the licence texts'
Check ($r.text -match 'would: Start menu shortcut .*amdgpu-wddm Control\.lnk') 'phase 2 shows the Start menu shortcut'
$dwm = [int]$m.defaults.desktop_router.DwmForceCpu
Check ($r.text -match "DesktopRouter: DwmForceCpu=$dwm \(new\); RequireKmdSwitches=1 \(new\); CpuUmdPath=") "desktop route DwmForceCpu $dwm from the defaults table"
Check ($r.text -match 'AppRouter: Mode=gpu-default \(new\); Allow=\[dxdiag\.exe\] \(new\)') 'D3D11 gpu-default from the defaults table'
$tbl = Get-Content -LiteralPath (Join-Path $Package 'installer\registry-defaults.json') -Raw | ConvertFrom-Json
Check (($m.defaults | ConvertTo-Json -Depth 6 -Compress) -eq ($tbl.defaults | ConvertTo-Json -Depth 6 -Compress)) 'manifest.json defaults = installer\registry-defaults.json defaults (one table)'
Check (@('EnableGpuPresentBlit', 'EnableCddDwmInterop', 'DpmMode', 'DpmMaxMHz' | Where-Object { $null -eq $m.defaults.parameters.$_ }).Count -eq 0 -and $null -ne $m.defaults.desktop_router.DwmForceCpu) 'manifest defaults carry the five values the control application resets'

Check ($r.text -match 'Dry run complete') 'walk-through completes'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'walk-through: no change, no elevation'

'registry defaults on upgrade (test-registry-defaults.ps1 under 5.1, HKCU scratch key)'
$rr = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-registry-defaults.ps1'), '-Installer', (Join-Path $Package 'installer'))
$rr.text
Check ($rr.code -eq 0) "set / same / update / kept / command line / installer-owned: exit $($rr.code)"

Check ($r.text -match 'would: copy payload\\system32\\bc250umd\.dll .*same SHA256: kept; in use: replaced by rename') 'stub copy is the safe replacement'
# BD-064: 32-bit processes get the x86 builds: D3D9/10/11 slots (no x86 D3D12), the x86 Vulkan ICD, the router's Wow paths.
Check ($r.text -match 'would: copy payload\\wow64 -> .+\\wow64 ') 'phase 2 installs the x86 builds (wow64)'
Check ($r.text -match 'would: copy payload\\syswow64\\bc250umd\.dll -> .+\\SysWOW64\\bc250umd\.dll .*same SHA256: kept; in use: replaced by rename') 'the x86 stub goes to SysWOW64 by the safe replacement'
Check ($r.text -match 'would: .+ UserModeDriverNameWow = bc250umd\.dll \| [^|]+\\wow64\\desktop\\bc250d3d_router\.dll \| [^|]+\\wow64\\desktop\\bc250d3d_router\.dll; VulkanDriverNameWow = [^;\r\n]+\\wow64\\vulkan\\radeon_icd\.json') 'UserModeDriverNameWow: stub and two x86 routers, no D3D12 slot; VulkanDriverNameWow'
Check ($r.text -match "would: HKLM:\\SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers '[^']+\\wow64\\vulkan\\radeon_icd\.json' = 0") 'the x86 ICD in the WOW6432Node Khronos list'
Check (($r.text -match 'DesktopRouter: .*CpuUmdPathWow=[^;\r\n]+\\wow64\\desktop\\bc250d3d\.dll') -and ($r.text -match 'AppRouter: .*GpuUmdPathWow=[^;\r\n]+\\wow64\\d3d11\\amdgpu_wddm_d3d11\.dll')) 'the x86 router paths are installer-owned'
# The H.264 encoder MFT (M15.11, driver/umd/mft-h264/INSTALL.md): manifest.json "mft_h264" says whether this release
# registers the transform, and the dry run shows every key it would write, or that it writes none.
$mftPath = 'payload/mft/amdgpu_wddm_mft_h264.dll'
$mftOn = [bool]($m.mft_h264 -and $m.mft_h264.register)
Check ((-not $m.mft_h264) -or (($m.mft_h264.clsid -eq '{A32438F0-0D79-4CA9-A5BF-9F3C80837253}') -and ($m.mft_h264.package_path -eq $mftPath) -and ($m.mft_h264.friendly_name -eq 'BC-250 H.264 Encoder MFT'))) "manifest mft_h264: register $mftOn, class id and name as INSTALL.md gives them"
if ($mftOn) {
    Check (@($m.files | Where-Object { $_.path -eq $mftPath }).Count -eq 1) "the package carries $mftPath"
    Check (@($m.components | Where-Object { $_.package_path -eq $mftPath -and $_.install_path -eq '<InstallDir>\mft\amdgpu_wddm_mft_h264.dll' }).Count -eq 1) 'manifest components: the encoder goes to <InstallDir>\mft'
    Check ($r.text -match 'would: copy payload\\mft -> [^\r\n]+\\mft \(same SHA256') 'phase 2 installs the encoder DLL'
    Check ($r.text -match 'H\.264 encoder MFT: BC-250 H\.264 Encoder MFT \{A32438F0-0D79-4CA9-A5BF-9F3C80837253\}, registration values from payload/mft/amdgpu_wddm_mft_h264\.dll') 'the registration values come out of the shipped DLL'
    Check ($r.text -match 'would: register the H\.264 encoder MFT: HKLM:\\SOFTWARE\\Classes\\CLSID\\\{A32438F0-0D79-4CA9-A5BF-9F3C80837253\} = ''BC-250 H\.264 Encoder MFT'', InprocServer32 = [^\r\n]+\\mft\\amdgpu_wddm_mft_h264\.dll \(ThreadingModel Both\)') 'the plan names the COM server key and its DLL path'
    Check ($r.text -match 'MediaFoundation\\Transforms\\A32438F0-0D79-4CA9-A5BF-9F3C80837253: MFTFlags 0x00000006, InputTypes \d+ bytes, OutputTypes \d+, Attributes \d+;') 'the plan names MFTFlags 0x00000006 and the three binary values with their sizes'
    Check ($r.text -match 'MediaFoundation\\Transforms\\Categories\\F79EAC7D-E545-4387-BDEE-D647D7BDE42A\\A32438F0-0D79-4CA9-A5BF-9F3C80837253 \(the key is the category membership') 'the plan names the category membership key'
    Check ($r.text -notmatch 'WOW6432Node\\Classes') 'no 32-bit registration of the encoder (the DLL is x64)'
} else {
    Check ($r.text -match 'H\.264 encoder MFT: not registered by this release') 'the dry run says this release does not register the encoder'
    Check ($r.text -notmatch 'would: copy payload\\mft|would: register the H\.264 encoder MFT') 'nothing of the encoder is installed'
}
'H.264 encoder MFT registration (test-mft-h264.ps1 under 5.1, HKCU scratch key)'
$rm = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-mft-h264.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Dll', (Join-Path $Package ($mftPath -replace '/', '\')))
$rm.text
Check ($rm.code -eq 0) "install, repair, uninstall and rollback of the encoder keys: exit $($rm.code)"
'32-bit registration (test-wow64.ps1 under 5.1, HKCU scratch key)'
$rw = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-wow64.ps1'), '-Package', $Package, '-WorkRoot', $WorkBase)
$rw.text
Check ($rw.code -eq 0) "x86 images, install paths, exports, verify's registration check: exit $($rw.code)"
Check ($r.text -match 'C:\\BC250 itself is not changed') 'firmware step leaves C:\BC250 itself alone'
Check ($r.text -match '\[ok\s*\]\s+GPU firmware\s+download from linux-firmware [0-9a-f]{40}: git\.kernel\.org, gitlab\.com reachable') 'preflight: both firmware download hosts reachable'
Check ($r.text -match 'would: get the 9 GPU firmware files from linux-firmware [0-9a-f]{40} into ') 'phase 2 shows the firmware download'
foreach ($f in @($m.firmware.files)) {
    $u1 = [regex]::Escape("https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/$($f.path)?id=$($m.firmware.commit)")
    $u2 = [regex]::Escape("https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$($m.firmware.commit)/$($f.path)")
    Check (($r.text -match "firmware $([regex]::Escape($f.name))\s+SHA256 $($f.sha256)") -and ($r.text -match $u1) -and ($r.text -match $u2)) "dry run prints $($f.name): SHA256 and both URLs"
}
Check ($r.text -notmatch '\S+ SHA256 [0-9A-F]{64} ok ') 'dry run downloads nothing'
$pos = @('Get-FirmwareStaged -Firmware', '$script:InPhase2 = $true', 'Import-Certificate') | ForEach-Object { $src0 = [IO.File]::ReadAllText((Join-Path $Package 'installer\install.ps1')); $src0.IndexOf($_) }
Check ($pos[0] -gt 0 -and $pos[0] -lt $pos[1] -and $pos[1] -lt $pos[2]) 'the firmware is downloaded and checked before phase 2 changes anything'
$src = [IO.File]::ReadAllText((Join-Path $Package 'installer\install.ps1'))
Check ($src -notmatch "icacls\.exe @\('C:\\BC250'") 'no icacls on C:\BC250 itself'
Check ($src -notmatch 'Copy-Item') 'install.ps1 copies only through Copy-FileSafe / Copy-TreeSafe'
Check (($src -match "Add-Result 'GPU desktop path'") -and ($src -match "Invoke-Native \`$cli @\('interop'\)") -and ($src -match 'effective blit\\\+cdd') -and ($src -match 'died in a session') -and ($src -match 'bc250d3d_zink')) 'verify checks the GPU desktop path (interop effective blit+cdd, no unclean session, zink in DWM)'
$pos = @("Read-RegistryValues `$script:ParametersKey", "Invoke-Change 'pnputil /add-driver", "Invoke-RegistryDefaults `$script:ParametersKey") | ForEach-Object { $src.IndexOf($_) }
Check (($pos[0] -gt 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($src -match "parameters_before_install") -and ($src -match '\$parametersBefore \$restoreNames')) 'driver settings are read before pnputil (the INF resets the gates) and judged from that snapshot'
Check ($r.text -match 'driver settings before the driver package: \d+ values under Parameters, \d+ of \d+ judged by this release') 'the walk-through takes the snapshot before pnputil'
# The snapshot keeps the whole key, and every value that this release does not judge goes back as it was. A snapshot
# of the judged names alone dropped CuMode at every release install, and the GPU then ran on 24 of its 40 compute
# units (test-registry-defaults.ps1 has the case).
Check (($src -match 'foreach \(\$n in \$all\.Keys\) \{ \$parametersBefore\[\$n\] = \$all\[\$n\] \}') -and ($src -match '\$restoreNames = @\(@\(\$infParameterNames\) \+ @\(\$parametersBefore\.Keys \| Where-Object \{ \$_ -notin \$judgedNames -and \(Test-RestorableRegistryValue')) 'the snapshot keeps every value of the Parameters key, and the ones outside the table are written back'
# The encoder registration is reported where a support report can read it (BugReport.cs collects every verify report).
Check (($src -match "Add-Result 'H\.264 encoder'") -and ($src -match "Add-Warning 'H\.264 encoder'") -and ($src -match 'Test-MftRegistration -ClassesKey \$script:ClassesKey -DllPath \$mftInstalled')) 'verify reports the H.264 encoder registration, and a registration that is not right is a warning'
Check (($src -match "Add-Result 'full WDDM gate'") -and ($src -match 'EnableFullWddm -eq 1\) -or \(\$p\.EnableFullWddm -eq 2\)')) 'verify fails a display-only start (EnableFullWddm not 1 or 2)'
$common = [IO.File]::ReadAllText((Join-Path $Package 'installer\common.ps1'))
Check (($src -match '(?m)^\s+Set-StateDirAccess\s*$') -and ($common -match "\*S-1-5-32-545:\(OI\)\(CI\)RX") -and ($common -match "'/reset', '/T'") -and ($common -match 'function Set-StateDirAccess \{\s+if \(\$script:DryRunMode\) \{ return \}')) 'the state folder (logs, verify results) gets administrators/SYSTEM full and users read, children reset; not in a dry run'
Check (([string]$m.kmd_abi -eq '0x000700CF' -and [version]($m.kmd_build) -ge [version]'0.7.207.0') -or ([string]$m.kmd_abi -eq '0x000700CE' -and [version]($m.kmd_build) -ge [version]'0.7.206.0' -and [version]($m.kmd_build) -lt [version]'0.7.207.0') -or ([string]$m.kmd_abi -eq '0x000700CD' -and [version]($m.kmd_build) -ge [version]'0.7.205.0' -and [version]($m.kmd_build) -lt [version]'0.7.206.0') -or ([string]$m.kmd_abi -eq '0x000700CC' -and [version]($m.kmd_build) -ge [version]'0.7.204.0' -and [version]($m.kmd_build) -lt [version]'0.7.205.0') -or ([string]$m.kmd_abi -eq '0x000700CB' -and [version]($m.kmd_build) -ge [version]'0.7.203.0' -and [version]($m.kmd_build) -lt [version]'0.7.204.0') -or ([string]$m.kmd_abi -eq '0x000700CA' -and [version]($m.kmd_build) -ge [version]'0.7.202.0' -and [version]($m.kmd_build) -lt [version]'0.7.203.0') -or ([string]$m.kmd_abi -eq '0x000700C9' -and [version]($m.kmd_build) -ge [version]'0.7.201.0' -and [version]($m.kmd_build) -lt [version]'0.7.202.0') -or ([string]$m.kmd_abi -eq '0x000700C8' -and [version]($m.kmd_build) -ge [version]'0.7.200.0' -and [version]($m.kmd_build) -lt [version]'0.7.201.0') -or ([string]$m.kmd_abi -eq '0x000700C7' -and [version]($m.kmd_build) -lt [version]'0.7.200.0')) "kmd_abi $($m.kmd_abi) for KMD build $($m.kmd_build)"

# BD-060: nothing in the installer stops or restarts DWM or the GPU under the running desktop; the driver package
# changes the GPU at the restart (INF Reboot directive), and only uninstall moves it in place (documented there).
$scripts = @(Get-ChildItem -LiteralPath (Join-Path $Package 'installer') -Filter *.ps1 | ForEach-Object { [pscustomobject]@{ name = $_.Name; text = [IO.File]::ReadAllText($_.FullName) } })
$bad = @(foreach ($s in $scripts) { foreach ($p in 'Stop-Process', 'taskkill', 'Restart-Service', 'Stop-Service', '/restart-device', '/disable-device', '/enable-device', '/remove-device', 'Disable-PnpDevice', 'Enable-PnpDevice', 'Restart-PnpDevice') { if ($s.text -match [regex]::Escape($p)) { "$($s.name): $p" } } })
Check ($bad.Count -eq 0) "no installer script stops or restarts DWM, a service or the GPU$(if ($bad.Count) { ': ' + ($bad -join '; ') })"
# The encoder DLL exports no DllRegisterServer on purpose (driver/umd/mft-h264/INSTALL.md, step 1): a driver package
# writes its COM registration itself.
$bad = @(foreach ($s in $scripts) { if ($s.text -match 'regsvr32') { $s.name } })
Check ($bad.Count -eq 0) "no installer script calls regsvr32$(if ($bad.Count) { ': ' + ($bad -join ', ') })"
$pnpCalls = @(foreach ($s in $scripts) { foreach ($mm in [regex]::Matches($s.text, "Invoke-Native pnputil\.exe @\('(/[a-z-]+)'")) { "$($s.name) $($mm.Groups[1].Value)" } })
Check ((($pnpCalls | Sort-Object) -join ', ') -eq 'common.ps1 /enum-drivers, install.ps1 /add-driver, uninstall.ps1 /delete-driver, uninstall.ps1 /scan-devices') "pnputil calls: $($pnpCalls -join ', ')"
$pos = @('Test-InfDefersDeviceRestart ([IO.File]::ReadAllLines($infFile))', "Invoke-Change 'pnputil /add-driver") | ForEach-Object { $src.IndexOf($_) }
Check (($pos[0] -gt 0) -and ($pos[0] -lt $pos[1])) 'install.ps1 refuses a package INF without the Reboot directive before pnputil'
Check ($r.text -match 'would: pnputil /add-driver payload\\kmd\\bc250kmd\.inf /install \(the GPU changes to it at the next restart\)') 'the walk-through shows the driver package for the next restart'
Check (($src -match "'observed' \{ Add-Warning 'DWM restarted in this session'") -and ($src -match 'Get-DwmReplacementFinding \(Find-DwmBaseline \(Read-DwmBaseline\) \$epoch\)') -and ($src -notmatch 'LogonUtc -DwmStartUtc|StartTime|CreationDate -gt')) 'verify reports a DWM replacement only when observed against the logon baseline, unknown history otherwise'
$sc0 = [IO.File]::ReadAllText((Join-Path $Package 'installer\start-confirm.ps1'))
Check (($sc0 -match 'if \(-not \$Probe\) \{\s+try \{\s+\. \(Join-Path \$here ''dwm-session\.ps1''\)') -and ($sc0 -match 'Save-DwmBaseline \$epoch')) 'the start-confirm task records the DWM baseline at each logon (not in -Probe)'
Check ((Test-Path -LiteralPath (Join-Path $Package 'payload\tools\dwm-session.ps1')) -and ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\dwm-session.ps1')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'installer\dwm-session.ps1')).Hash)) 'dwm-session.ps1 ships next to the installed start-confirm task'
Check ($r.text -match 'DWM of session \d+ before the driver package: ') 'the walk-through records the DWM before the driver package'
# BD-069: the installer tells the driver's own safety closure from a setting of the tester, and only a repair reopens
# one. The records belong to the KMD (driver/kmd/interop.c, driver/kmd/dpm.c); the installer reads them.
Check (($common -match '\$script:DriverClosures = @\{') -and ($common -match "record = 'InteropClosedReason'") -and ($common -match "record = 'DpmClosedReason'") -and ($common -match "legacy_record = 'DpmLastReason'") -and ($common -match "'reopened'") -and ($common -match "'driver-closed'")) 'common.ps1 holds the driver closure table (InteropClosedReason, DpmClosedReason with DpmLastReason as the legacy record) and both decisions'
Check (($src -match "(?m)^\`$script:ReopenClosures = \[bool\]\`$Repair\s*$") -and ([regex]::Matches($src, '\$script:ReopenClosures =').Count -eq 1) -and ([regex]::Matches($src, '-Reopen:\$script:ReopenClosures').Count -eq 2) -and ($common -match "InstallSwitches = @\('NoControlApp', 'NoReboot', 'Force', 'Repair'\)")) 'only the -Repair switch reopens a closure (the plan and the write both), and -Repair survives the restart'
Check (($common -match "8 = 'smu-error'") -and ($src -match "\`$ioRemedy = '; remedy: run install\.cmd -Repair") -and ($src -notmatch "if \(\`$ioClosed\) \{ \`$ioClosed \+= '; remedy")) 'every reason the DPM guard persists is a closure (3, 4, 8), and verify names the repair only where the driver left a record'
Check (($common -match "legacy_record = 'DpmLastReason'; any_reason = \`$true") -and ($common -match "\(\`$record -eq \`$c\.record\) -and \`$c\.any_reason")) 'a durable record takes any reason the driver writes; only the legacy record is held to the three names'
$marker = @(foreach ($s in $scripts) { if ((@($s.text -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n") -match 'InteropSession|InteropBoot') { $s.name } })
Check ($marker.Count -eq 0) "the BD-059 session marker stays under KMD ownership: no installer code names it$(if ($marker.Count) { ': ' + ($marker -join ', ') })"
$pos = @("Invoke-Change 'pnputil /add-driver", "Add-DwmObservation 'after the driver package'", "Save-Phase 'driver-pending-restart'", "Add-DwmObservation 'end of phase 2'", "Save-Phase 'installed'") | ForEach-Object { $src.IndexOf($_) }
Check (($pos[0] -gt 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($pos[2] -lt $pos[3]) -and ($pos[3] -lt $pos[4]) -and ($src -match "Set-StateValue \`$state 'dwm_observations'")) 'DWM observations before and right after the driver package (ahead of the restart-pending branch) and at the end of phase 2 go into the state'
$pos = @('$installInputs = Get-InstallInputs $PSBoundParameters $early', '$was = Set-InstallPackage $state $packageVersion', 'Save-InstallInputs $state $installInputs', "if (`$state.phase -eq 'new')", "Save-Phase 'driver-pending-restart'", 'Clear-InstallInputs $state', "Save-Phase 'installed'") | ForEach-Object { $src.IndexOf($_) }
Check ((@($pos | Where-Object { $_ -lt 0 }).Count -eq 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($pos[2] -lt $pos[3]) -and ($pos[3] -lt $pos[4]) -and ($pos[4] -lt $pos[5]) -and ($pos[5] -lt $pos[6]) -and ([regex]::Matches($src, 'Clear-InstallInputs').Count -eq 1)) 'install inputs: restored first, bound to the package that takes an unfinished install over, saved before any restart for every install action, cleared only when phase 2 completes'
'BD-060 session rules (test-session-checks.ps1 under 5.1, HKCU scratch key)'
$rs = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-session-checks.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Inf', (Join-Path $Package 'payload\kmd\bc250kmd.inf'), '-WorkRoot', $WorkBase)
$rs.text
Check ($rs.code -eq 0) "INF Reboot directive, pnputil outcomes, install inputs across a restart, DWM baseline: exit $($rs.code)"

'file replacement and re-run (test-filesafe.ps1 under 5.1, inside a scratch folder)'
$work = Join-Path $WorkBase ('filesafe-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-filesafe.ps1'), '-Installer', (Join-Path $Package 'installer'), '-WorkRoot', $work)
$r.text
Check ($r.code -eq 0) "equal-hash skip, in-use replacement, partial-install re-run: exit $($r.code)"
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-filesafe.ps1'), '-Installer', (Join-Path $Package 'installer'), '-WorkRoot', "$work-trap", '-Case', 'trap')
$r.text
Check ($r.code -eq 6) "a throwing step exits 6 ($($r.code))"
Check ($r.text -match 'stopped at step: second test step \(throws\)') 'the failure names the step'
Check ($r.text -match 'run install\.cmd again from the same package folder') 'the failure gives the re-run hint'
Check ($r.text -notmatch 'not reached') 'nothing after the failed step ran'
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }

'upgrade and repair over an existing installation (dry runs over a test state file, AMDGPU_WDDM_TEST_STATE_DIR)'
$pkgVersion = [string]$m.version
$cases = @(
    @{ name = 'upgrade from verify-failed'; phase = 'verify-failed'; version = '0.7.197.100-tester.2'; extra = @(); expect = "upgrading 0\.7\.197\.100-tester\.2 -> $([regex]::Escape($pkgVersion)) \(installed phase verify-failed\)"; phase2 = $true }
    @{ name = 'upgrade from verified, older'; phase = 'verified'; version = '0.7.197.100-tester.1'; extra = @(); expect = "upgrading 0\.7\.197\.100-tester\.1 -> $([regex]::Escape($pkgVersion)) \(installed phase verified\)"; phase2 = $true }
    @{ name = 'repair: same version, install-incomplete'; phase = 'install-incomplete'; version = $pkgVersion; extra = @(); expect = "repairing $([regex]::Escape($pkgVersion)) \(phase install-incomplete\)"; phase2 = $true }
    @{ name = 'same version, verified'; phase = 'verified'; version = $pkgVersion; extra = @(); expect = "$([regex]::Escape($pkgVersion)) is already installed and verified"; phase2 = $false }
    @{ name = 'same version, verified, -Repair'; phase = 'verified'; version = $pkgVersion; extra = @('-Repair'); expect = "repairing $([regex]::Escape($pkgVersion)) \(-Repair, phase verified\)"; phase2 = $true }
)
$n = 0
foreach ($c in $cases) {
    $n++
    $dir = Join-Path $WorkBase ('state-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + "-$n")
    [void][IO.Directory]::CreateDirectory($dir)
    $st = [ordered]@{ schema = 1; phase = $c.phase; package_version = $c.version; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); testsigning_set_by_installer = $true; updated_utc = '2026-10-03T00:00:00Z' }
    [IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
    $env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
    try { $r = Invoke-Ps51 (@((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') + $c.extra) } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq 0) "$($c.name): exit $($r.code)"
    Check ($r.text -match $c.expect) "$($c.name): '$($c.expect)'"
    if ($c.phase2) {
        Check (($r.text -match 'would: copy payload\\tools') -and ($r.text -match 'would: pnputil /add-driver') -and ($r.text -match 'Dry run complete')) "$($c.name): phase 2 runs again"
        Check ($r.text -notmatch 'would: bcdedit /set') "$($c.name): phase 1 (test signing) is not repeated"
        Check ($r.text -match "would: RunOnce entry 'amdgpu-wddm-installer' -> .*powershell\.exe`" -NoProfile -ExecutionPolicy Bypass -File `"[^`"]*\\installer\\install\.ps1`" -HoldWindow -Verify") "$($c.name): RunOnce verify armed again (Windows PowerShell, no cmd.exe)"
        Check ($r.text -match 'would: .*\\Release: Version') "$($c.name): Release\Version rewritten"
    } else {
        Check ($r.text -notmatch 'would: copy payload|would: pnputil') "$($c.name): nothing is installed"
    }
    if ($r.code -ne 0 -or $r.text -notmatch $c.expect) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

Check ($src -notmatch 'New-Item -Path [^\r\n]*-Force') 'install.ps1 never runs New-Item -Force on a registry key (it deletes the key''s values)'

'verify before and after the restart (test state, phase installed, the boot it was saved in)'
$thisBoot = $null
$v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters' -Name BootId -ErrorAction SilentlyContinue).BootId
if ($null -ne $v) { $thisBoot = [int64][BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$v), 0) }
foreach ($c in @(
        @{ name = 'verify before the restart'; utc = [DateTime]::UtcNow.ToString('o'); boot = $thisBoot; code = 7; expect = 'has not been confirmed: this is still the boot in which it was asked for' }
        @{ name = 'verify after the restart'; utc = '2000-01-01T00:00:00.0000000Z'; boot = $thisBoot + 1; code = 3; expect = 'BC-250 GPU not found' })) {
    $dir = Join-Path $WorkBase ('state-verify-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
    [void][IO.Directory]::CreateDirectory($dir)
    $st = [ordered]@{ schema = 1; phase = 'installed'; package_version = $pkgVersion; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = $c.utc; restart_boot_id = $c.boot }
    [IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
    $env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
    try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-Verify') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq $c.code) "$($c.name): exit $($r.code) (expected $($c.code))"
    Check ($r.text -match $c.expect) "$($c.name): '$($c.expect)'"
    Check ($r.text -notmatch 'waiting for the start-confirm task') "$($c.name): no wait for the task"
    $reports = @(Get-ChildItem -LiteralPath (Join-Path $dir 'verify') -Filter 'verify-*.json' -ErrorAction SilentlyContinue)
    if ($c.code -eq 3) {
        # A failed verify writes a matching failed report: identity of one package and a non-empty result list.
        $vr = $(if ($reports.Count -eq 1) { Get-Content -LiteralPath $reports[0].FullName -Raw | ConvertFrom-Json })
        Check (($null -ne $vr) -and ($vr.schema -eq 'amdgpu-wddm.verify-report/1') -and ($vr.outcome -eq 'failed') -and ($vr.package_version -eq $pkgVersion) -and ($vr.release -eq $m.name) -and ($vr.manifest_sha256 -match '^[0-9A-F]{64}$') -and (@($vr.results).Count -ge 1) -and (@($vr.results | Where-Object { $_.pass -eq $false }).Count -ge 1) -and ($vr.failed -ge 1)) "$($c.name): one failed verify report bound to $pkgVersion with its failing result"
    } else {
        Check ($reports.Count -eq 0) "$($c.name): no verify report (no check ran)"
    }
    if ($r.code -ne $c.code) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

'start-confirm.ps1 -Probe (read-only; compiles the bc250control.dll interop and calls Bc250StartHealth READ)'
$r = Invoke-Ps51 @((Join-Path $Package 'payload\tools\start-confirm.ps1'), '-Probe')
$r.text
Check ($r.code -eq 5) "probe on a PC without the driver: exit $($r.code) (5 = no start-health reading)"
# The package has no manifest.json next to payload\tools\, so the probe uses the built-in expected version of
# start-confirm.ps1. This gate therefore holds that constant at the release's kmd_abi: an install whose manifest.json
# cannot be read must still expect the driver version this package carries.
Check ($r.text -match "probe: fallback view: device problem -1, driver version  \(expected $($m.kmd_abi)\), LastStage ") "probe: fallback view runs bc250kmd_cli info/stages; the built-in expected version is this release's kmd_abi $($m.kmd_abi)"
Check ($r.text -match 'probe: start health no reading: start health read refused, status 0x[0-9A-F]{8}') 'probe: the DLL loads and Bc250StartHealth answers (no device)'
Check ($r.text -match 'probe: DpmMode no key') 'probe: DPM state read from the registry'
$sc = [IO.File]::ReadAllText((Join-Path $Package 'payload\tools\start-confirm.ps1'))
Check ($sc -notmatch "cli health|& `\$cli health") 'start-confirm reads the start health through bc250control.dll, not the CLI'
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-cli-commands.ps1'), '-Cli', (Join-Path $Package 'payload\tools\bc250kmd_cli.exe'), '-Sources', (Join-Path $PSScriptRoot 'release-sources.json'))
$r.text
Check ($r.code -eq 0) "packaged bc250kmd_cli.exe answers every form of cli-commands.json, one build with the control DLL: exit $($r.code)"
Check ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\bc250control.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'payload\control\bc250control.dll')).Hash) 'tools\bc250control.dll is the control application''s DLL'

'GPU firmware: not in the package, downloaded at install time (test-firmware.ps1 under 5.1, real download into a scratch folder)'
Check (-not (Test-Path -LiteralPath (Join-Path $Package 'payload\firmware'))) 'no payload\firmware in the package'
Check (-not @(Get-ChildItem -LiteralPath $Package -Recurse -File -Filter '*.bin').Count) 'no .bin file in the package'
Check (-not @($m.files | Where-Object { $_.path -like 'payload/firmware/*' }).Count) 'manifest files: no firmware'
Check (@($m.firmware.files).Count -eq 9 -and [string]$m.firmware.commit -match '^[0-9a-f]{40}$') "manifest firmware: $(@($m.firmware.files).Count) files at $($m.firmware.commit)"
Check (@($m.components | Where-Object { $_.role -eq 'firmware' -and $_.install_path -like 'C:\BC250\firmware\*' -and -not $_.package_path }).Count -eq 9) 'manifest components: 9 firmware files with install path and SHA256, no package path'
$fwWork = Join-Path $WorkBase ('firmware-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test-firmware.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Manifest', (Join-Path $Package 'manifest.json'), '-WorkRoot', $fwWork) -TimeoutSeconds 600
"  (pid $($r.pid), exit $($r.code), no window)"
$r.text
Check ($r.code -eq 0) "firmware download, fallback, offline folder and wrong-hash refusal: exit $($r.code)"
Check (([regex]::Matches($r.text, 'SHA256 [0-9A-F]{64} ok  https://git\.kernel\.org/')).Count -eq 9) 'all 9 files came from the first address (git.kernel.org), not only from the fallback'

'install -DryRun -DryRunIgnoreBoard -FirmwareDir (offline): a good folder, then a folder with a changed file'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-FirmwareDir', (Join-Path $fwWork 'download'))
Check ($r.code -eq 0) "good folder: exit $($r.code)"
Check ($r.text -match '\[ok\s*\]\s+GPU firmware\s+9 files in .+ match the pinned SHA256') 'good folder: preflight checks every SHA256'
Check ($r.text -match 'would: get the 9 GPU firmware files from .+download into ') 'good folder: phase 2 takes the files from the folder'
Check ($r.text -notmatch 'https://') 'good folder: no download address used'
if ($r.code -ne 0) { $r.text }
$badDir = Join-Path $fwWork 'bad'
[void][IO.Directory]::CreateDirectory($badDir)
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $fwWork 'download') -File) { Copy-Item -LiteralPath $f.FullName -Destination $badDir }
[IO.File]::AppendAllText((Join-Path $badDir 'cyan_skillfish2_me.bin'), 'x')
Remove-Item -LiteralPath (Join-Path $badDir 'LICENSE.amdgpu')
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-FirmwareDir', $badDir)
Check ($r.code -eq 2) "changed folder: exit $($r.code) (2 = preflight refusal)"
Check ($r.text -match '\[fail\]\s+GPU firmware\s+-FirmwareDir .+cyan_skillfish2_me\.bin has another SHA256; LICENSE\.amdgpu missing') 'changed folder: the refusal names the changed and the missing file'
Check ($r.text -match 'Nothing was changed') 'changed folder: nothing was changed'
if ($r.code -ne 2) { $r.text }

'continuation after the driver package restart without arguments (RunOnce): an offline fresh install and an upgrade'
$goodDir = Join-Path $fwWork 'download'
$n = 0
foreach ($c in @(
        @{ name = 'fresh offline install'; st = [ordered]@{ previous_service = 'BasicDisplay'; testsigning_set_by_installer = $true } }
        @{ name = 'upgrade'; st = [ordered]@{ previous_package_version = '0.7.198.100-tester.10' } })) {
    $n++
    $dir = Join-Path $WorkBase ('state-resume-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + "-$n")
    [void][IO.Directory]::CreateDirectory($dir)
    $st = [ordered]@{ schema = 1; phase = 'driver-pending-restart'; package_version = $pkgVersion; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2026-10-03T00:00:00Z'
        firmware_source_dir = $goodDir; command_line_parameters = [ordered]@{ DpmMaxMHz = 1200 }; install_switches = @('NoControlApp')
        restart_boot_id = $(if ($null -ne $thisBoot) { $thisBoot - 1 } else { 0 }) }   # the restart happened (R1: only a new boot continues)
    foreach ($k in $c.st.Keys) { $st[$k] = $c.st[$k] }
    [IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json -Depth 4))
    $env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
    try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq 0) "$($c.name): exit $($r.code)"
    Check ($r.text -match "from the first run of this install \(driver-pending-restart\): -FirmwareDir $([regex]::Escape($goodDir)) -DpmMaxMHz 1200 -NoControlApp") "$($c.name): the folder, the setting and the switch come from the state"
    Check (($r.text -match '\[ok\s*\]\s+GPU firmware\s+9 files in .+ match the pinned SHA256') -and ($r.text -match 'would: get the 9 GPU firmware files from .+download into ') -and ($r.text -notmatch 'https://')) "$($c.name): the firmware comes from the first run's folder, no download"
    Check ($r.text -match 'DpmMaxMHz=1200 \(command line\)') "$($c.name): the first run's -DpmMaxMHz is written"
    Check (($r.text -match 'control application: not in this package \(or -NoControlApp\); skipped') -and ($r.text -notmatch 'would: copy payload\\control')) "$($c.name): -NoControlApp still holds"
    if ($r.code -ne 0) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}
$dir = Join-Path $WorkBase ('state-upgrade-args-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
[void][IO.Directory]::CreateDirectory($dir)
[IO.File]::WriteAllText((Join-Path $dir 'state.json'), ([ordered]@{ schema = 1; phase = 'verified'; package_version = '0.7.198.100-tester.10'; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2026-10-03T00:00:00Z' } | ConvertTo-Json))
$env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-FirmwareDir', $goodDir, '-CuMode', '40', '-NoControlApp') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
Check (($r.code -eq 0) -and ($r.text -match "options of this install, kept until it completes: -FirmwareDir $([regex]::Escape($goodDir)) -CuMode 40 -NoControlApp") -and ($r.text -notmatch 'from the first run of this install')) "upgrade with arguments: its options are kept for the continuation (exit $($r.code))"
if ($r.code -ne 0) { $r.text }
Remove-Item -LiteralPath $dir -Recurse -Force
# This package over a tester.10 phase 1 that did not finish, with its own arguments: it takes the installation over
# (the state names it, so its inputs come back after its restarts), and test signing is still finished first.
foreach ($c in @(
        @{ phase = 'testsigning-active'; code = 0 }
        @{ phase = 'testsigning-pending'; code = 5 })) {
    $dir = Join-Path $WorkBase ('state-takeover-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
    [void][IO.Directory]::CreateDirectory($dir)
    [IO.File]::WriteAllText((Join-Path $dir 'state.json'), ([ordered]@{ schema = 1; phase = $c.phase; package_version = '0.7.198.100-tester.10'; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); testsigning_set_by_installer = $true; firmware_source_dir = 'C:\tester10-fw'; updated_utc = '2026-10-03T00:00:00Z' } | ConvertTo-Json))
    $env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
    try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-FirmwareDir', $goodDir, '-NoControlApp') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq $c.code) "takeover at $($c.phase): exit $($r.code) (expected $($c.code))"
    Check (($r.text -match "this package \($([regex]::Escape($pkgVersion))\) takes over the unfinished installation of 0\.7\.198\.100-tester\.10 \(phase $($c.phase)\)") -and ($r.text -match "options of this install, kept until it completes: -FirmwareDir $([regex]::Escape($goodDir)) -NoControlApp") -and ($r.text -notmatch 'tester10-fw')) "takeover at $($c.phase): the state names this package with its own options"
    if ($c.phase -eq 'testsigning-pending') {
        # The run after the restart is this package's, from its continuation closure (GUI plan A1), not the folder it started from.
        $closureCmd = [regex]::Escape('powershell.exe" -NoProfile -ExecutionPolicy Bypass -File ' + (Format-RunOnceArg (Join-Path $dir "packages\$pkgVersion\installer\install.ps1")) + ' -HoldWindow')
        Check (($r.text -match 'Test signing is set but not active yet') -and ($r.text -match "would: RunOnce entry 'amdgpu-wddm-installer' -> .*$closureCmd") -and ($r.text -match "would: stage the continuation closure: every file of this package and the firmware folder $([regex]::Escape($goodDir))") -and ($r.text -notmatch 'would: pnputil')) 'takeover at testsigning-pending: stops for the test-signing restart, and the run after it is this package''s, from its closure'
    } else {
        Check (($r.text -match 'would: pnputil /add-driver') -and ($r.text -notmatch 'would: bcdedit /set')) 'takeover at testsigning-active: phase 2 runs, test signing is not set again'
    }
    if ($r.code -ne $c.code) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

# The setup window's contract with the engine (GUI plan, docs/gui/interfaces-setup.md).
'engine units: RunOnce command line, closure, repair set, witness, compatibility record, lock (test-engine-units.ps1 under 5.1)'
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-engine-units.ps1'), '-Installer', (Join-Path $Package 'installer'), '-WorkRoot', $WorkBase)
$r.text
Check ($r.code -eq 0) "G-STAGE units, witness writer, C7 record, engine lock: exit $($r.code)"
$pwsh = (Get-Process -Id $PID).Path
'G-EVT and G-STAGE: events and terminal result of the engine (test-engine-events.ps1)'
$r = Invoke-Headless -File $pwsh -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'test-engine-events.ps1'), '-Package', $Package, '-WorkRoot', $WorkBase, '-FirmwareDir', $goodDir) -TimeoutSeconds 1200
$r.text
Check ($r.code -eq 0) "G-EVT, G-STAGE: exit $($r.code)"
'G-OFF: prepared folder and kept repair set with the network unavailable (test-offline.ps1)'
$r = Invoke-Headless -File $pwsh -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'test-offline.ps1'), '-Package', $Package, '-WorkRoot', $WorkBase, '-FirmwareDir', $goodDir) -TimeoutSeconds 1800
$r.text
Check ($r.code -eq 0) "G-OFF: exit $($r.code)"
# The setup window, when the package carries it, against this package's real engine: a plan run and a dry run through
# the window's own engine client, each screen in four languages without internals (--smoke-engine shows no window).
$setupExe = Join-Path $Package 'setup\amdgpu_wddm_setup.exe'
if (Test-Path -LiteralPath $setupExe) {
    'setup window against the real engine (--smoke-engine: plan and dry run, no window)'
    foreach ($c in @(
            @{ name = 'plan'; flags = @('--plan'); engine = @('-Plan', '-DryRunIgnoreBoard', '-FirmwareDir', $goodDir); expect = @('result: bound', 'outcome: planned', 'view: Plan result.planned.title nothing-changed', 'decision: install') }
            @{ name = 'dry run'; flags = @(); engine = @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning', '-FirmwareDir', $goodDir); expect = @('result: bound', 'message: result.dry-run-complete', 'mutated: false', 'view: Information result.dry-run-complete.title') })) {
        $out = Join-Path $WorkBase ('setup-smoke-' + ($c.name -replace ' ', '-') + '-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
        $stateDir = "$out-state"
        [void][IO.Directory]::CreateDirectory($stateDir)
        $env:AMDGPU_WDDM_TEST_STATE_DIR = $stateDir
        try { $r = Invoke-Headless -File $setupExe -Arguments (@('--smoke-engine', $Package, $out) + $c.flags + @('--') + $c.engine) -TimeoutSeconds 900 } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
        $summary = [string](Get-Content -LiteralPath (Join-Path $out 'summary.txt') -Raw -ErrorAction SilentlyContinue)
        $missing = @($c.expect | Where-Object { -not $summary.Contains($_) })
        $screens = @([regex]::Matches($summary, '(?m)^screen (en|pl|ja|ko): \w+; internals none; missing strings none').Count)
        Check (($r.code -eq 0) -and ($missing.Count -eq 0)) "setup window $($c.name): exit $($r.code)$(if ($missing.Count) { '; missing: ' + ($missing -join ', ') })"
        Check ($summary -match '(?m)^unknown checks: \r?$') "setup window $($c.name): every check the engine reported has words"
        Check ($summary -match '(?m)^events: \d+ ignored 0 problems 0') "setup window $($c.name): events in order, none ignored"
        Check ($screens[0] -eq 4) "setup window $($c.name): the screen in EN, PL, JA and KO without internals or missing strings"
        if (($r.code -ne 0) -or $missing.Count -or ($screens[0] -ne 4)) { $summary }
        Remove-Item -LiteralPath $stateDir -Recurse -Force
    }
    $cr0 = Get-Content -LiteralPath (Join-Path $Package 'compatibility.json') -Raw | ConvertFrom-Json
    Check (($cr0.continuation.setup_exe -eq 'setup/amdgpu_wddm_setup.exe') -and (@($m.files | Where-Object { $_.path -eq 'setup/amdgpu_wddm_setup.exe' }).Count -eq 1)) 'the compatibility record names the setup window as the continuation, manifest.json lists it'
} else { '  (no setup window in this package: skipped)' }
if (Test-Path -LiteralPath $fwWork) { Remove-Item -LiteralPath $fwWork -Recurse -Force }

'package records: compatibility, release notes, witness writer, planned restarts only'
$cr = Get-Content -LiteralPath (Join-Path $Package 'compatibility.json') -Raw | ConvertFrom-Json
Check (($cr.schema -eq 'amdgpu-wddm.compatibility/1') -and $cr.no_live_rebind.reboot_directive -and (@($m.files | Where-Object { $_.path -eq 'compatibility.json' }).Count -eq 1)) 'compatibility.json: the Reboot directive recorded, listed in manifest.json'
$notesText = Get-Content -LiteralPath (Join-Path $Package 'RELEASE-NOTES.md') -Raw
Check (@('New', 'Fixed', 'Known issues', 'Settings affected' | Where-Object { $notesText -notmatch "(?m)^## $_\s*$" }).Count -eq 0) 'RELEASE-NOTES.md has New / Fixed / Known issues / Settings affected'
Check ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\release-witness.ps1')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'installer\release-witness.ps1')).Hash) 'release-witness.ps1 ships next to the installed start-confirm task'
Check (($sc0 -match "Write-RunningReleaseWitness -InstallRoot \(Split-Path \`$here\) -RecordedBy 'start-confirm'") -and ($sc0.IndexOf('Write-RunningReleaseWitness') -gt $sc0.IndexOf('if ($Probe) {'))) 'the start-confirm task writes the running-release witness (not in -Probe)'
Check (($src -match "Write-RunningReleaseWitness -InstallRoot \`$InstallRoot -RecordedBy 'verify'")) 'verify writes the running-release witness'
$forced = @(foreach ($s in $scripts) { if ($s.text -match 'Restart-Computer|shutdown(\.exe)?\s+/r') { $s.name } })
Check ($forced.Count -eq 0) "no installer script forces a restart (planned ExitWindowsEx after the user's yes only)$(if ($forced.Count) { ': ' + ($forced -join ', ') })"

'uninstall -DryRun'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\uninstall.ps1'), '-DryRun')
$r.text
Check ($r.code -eq 0) "uninstall -DryRun exit $($r.code)"
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'uninstall dry run: no change, no elevation'
'uninstall -DryRun over a test state (BD-064: the x86 parts go too)'
$dir = Join-Path $WorkBase ('state-uninstall-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($dir)
$st = [ordered]@{ schema = 1; phase = 'verified'; package_version = [string]$m.version; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); stub_existed = $false; stub_wow_existed = $false; updated_utc = '2026-10-04T00:00:00Z' }
[IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
$env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\uninstall.ps1'), '-DryRun', '-Yes', '-KeepTestSigning') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
Check ($r.code -eq 0) "uninstall -DryRun with a state: exit $($r.code)"
Check (($r.text -match 'would: remove .+\\System32\\bc250umd\.dll') -and ($r.text -match 'would: remove .+\\SysWOW64\\bc250umd\.dll')) 'both stubs removed (neither was there before the install)'
Check ($r.text -match "would: remove '[^']+\\wow64\\vulkan\\radeon_icd\.json' from HKLM:\\SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers") 'the WOW6432Node Khronos entry removed'
# The H.264 encoder MFT keys go at every uninstall, whether this release registered the transform or not. On this
# computer none of them is there, and the step says so instead of naming keys that do not exist.
Check ($r.text -match 'would: remove the H\.264 encoder MFT registration \(no key of ours present\)') 'the encoder registration is removed at uninstall'
$mftAt = $r.text.IndexOf('would: remove the H.264 encoder MFT registration')
$rootAt = $r.text.IndexOf('would: remove ' + (Join-Path $env:ProgramFiles 'amdgpu-wddm'))
Check (($mftAt -gt 0) -and ($rootAt -gt $mftAt)) 'the encoder keys go before the install root, so no COM registration points at a DLL that is gone'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'uninstall dry run with a state: no change, no elevation'
if ($r.code -ne 0) { $r.text }
Remove-Item -LiteralPath $dir -Recurse -Force
$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
