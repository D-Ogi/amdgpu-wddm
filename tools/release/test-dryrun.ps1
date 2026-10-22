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
param([Parameter(Mandatory)][string]$Package, [string]$WorkBase)  # WorkBase: scratch for test-filesafe.ps1
# -Package takes the folder the zip unpacks to or the files\ folder under it: the package root is the one that holds
# manifest.json. The release recipe and an operator may name either.
if ((Test-Path -LiteralPath (Join-Path $Package 'files\manifest.json') -PathType Leaf) -and
    -not (Test-Path -LiteralPath (Join-Path $Package 'manifest.json') -PathType Leaf)) {
    $Package = (Join-Path $Package 'files')
}
# Default scratch: beside the folder the zip unpacks to, never inside it. The package root is files\ of that folder, so one more level up than it used to be; an older flat package keeps the old place.
if (-not $PSBoundParameters.ContainsKey('WorkBase')) {
    $packageTop = $(if ((Split-Path $Package -Leaf) -eq 'files') { Split-Path $Package } else { $Package })
    $WorkBase = Join-Path (Split-Path $packageTop) 'test-tmp'
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
. (Join-Path $PSScriptRoot 'installer\driver-package.ps1')
. (Join-Path $PSScriptRoot 'installer\firmware-package.ps1')   # Test-DriverStoreFirmware (71b3e6bc); dot-sourcing does no I/O
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
# Dry runs use a recorded shape, not a DISM query against this development PC.
[void][IO.Directory]::CreateDirectory($WorkBase)
$inventoryFixture = Join-Path $WorkBase 'driver-store-empty.json'
[IO.File]::WriteAllText($inventoryFixture, '[]')
$env:AMDGPU_WDDM_TEST_DRIVER_STORE = $inventoryFixture
# One RunOnce argument as common.ps1 Format-CommandLine writes it (quoted unless plain path characters).
function Format-RunOnceArg([string]$A) { if ($A -match '[^A-Za-z0-9_.:\\/=,+-]') { return '"' + $A + '"' } else { return $A } }
# BD-163: the folder a single-file setup unpacks itself into is read by name, not by a count. A count over the whole
# of %ProgramData%\amdgpu-wddm moves with every file a run writes there, and once such a folder stays behind it reads
# the same before and after, so the count went blind to the very thing it was meant to catch. The folder is taken out
# of the count and named on its own. A test may point this at a folder of its own.
$script:SetupRoot = Join-Path $env:ProgramData 'amdgpu-wddm\setup'
function Get-Footprint {
    [ordered]@{
        programdata  = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramData 'amdgpu-wddm') -Recurse -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -ne $script:SetupRoot -and -not $_.FullName.StartsWith($script:SetupRoot + '\', [StringComparison]::OrdinalIgnoreCase) }).Count
        setuproot    = Test-Path -LiteralPath $script:SetupRoot
        software     = Test-Path -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm'
        runonce      = [bool](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'amdgpu-wddm-installer' -ErrorAction SilentlyContinue)
        task         = [bool](Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue)
        windowtask   = [bool](Get-ScheduledTask -TaskName 'amdgpu-wddm start window' -ErrorAction SilentlyContinue)   # BD-080: both logon tasks are in the before/after of a dry run
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
# BD-150: the INF names the release and the signed KMD, so two packages with different KMDs never share an INF.
$kmdSys = (Get-FileHash -LiteralPath (Join-Path $Package 'payload\kmd\bc250kmd.sys') -Algorithm SHA256).Hash
$identitySuffix = if (Test-DriverStoreUmdPackage $m) {
    if ([string]$m.driver_signing.submitted_payload_sha256 -notmatch '\A[0-9A-F]{64}\z') { throw 'missing submitted driver payload identity' }
    ', submitted driver payload SHA256 ' + [string]$m.driver_signing.submitted_payload_sha256
} else { '' }
Check ($inf -match ('(?m)^; release ' + [regex]::Escape([string]$m.version) + ', bc250kmd\.sys SHA256 ' + $kmdSys + $identitySuffix + '\r?$')) "INF identity line names release $($m.version) and bc250kmd.sys $($kmdSys.Substring(0, 8))"
. (Join-Path $PSScriptRoot 'installer\version-identity.ps1')
if ($m.version_identity) {
    Check ((Test-PackageVersionIdentity $m $inf $kmdSys).quad -ceq (Get-PackageDriverQuad (Get-ProductPackageQuad $m.version))) 'mapped INF, product and embedded KMD identities agree'
} else {
    Check ([string]$m.version -like "$($m.kmd_version)-*") 'legacy release version carries its INF version'
}
$before = Get-Footprint

'install -DryRun'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun')
$r.text
Check ($r.code -eq 2) "install -DryRun exit $($r.code) (2 = preflight refusal)"
Check ($r.text -match '\[fail\]\s+BC-250 GPU\s+no device PCI\\VEN_1002&DEV_13FE') 'the BC-250 GPU check fails on this computer'
Check ($r.text -match 'package integrity\s+\d+ files match manifest\.json') 'package integrity passes'
Check ($r.text -match 'Nothing was changed') 'refusal says nothing was changed'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'no change, no elevation'

# Smart App Control on refuses before the first change, with the board ignored, so it is the only failed check.
'install -DryRun -DryRunIgnoreBoard with Smart App Control on (AMDGPU_WDDM_TEST_SAC, a dry run only)'
$env:AMDGPU_WDDM_TEST_SAC = 'on'
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_SAC }
$r.text
Check ($r.code -eq 2) "install with Smart App Control on exit $($r.code) (2 = preflight refusal)"
Check ($r.text -match '\[fail\]\s+Smart App Control\s+on \(source: CI\\Policy VerifiedAndReputablePolicyState = 1\): .*Turn Smart App Control off first: open Windows Security > App & browser control > Smart App Control settings, select Off, then "Yes, I''m sure", then Yes in the administrator prompt \(see INSTALL\.md, section Smart App Control and SmartScreen\)') 'the refusal names the steps and the INSTALL.md section'
Check ($r.text -match 'Preflight refused the installation: 1 check\(s\) failed\. Nothing was changed\.') 'Smart App Control is the one failed check, and nothing was changed'
Check ($r.text -notmatch 'would:|doing:') 'no phase runs after the refusal'

'install -DryRun -DryRunIgnoreBoard (walks phases 1 and 2, prints every change)'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard')
$r.text
Check ($r.code -eq 0) "walk-through exit $($r.code)"
Check ($r.text -match 'would: bcdedit /set ''\{current\}'' testsigning on') 'phase 1 shows the test-signing change'
Check ($r.text -match 'would: DiInstallDriverW INSTALL_AS_SET') 'phase 2 shows deferred driver staging'
# Since the shared driver state (6282d18d, 00ce1b02, BD-175) phase 2 writes no Parameters plan of its own: the INF
# carries the installation defaults under Parameters\DefaultsV2, and the resolver applies the release table to the
# migrated state. A dry run cannot read that state, so it names the step; the values are read from the table and the INF.
$pkgInfDefaults = @{}; foreach ($mm in [regex]::Matches($inf, '(?m)^HKR,Parameters\\DefaultsV2,([A-Za-z0-9_]+),0x00010001,(\d+)\s*$')) { $pkgInfDefaults[$mm.Groups[1].Value] = [int64]$mm.Groups[2].Value }
Check (($r.text -match 'would: apply release setting policy through the validated shared-state resolver') -and ([int]$m.defaults.parameters.DpmMode -eq 1) -and ([int]$m.defaults.parameters.DpmMaxMHz -eq 2000) -and ($pkgInfDefaults['DpmMode'] -eq 1) -and ($pkgInfDefaults['DpmMaxMHz'] -eq 2000)) 'phase 2 applies the release table through the shared-state resolver, DPM on, 2000 MHz (table and INF DefaultsV2)'
# UnconfirmedStarts is a runtime record of the driver since 6282d18d (driver/state/catalog.json, kind record), and
# runtime records never get setting defaults (docs/design/driver-state.md): neither the INF, the table nor the plan
# writes it any more.
Check ((-not $pkgInfDefaults.ContainsKey('UnconfirmedStarts')) -and ($inf -notmatch '(?m)^HKR,\s*Parameters[^,]*,\s*UnconfirmedStarts') -and ($null -eq $m.defaults.parameters.UnconfirmedStarts) -and ($r.text -notmatch 'UnconfirmedStarts=')) 'UnconfirmedStarts is a driver record: no installation default and no installer write'
Check ($r.text -match 'previous installer defaults: the defaults of tester\.1 to tester\.7 \(no record\)') 'no AppliedDefaults record on this computer: legacy table'
Check ($r.text -match 'would: .*\\Release: .*AppliedDefaults') 'Release\AppliedDefaults recorded'
Check ($r.text -match "would: scheduled task 'amdgpu-wddm start confirm'") 'phase 2 shows the start-confirm task'
Check ($r.text -match 'would: copy payload\\control') 'phase 2 installs the control application'
Check ($r.text -match 'would: copy licenses\\ and THIRD-PARTY\.md -> .+\\licenses') 'phase 2 installs the licence texts'
Check ($r.text -match 'would: Start menu shortcut .*\\Nagi Control App\.lnk') 'phase 2 shows the Start menu shortcut, with the name Nagi Control App'
Check ($r.text -match 'would: desktop shortcut .*\\Nagi Control App\.lnk') 'phase 2 shows the desktop icon, with the name Nagi Control App'
Check ($r.text -notmatch 'would: (Start menu|desktop) shortcut .*amdgpu-wddm Control') 'no shortcut is written under the old name'
# The Nagi wallpaper: on by default, recorded for Nagi Control App with a stamp of this run.
Check ($r.text -match 'wallpaper: the Nagi wallpaper, set by Nagi Control App once for each person after the install is verified') 'phase 2 says the Nagi wallpaper is chosen'
Check ($r.text -match 'would: HKLM:\\SOFTWARE\\amdgpu-wddm\\Release: Wallpaper = 1, WallpaperStamp = \S+ \d{8}T\d{6}Z') 'phase 2 records the wallpaper choice and its install stamp'
# The picture on the start screen: on by default, and the default writes nothing, because the
# driver reads an absent BootLogo as on. This PC has no bc250kmd service, so the walk-through sees no value.
Check ($r.text -match 'start screen: Nagi on the start screen, by the release default') 'phase 2 says Nagi is drawn on the start screen'
Check ($r.text -notmatch 'BootLogo') 'the default writes no BootLogo value: an absent value is the picture'
# The old names of an older release: this PC has none, and the run says it looked (common.ps1, Remove-LegacyControlShortcuts).
Check ($r.text -match 'shortcuts of an older release \(amdgpu-wddm Control\): none') 'phase 2 looks for the shortcuts of an older release'
$dwm = [int]$m.defaults.desktop_router.DwmForceCpu
# DirectFlipFront is between them from 0.7.213.100-tester.15: the D3D11_1 front of route C is a finished
# feature, so the installer writes it on, and the value is read from the same defaults table as DwmForceCpu.
$front = [int]$m.defaults.desktop_router.DirectFlipFront
$desktopEnd = if (Test-DriverStoreUmdPackage $m) { '(?:\r?\n|$)' } else { '; CpuUmdPath=' }
Check ($r.text -match ("DesktopRouter: DwmForceCpu=$dwm \(new\); RequireKmdSwitches=1 \(new\); " +
                       "DirectFlipFront=$front \(new\)" + $desktopEnd)) `
      "desktop route DwmForceCpu $dwm and DirectFlipFront $front from the defaults table"
# Allow and Deny ship empty from b20 (the Windows component rule is gone); the expected text comes from the table.
$allow = [regex]::Escape('[' + (@($m.defaults.app_router.Allow) -join ', ') + ']')
$deny = [regex]::Escape('[' + (@($m.defaults.app_router.Deny) -join ', ') + ']')
Check ($r.text -match "AppRouter: Mode=gpu-default \(new\); Allow=$allow \(new\); Deny=$deny \(new\)") 'D3D11 gpu-default from the defaults table'
$tbl = Get-Content -LiteralPath (Join-Path $Package 'installer\registry-defaults.json') -Raw | ConvertFrom-Json
Check (($m.defaults | ConvertTo-Json -Depth 6 -Compress) -eq ($tbl.defaults | ConvertTo-Json -Depth 6 -Compress)) 'manifest.json defaults = installer\registry-defaults.json defaults (one table)'
Check (@('EnableGpuPresentBlit', 'EnableCddDwmInterop', 'DpmMode', 'DpmMaxMHz' | Where-Object { $null -eq $m.defaults.parameters.$_ }).Count -eq 0 -and $null -ne $m.defaults.desktop_router.DwmForceCpu) 'manifest defaults carry the five values the control application resets'

Check ($r.text -match 'Dry run complete') 'walk-through completes'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'walk-through: no change, no elevation'
Check ($r.text -match 'shortcuts: Start menu entry, Start menu repair entry, desktop icon') 'the summary names the three shortcuts as a person reads them'
Check ($r.text -notmatch 'desktop desktop') 'no line says "desktop desktop"'
Check ($r.text -match 'Microsoft Visual C\+\+ runtime: present: vcruntime140\.dll') 'this PC has the runtime, and phase 2 says so'

# The two paths of the new steps that this PC would otherwise never walk: a PC without the Microsoft Visual C++
# runtime (AMDGPU_WDDM_TEST_VCRUNTIME, a dry run only), and an install that was asked for no desktop icon. Both
# print what they would do and change nothing.
'install -DryRun with the runtime missing, no desktop icon and Smart App Control in evaluation'
$env:AMDGPU_WDDM_TEST_VCRUNTIME = 'missing'
$env:AMDGPU_WDDM_TEST_SAC = 'evaluation'
try { $r2 = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-NoDesktopShortcut', '-NoWallpaper', '-NoBootLogo') }
finally { Remove-Item Env:\AMDGPU_WDDM_TEST_VCRUNTIME; Remove-Item Env:\AMDGPU_WDDM_TEST_SAC }
$r2.text
Check ($r2.code -eq 0) "walk-through without the runtime exit $($r2.code)"
Check ($r2.text -match '\[warn\]\s+Smart App Control\s+evaluation \(source: CI\\Policy VerifiedAndReputablePolicyState = 2\): if Windows turns Smart App Control on later, Windows blocks the programs of the graphics driver, and the desktop and games lose the driver\. Turn it off now: open Windows Security > App & browser control > Smart App Control settings, select Off, then "Yes, I''m sure", then Yes in the administrator prompt \(see INSTALL\.md, section Smart App Control and SmartScreen\)') 'Smart App Control in evaluation is a warning that says what Windows blocks, with the steps, not a refusal'
Check ($r2.text -match '\[warn\]\s+Visual C\+\+ runtime\s+missing in System32: vcruntime140\.dll, vcruntime140_1\.dll, msvcp140\.dll') 'the preflight states the missing runtime as a line, not a refusal'
Check ($r2.text -match 'the installer installs 14\.44\.35211\.0 from payload\\redist\\vc_redist\.x64\.exe \(SHA256 CC0FF0EB1DC3F518, signature Valid, CN=Microsoft Corporation') 'the line names the file, its hash and its signature'
Check ($r2.text -match 'would: install the Microsoft Visual C\+\+ 2015-2022 Redistributable \(x64\) 14\.44\.35211\.0 from payload\\redist\\vc_redist\.x64\.exe \(/install /quiet /norestart; SHA256 CC0FF0EB1DC3F518 and the Microsoft signature checked\)') 'phase 2 shows the runtime step before anything of ours is copied'
$iVc = $r2.text.IndexOf('would: install the Microsoft Visual C++')
$iCopy = $r2.text.IndexOf('would: copy payload\')
Check ($iVc -ge 0 -and $iCopy -gt $iVc) "the runtime step comes before the first file of ours (runtime at $iVc, first copy at $iCopy)"
Check ($r2.text -match 'shortcuts: Start menu entry, Start menu repair entry; left out: desktop icon') 'the run asked for no desktop icon leaves it out and says so'
Check ($r2.text -notmatch 'would: desktop shortcut') 'no desktop icon is written in that run'
Check (($r2.text -match 'wallpaper: not set \(-NoWallpaper\)') -and ($r2.text -match 'Release: Wallpaper = 0, WallpaperStamp = ')) 'the run asked for no wallpaper records 0 and says so'
Check ($r2.text -match 'start screen: no picture on the start screen \(-NoBootLogo\)') 'the run asked for no start-screen picture says so'
Check ($r2.text -match 'would: HKLM:\\SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters: BootLogo = 0') 'the run asked for no start-screen picture writes BootLogo = 0'
Check ($r2.text -match 'Dry run complete' -and $r2.text -notmatch 'doing:') 'that walk-through changed nothing either'

'registry defaults on upgrade (test-registry-defaults.ps1 under 5.1, HKCU scratch key)'
$rr = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-registry-defaults.ps1'), '-Installer', (Join-Path $Package 'installer'))
$rr.text
Check ($rr.code -eq 0) "set / same / update / kept / command line / installer-owned: exit $($rr.code)"

# BD-122: the readers of the KMD's start record, against the packaged installer (test-start-record.ps1 under 5.1,
# HKCU scratch key). The driver's own half is the host test driver\kmd\test\run_start_record.ps1.
$sr = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-start-record.ps1'), '-Installer', (Join-Path $Package 'installer'))
$sr.text
Check ($sr.code -eq 0) "start record: step failure / guard refusal / started after a failure / absent: exit $($sr.code)"

# The installer collects the logs itself when the install does not pass. The decision and the
# readers of that diagnostics file (test-diagnostics.ps1 under 5.1, an HKCU scratch key and a folder of this run),
# with the field report 20261011T222253Z replayed as its first fixture.
$dg = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-diagnostics.ps1'), '-Installer', (Join-Path $Package 'installer'), '-WorkRoot', (Join-Path $WorkBase 'diagnostics'))
$dg.text
Check ($dg.code -eq 0) "diagnostics: the field case / a started driver / every hard signal / the readers / the bounds: exit $($dg.code)"

# D3D9 goes through D3D9On12: the D3D9 slot holds '<>' in both views (an empty first entry stops the adapter
# on newer display kernels), and no stub is copied.
Check ($r.text -notmatch 'would: copy payload\\(system32|syswow64)|bc250umd\.dll -> ') 'no D3D9 stub is copied (System32 or SysWOW64)'
$driverStoreUmd = [bool]$m.driver_package
if ($driverStoreUmd) {
    Check ($r.text -match 'INF .+ UserModeDriverName \[MultiString\] = <> \| %13%\\desktop\\bc250d3d_router\.dll \| %13%\\desktop\\bc250d3d_router\.dll \| %13%\\d3d12\\amdgpu_wddm_d3d12\.dll') 'INF owns x64 D3D slots'
    Check ($r.text -match 'INF .+ UserModeDriverNameWow \[MultiString\] = <> \| %13%\\wow64\\desktop\\bc250d3d_router32\.dll \| %13%\\wow64\\desktop\\bc250d3d_router32\.dll \| %13%\\wow64\\d3d12\\amdgpu_wddm_d3d1232\.dll') 'INF owns x86 D3D slots'
    Check ($r.text -match 'INF .+ VulkanDriverNameWow \[MultiString\] = %13%\\wow64\\vulkan\\radeon_icd32\.json') 'INF owns x86 Vulkan discovery'
    Check ($r.text -notmatch 'would: copy payload\\(desktop|wow64|d3d11|d3d12|vulkan|opengl|mft) ->') 'no adapter library is copied to Program Files'
    Check ($r.text -notmatch 'would: .*Khronos\\Vulkan\\Drivers.* = 0') 'no global Vulkan duplicate is created'
    Check ($r.text -match 'remove legacy router paths and duplicate global Vulkan entries') 'upgrade removes stale overrides after binding'
} else {
Check ($r.text -match "would: .+ UserModeDriverName = <> \| [^|]+\\desktop\\bc250d3d_router\.dll \| [^|]+\\desktop\\bc250d3d_router\.dll \| [^|]+\\d3d12\\amdgpu_wddm_d3d12\.dll; VulkanDriverName = ") "UserModeDriverName: D3D9 slot <> (D3D9On12), the router in the D3D10 and D3D11 slots, the D3D12 shell"
Check ($r.text -notmatch 'UserModeDriverName(Wow)? = bc250umd') 'no slot names the D3D9 stub'
# BD-064: 32-bit processes get the x86 builds: D3D9 (<>)/10/11/12 slots, the x86 Vulkan ICD, the router's Wow paths.
Check ($r.text -match 'would: copy payload\\wow64 -> .+\\wow64 ') 'phase 2 installs the x86 builds (wow64)'
Check ($r.text -match "would: .+ UserModeDriverNameWow = <> \| [^|]+\\wow64\\desktop\\bc250d3d_router\.dll \| [^|]+\\wow64\\desktop\\bc250d3d_router\.dll \| [^|]+\\wow64\\d3d12\\amdgpu_wddm_d3d12\.dll; VulkanDriverNameWow = [^;\r\n]+\\wow64\\vulkan\\radeon_icd\.json") 'UserModeDriverNameWow: D3D9 slot <> (D3D9On12), two x86 routers, the x86 D3D12 shell; VulkanDriverNameWow'
Check ($r.text -match "would: HKLM:\\SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers '[^']+\\wow64\\vulkan\\radeon_icd\.json' = 0") 'the x86 ICD in the WOW6432Node Khronos list'
Check (($r.text -match 'DesktopRouter: .*CpuUmdPathWow=[^;\r\n]+\\wow64\\desktop\\bc250d3d\.dll') -and ($r.text -match 'AppRouter: .*GpuUmdPathWow=[^;\r\n]+\\wow64\\d3d11\\amdgpu_wddm_d3d11\.dll')) 'the x86 router paths are installer-owned'
}
# The H.264 encoder MFT (M15.11, driver/umd/mft-h264/INSTALL.md): manifest.json "mft_h264" says whether this release
# registers the transform, and the dry run shows every key it would write, or that it writes none.
$mftPath = if ($driverStoreUmd) { 'payload/kmd/mft/amdgpu_wddm_mft_h264.dll' } else { 'payload/mft/amdgpu_wddm_mft_h264.dll' }
$mftOn = [bool]($m.mft_h264 -and $m.mft_h264.register)
Check ((-not $m.mft_h264) -or (($m.mft_h264.clsid -eq '{A32438F0-0D79-4CA9-A5BF-9F3C80837253}') -and ($m.mft_h264.package_path -eq $mftPath) -and ($m.mft_h264.friendly_name -eq 'BC-250 H.264 Encoder MFT'))) "manifest mft_h264: register $mftOn, class id and name as INSTALL.md gives them"
if ($mftOn) {
    Check (@($m.files | Where-Object { $_.path -eq $mftPath }).Count -eq 1) "the package carries $mftPath"
    $mftInstalled = if ($driverStoreUmd) { 'DriverStore (bc250kmd.inf)\mft\amdgpu_wddm_mft_h264.dll' } else { '<InstallDir>\mft\amdgpu_wddm_mft_h264.dll' }
    Check (@($m.components | Where-Object { $_.package_path -eq $mftPath -and $_.install_path -eq $mftInstalled }).Count -eq 1) 'manifest components: encoder location matches the delivery contract'
    if (-not $driverStoreUmd) { Check ($r.text -match 'would: copy payload\\mft -> [^\r\n]+\\mft \(same SHA256') 'phase 2 installs the encoder DLL' }
    Check ($r.text -match ('registration values from ' + [regex]::Escape($mftPath))) 'the registration values come out of the shipped DLL'
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
# The OpenGL ICD (BD-076, GitHub issue #8): manifest.json "opengl_icd" says whether this release registers an OpenGL
# driver and for which architectures ("architectures"), and the dry run shows the files and the values it would write.
# This release lists x64 alone, so the plan has to name the three x64 values and none of the three Wow ones: a 32-bit
# OpenGL program keeps the renderer of Windows itself (common.ps1 holds the lab measurement behind that).
$glOn = [bool]($m.opengl_icd -and $m.opengl_icd.register)
$glArch = @()
if ($m.opengl_icd -and $m.opengl_icd.architectures) { $glArch = @($m.opengl_icd.architectures | ForEach-Object { [string]$_ }) }
$glX86 = [bool]($glArch -contains 'x86')
if ($driverStoreUmd) {
    if ($glOn) {
        Check ($m.opengl_icd.package_path -eq 'payload/kmd/opengl/amdgpu_wddm_gl.dll') 'OpenGL uses the DriverStore package path'
        Check ($r.text -match 'INF .+ OpenGLDriverName \[MultiString\] = %13%\\opengl\\amdgpu_wddm_gl\.dll') 'OpenGL registration comes from the INF'
        Check ($r.text -match 'INF .+ OpenGLFlags \[DWord\] = 1') 'OpenGL flags retained'
        Check ($r.text -match 'INF .+ OpenGLVersion \[DWord\] = 1') 'OpenGL version retained'
        if (-not $glX86) { Check ($r.text -notmatch 'INF .+ OpenGLDriverNameWow') 'no undeclared x86 OpenGL registration' }
    }
} else {
Check ((-not $m.opengl_icd) -or (($m.opengl_icd.package_path -eq 'payload/opengl/amdgpu_wddm_gl.dll') -and ($m.opengl_icd.package_path_wow -eq 'payload/wow64/opengl/amdgpu_wddm_gl.dll') -and (@('REG_MULTI_SZ', 'REG_SZ') -contains [string]$m.opengl_icd.value_type) -and ([int]$m.opengl_icd.version -ge 1))) "manifest opengl_icd: register $glOn, both package paths, $([string]$m.opengl_icd.value_type), version $([string]$m.opengl_icd.version), flags $([string]$m.opengl_icd.flags)"
Check ((-not $glOn) -or (@($glArch).Count -and (@($glArch | Where-Object { @('x64', 'x86') -notcontains $_ }).Count -eq 0))) "manifest opengl_icd: the architecture list is named and known ($($glArch -join ', '))"
if ($glOn) {
    foreach ($p in @(@(if ($glArch -contains 'x64') { [string]$m.opengl_icd.package_path }) + @(if ($glX86) { [string]$m.opengl_icd.package_path_wow }))) {
        Check (@($m.files | Where-Object { $_.path -eq $p }).Count -eq 1) "the package carries $p"
    }
    if ($glArch -contains 'x64') {
        Check (@($m.components | Where-Object { $_.package_path -eq [string]$m.opengl_icd.package_path -and $_.install_path -eq '<InstallDir>\opengl\amdgpu_wddm_gl.dll' }).Count -eq 1) 'manifest components: the x64 ICD goes to <InstallDir>\opengl'
        Check ($r.text -match 'would: copy payload\\opengl -> [^\r\n]+\\opengl ') 'phase 2 installs the x64 OpenGL ICD'
    }
    if ($glX86) {
        Check ($r.text -match "would: .+ OpenGLDriverName = \[[^\r\n]+\\opengl\\amdgpu_wddm_gl\.dll\]; OpenGLVersion = 1; OpenGLFlags = 1; OpenGLDriverNameWow = \[[^\r\n]+\\wow64\\opengl\\amdgpu_wddm_gl\.dll\]; OpenGLVersionWow = 1; OpenGLFlagsWow = 1 \(REG_MULTI_SZ, x64 and x86\)") 'the plan names the six values of the OpenGL registration, x64 and Wow'
    } else {
        # The positive control and the negative one in one line each: the three x64 values are named with the
        # architecture list, and no Wow value is named anywhere in the plan.
        Check ($r.text -match "would: .+ OpenGLDriverName = \[[^\r\n]+\\opengl\\amdgpu_wddm_gl\.dll\]; OpenGLVersion = 1; OpenGLFlags = 1 \(REG_MULTI_SZ, x64\)") 'the plan names the three x64 values of the OpenGL registration and the architecture list'
        Check ($r.text -notmatch 'OpenGLDriverNameWow = |OpenGLVersionWow = |OpenGLFlagsWow = ') 'no 32-bit OpenGL value is written by a release that registers x64 alone'
        Check (@($m.files | Where-Object { $_.path -eq [string]$m.opengl_icd.package_path_wow }).Count -eq 0) 'the package carries no 32-bit OpenGL ICD'
        Check (@($m.components | Where-Object { [string]$_.install_path -like '*wow64\opengl\*' }).Count -eq 0) 'no component installs a 32-bit OpenGL ICD'
    }
    Check ($r.text -notmatch 'would: remove [^\r\n]+\\opengl \(the OpenGL ICD of an earlier install') 'nothing of the ICD is rolled back by a release that registers it'
} else {
    Check ($r.text -match 'OpenGL ICD: not registered by this release') 'the dry run says this release registers no OpenGL ICD'
    Check ($r.text -notmatch 'would: copy payload\\opengl|OpenGLDriverName = ') 'nothing of the OpenGL ICD is installed'
}
}
'OpenGL ICD registration (test-opengl-icd.ps1 under 5.1, HKCU scratch key)'
$rg = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-opengl-icd.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Package', $Package)
$rg.text
Check ($rg.code -eq 0) "the release switch, the six values, the ICD file check, the restore plan: exit $($rg.code)"
'32-bit registration (test-wow64.ps1 under 5.1, HKCU scratch key)'
$rw = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-wow64.ps1'), '-Package', $Package, '-WorkRoot', $WorkBase)
$rw.text
Check ($rw.code -eq 0) "x86 images, install paths, exports, verify's registration check: exit $($rw.code)"
if (Test-DriverStoreFirmware $m.firmware) {
    # From 71b3e6bc the Driver Store package carries the firmware, and the installer writes no firmware folder at all.
    Check (($r.text -match 'would: keep the pinned firmware with this repair set; Driver Store owns the installed copies') -and ($r.text -notmatch 'would: [^\r\n]*C:\\BC250')) 'firmware step: the Driver Store owns the installed copies, and no step names C:\BC250'
} else {
    Check ($r.text -match 'C:\\BC250 itself is not changed') 'firmware step leaves C:\BC250 itself alone'
}
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
# Since the shared driver state (6282d18d, 00ce1b02, BD-175) the settings are captured through the state worker before
# staging and judged from that capture after the binding; the Parameters snapshot of parameters_before_install is
# never replayed, because it would put back a value that a later control-app choice deleted.
$pos = @('Save-DriverSettingsComparison -State $state', "Invoke-Change 'DiInstallDriverW INSTALL_AS_SET", '$before=Get-DriverSettingsComparison -State $state', 'Invoke-DriverStateDefaults -Cli') | ForEach-Object { $src.IndexOf($_) }
Check ((@($pos | Where-Object { $_ -lt 0 }).Count -eq 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($pos[2] -lt $pos[3]) -and ($src -match '-Before \$before') -and ($src -notmatch '\$state\.parameters_before_install')) 'driver settings are captured before staging and judged from that capture, never from parameters_before_install'
$iFreeze = $r.text.IndexOf('would: freeze state writers and publish the protected state handoff before INF staging')
$iStage = $r.text.IndexOf('would: DiInstallDriverW INSTALL_AS_SET')
$iResolve = $r.text.IndexOf('would: apply release setting policy through the validated shared-state resolver')
Check (($iFreeze -ge 0) -and ($iFreeze -lt $iStage) -and ($iStage -lt $iResolve)) "the walk-through captures the state before staging and applies the table after it ($iFreeze, $iStage, $iResolve)"
# The capture keeps the whole state, and the INF resets no value: a snapshot of the judged names alone once dropped
# CuMode at every release install, and the GPU then ran on 24 of its 40 compute units.
$lifecycle = [IO.File]::ReadAllText((Join-Path $Package 'installer\state-lifecycle.ps1'))
Check (($lifecycle -match "Set-StateValue \`$State 'driver_settings_compare' \(\[pscustomobject\]@\{schema=1;inf_sha256=\`$InfSha256;device=\`$Device;items=@\(\`$snapshot\.items\)\}\)") -and ([regex]::Matches($inf, '(?m)^\s*HKR\s*,\s*"?Parameters"?\s*,').Count -eq 0)) 'the capture keeps every item of the state, and the INF writes no value straight into Parameters'
# The encoder registration is reported where a support report can read it (BugReport.cs collects every verify report).
Check (($src -match "Add-Result 'H\.264 encoder'") -and ($src -match "Add-Warning 'H\.264 encoder'") -and ($src -match 'Test-MftRegistration -ClassesKey \$script:ClassesKey -DllPath \$mftInstalled')) 'verify reports the H.264 encoder registration, and a registration that is not right is a warning'
Check (($src -match "Add-Result 'full WDDM gate'") -and ($src -match 'EnableFullWddm -eq 1\) -or \(\$p\.EnableFullWddm -eq 2\)')) 'verify fails a display-only start (EnableFullWddm not 1 or 2)'
$common = [IO.File]::ReadAllText((Join-Path $Package 'installer\common.ps1'))
Check (($src -match '(?m)^\s+Set-StateDirAccess\s*$') -and ($common -match "\*S-1-5-32-545:\(OI\)\(CI\)RX") -and ($common -match "'/reset', '/T'") -and ($common -match 'function Set-StateDirAccess \{\s+if \(\$script:DryRunMode\) \{ return \}')) 'the state folder (logs, verify results) gets administrators/SYSTEM full and users read, children reset; not in a dry run'
Check (([string]$m.kmd_abi -eq '0x000700D8' -and [version]($m.kmd_build) -ge [version]'0.7.216.0') -or ([string]$m.kmd_abi -eq '0x000700D7' -and [version]($m.kmd_build) -ge [version]'0.7.215.0' -and [version]($m.kmd_build) -lt [version]'0.7.216.0') -or ([string]$m.kmd_abi -eq '0x000700D6' -and [version]($m.kmd_build) -ge [version]'0.7.214.0' -and [version]($m.kmd_build) -lt [version]'0.7.215.0') -or ([string]$m.kmd_abi -eq '0x000700D5' -and [version]($m.kmd_build) -ge [version]'0.7.213.0' -and [version]($m.kmd_build) -lt [version]'0.7.214.0') -or ([string]$m.kmd_abi -eq '0x000700D4' -and [version]($m.kmd_build) -ge [version]'0.7.212.0' -and [version]($m.kmd_build) -lt [version]'0.7.213.0') -or ([string]$m.kmd_abi -eq '0x000700D0' -and [version]($m.kmd_build) -ge [version]'0.7.208.0' -and [version]($m.kmd_build) -lt [version]'0.7.209.0') -or ([string]$m.kmd_abi -eq '0x000700CF' -and [version]($m.kmd_build) -ge [version]'0.7.207.0' -and [version]($m.kmd_build) -lt [version]'0.7.208.0') -or ([string]$m.kmd_abi -eq '0x000700CE' -and [version]($m.kmd_build) -ge [version]'0.7.206.0' -and [version]($m.kmd_build) -lt [version]'0.7.207.0') -or ([string]$m.kmd_abi -eq '0x000700CD' -and [version]($m.kmd_build) -ge [version]'0.7.205.0' -and [version]($m.kmd_build) -lt [version]'0.7.206.0') -or ([string]$m.kmd_abi -eq '0x000700CC' -and [version]($m.kmd_build) -ge [version]'0.7.204.0' -and [version]($m.kmd_build) -lt [version]'0.7.205.0') -or ([string]$m.kmd_abi -eq '0x000700CB' -and [version]($m.kmd_build) -ge [version]'0.7.203.0' -and [version]($m.kmd_build) -lt [version]'0.7.204.0') -or ([string]$m.kmd_abi -eq '0x000700CA' -and [version]($m.kmd_build) -ge [version]'0.7.202.0' -and [version]($m.kmd_build) -lt [version]'0.7.203.0') -or ([string]$m.kmd_abi -eq '0x000700C9' -and [version]($m.kmd_build) -ge [version]'0.7.201.0' -and [version]($m.kmd_build) -lt [version]'0.7.202.0') -or ([string]$m.kmd_abi -eq '0x000700C8' -and [version]($m.kmd_build) -ge [version]'0.7.200.0' -and [version]($m.kmd_build) -lt [version]'0.7.201.0') -or ([string]$m.kmd_abi -eq '0x000700C7' -and [version]($m.kmd_build) -lt [version]'0.7.200.0')) "kmd_abi $($m.kmd_abi) for KMD build $($m.kmd_build)"

# BD-060: nothing in the installer stops or restarts DWM or the display device under the running desktop; the driver
# package changes the GPU at shutdown (INSTALL_AS_SET), and only uninstall moves it in place (documented
# there). The GPU's HD Audio function is a different device on the same part, and the installer restarts that one
# function on purpose (BD-092: DisplayPort audio plays at the right rate only with a message-signalled interrupt,
# which the function reads when it starts). The four checks below hold that restart to that one function, so a
# restart of the display device, of a service or of DWM still fails here.
$scripts = @(Get-ChildItem -LiteralPath (Join-Path $Package 'installer') -Filter *.ps1 | ForEach-Object { [pscustomobject]@{ name = $_.Name; text = [IO.File]::ReadAllText($_.FullName) } })
$byName = @{}; foreach ($s in $scripts) { $byName[$s.name] = $s.text }
$bad = @(foreach ($s in $scripts) { foreach ($p in 'Stop-Process', 'taskkill', 'Restart-Service', 'Stop-Service', '/disable-device', '/enable-device', '/remove-device', 'Disable-PnpDevice', 'Enable-PnpDevice', 'Restart-PnpDevice') { if ($s.text -match [regex]::Escape($p)) { "$($s.name): $p" } } })
# The uninstall is the one place that moves the GPU in place (above). Since the shared driver state (6282d18d) its
# state worker archives the final state with the device stopped and disabled, and uninstall.ps1 enables that device
# again for its inbox driver: one /enable-device, on the archived device, and nowhere else.
$enableOk = ([regex]::Matches($byName['uninstall.ps1'], '/enable-device').Count -eq 1) -and
    ($byName['uninstall.ps1'] -match "(?s)Invoke-Change 'enable the archived, removed device for its inbox display driver' \{\s+\`$enabled=Invoke-Native pnputil\.exe @\('/enable-device',\`$removalDevice\)")
$bad = @($bad | Where-Object { -not ($enableOk -and $_ -eq 'uninstall.ps1: /enable-device') })
Check ($bad.Count -eq 0) "no installer script stops or restarts DWM, a service or the display device$(if ($bad.Count) { ': ' + ($bad -join '; ') })"
Check $enableOk 'uninstall.ps1 enables the archived, removed device once, for its inbox display driver'
# The argument lists, not the whole text: the scripts name /restart-device in comments and in the words they print,
# and those are not a restart. An argument list reaches a binary either as `Invoke-Native <exe> @(...)` or as
# `-Arguments @(...)`, and the match below reads /restart-device out of one whatever its quoting is ('x', "x" or
# bare x), so a second restart cannot hide behind a different quote. The call site is counted as well as the
# argument text, because an argument can also come from a variable: $script:PnpUtilPath is bounded-invoked once.
$argLists = @(foreach ($s in $scripts) {
    foreach ($mm in [regex]::Matches($s.text, '(?:Invoke-Native(?:Bounded)?\s+(?:-File\s+)?\S+\s+(?:-Arguments\s+)?|-Arguments\s+)@\(([^)]*)\)')) {
        [pscustomobject]@{ name = $s.name; args = $mm.Groups[1].Value }
    }
})
$restartIn = @($argLists | Where-Object { $_.args -match '(?<![\w/-])[''"]?/restart-device[''"]?(?![\w-])' } | ForEach-Object { $_.name })
Check (($restartIn -join ', ') -eq 'common.ps1') "one pnputil /restart-device argument in the package, in common.ps1: $(if ($restartIn.Count) { $restartIn -join ', ' } else { 'none' })"
$pnpUtilCalls = @(foreach ($s in $scripts) { foreach ($mm in [regex]::Matches($s.text, 'Invoke-Native(?:Bounded)?\s+-File\s+\$script:PnpUtilPath')) { $s.name } })
Check (($pnpUtilCalls -join ', ') -eq 'common.ps1') "one bounded pnputil call site in the package, in common.ps1: $(if ($pnpUtilCalls.Count) { $pnpUtilCalls -join ', ' } else { 'none' })"
Check ($byName['common.ps1'] -match '(?s)function Restart-GpuAudioDevice \{.{0,400}?Invoke-NativeBounded -File \$script:PnpUtilPath -Arguments @\(''/restart-device'', \$InstanceId\)') 'that one call is Restart-GpuAudioDevice, and it restarts the device whose instance id it is given'
$audioRestarts = @(foreach ($n in 'install.ps1', 'uninstall.ps1') { foreach ($mm in [regex]::Matches($byName[$n], 'Restart-GpuAudioDevice[^\r\n]*')) { "$($n): $($mm.Value.Trim())" } })
Check (($audioRestarts -join '; ') -eq 'install.ps1: Restart-GpuAudioDevice -InstanceId $audio.instance; uninstall.ps1: Restart-GpuAudioDevice -InstanceId $audio.instance') "install and uninstall restart the HD Audio function of the audio plan and nothing else: $(if ($audioRestarts.Count) { $audioRestarts -join '; ' } else { 'no call' })"
# The encoder DLL exports no DllRegisterServer on purpose (driver/umd/mft-h264/INSTALL.md, step 1): a driver package
# writes its COM registration itself.
$bad = @(foreach ($s in $scripts) { if ($s.text -match 'regsvr32') { $s.name } })
Check ($bad.Count -eq 0) "no installer script calls regsvr32$(if ($bad.Count) { ': ' + ($bad -join ', ') })"
$pnpCalls = @(foreach ($s in $scripts) { foreach ($mm in [regex]::Matches($s.text, 'Invoke-Native pnputil\.exe @\(\s*[''"]?(/[a-z-]+)[''"]?')) { "$($s.name) $($mm.Groups[1].Value)" } })
Check ((($pnpCalls | Sort-Object) -join ', ') -eq 'install.ps1 /delete-driver, uninstall.ps1 /delete-driver, uninstall.ps1 /enable-device, uninstall.ps1 /scan-devices') "pnputil calls:$($pnpCalls -join ', ')"
# BD-089: the install deletes an older package of ours from the driver store, and it must not touch the device while
# it does it. The uninstall takes the GPU off our driver on purpose, so there both switches belong.
Check (($src -match "Invoke-Native pnputil\.exe @\('/delete-driver', \`$oldPackage\)") -and ($src -notmatch "'/delete-driver'[^\r\n]*/uninstall")) 'install.ps1 deletes an older driver package without /uninstall and without /force'
$unsrc = [IO.File]::ReadAllText((Join-Path $Package 'installer\uninstall.ps1'))
Check ($unsrc -match "@\('/delete-driver', \`$p, '/uninstall', '/force'\)") 'uninstall.ps1 removes the package of the GPU with /uninstall /force (the GPU goes back to Microsoft Basic Display Adapter)'
$pos = @('Test-InfHasReboot ([IO.File]::ReadAllLines($infFile))', "Invoke-Change 'DiInstallDriverW INSTALL_AS_SET") | ForEach-Object { $src.IndexOf($_) }
Check (($pos[0] -gt 0) -and ($pos[0] -lt $pos[1])) 'install.ps1 rejects Reboot before staging the display package'
Check ($r.text -match 'would: DiInstallDriverW INSTALL_AS_SET payload\\kmd \(the GPU changes to it at the next restart\)') 'the walk-through shows the driver set for the next restart'
Check (($src -match "'observed' \{ Add-Warning 'DWM restarted in this session'") -and ($src -match 'Get-DwmReplacementFinding \(Find-DwmBaseline \(Read-DwmBaseline\) \$epoch\)') -and ($src -notmatch 'LogonUtc -DwmStartUtc|StartTime|CreationDate -gt')) 'verify reports a DWM replacement only when observed against the logon baseline, unknown history otherwise'
$sc0 = [IO.File]::ReadAllText((Join-Path $Package 'installer\start-confirm.ps1'))
Check (($sc0 -match 'if \(-not \$Probe\) \{\s+try \{\s+\. \(Join-Path \$here ''dwm-session\.ps1''\)') -and ($sc0 -match 'Save-DwmBaseline \$epoch')) 'the start-confirm task records the DWM baseline at each logon (not in -Probe)'
Check ((Test-Path -LiteralPath (Join-Path $Package 'payload\tools\dwm-session.ps1')) -and ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\dwm-session.ps1')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'installer\dwm-session.ps1')).Hash)) 'dwm-session.ps1 ships next to the installed start-confirm task'
Check ($r.text -match 'DWM of session \d+ before the driver package: ') 'the walk-through records the DWM before the driver package'
# BD-069: the installer tells the driver's own safety closure from a setting of the tester, and only a repair reopens
# one. The records belong to the KMD (driver/kmd/interop.c, driver/kmd/dpm.c); the installer reads them.
Check (($common -match '\$script:DriverClosures = @\{') -and ($common -match "record = 'InteropClosedReason'") -and ($common -match "record = 'DpmClosedReason'") -and ($common -match "legacy_record = 'DpmLastReason'") -and ($common -match "record = 'CuModeClosedReason'") -and ($common -match "'reopened'") -and ($common -match "'driver-closed'")) 'common.ps1 holds the driver closure table (InteropClosedReason, DpmClosedReason with DpmLastReason as the legacy record, CuModeClosedReason) and both decisions'
# BD-077: the graphics cores are a closure of the same kind, and they carry no legacy record. CuModeLastReason is
# written by every start, a tester's own 24 start among them, so it would turn a chosen 24 into a closure.
Check (($common -match "CuMode = @\{ closed = 24; record = 'CuModeClosedReason'; any_reason = \`$true") -and ($common -notmatch "legacy_record = 'CuModeLastReason'")) 'the graphics-core closure is CuMode 24 with CuModeClosedReason, and no legacy record'
Check (($src -match "(?m)^\`$script:ReopenClosures = \[bool\]\`$Repair\s*$") -and ([regex]::Matches($src, '\$script:ReopenClosures =').Count -eq 1) -and ([regex]::Matches($src, '-Reopen:\$script:ReopenClosures').Count -eq 2) -and ($common -match "InstallSwitches = @\('NoControlApp', 'NoReboot', 'Force', 'Repair', 'ApplySystemTuning', 'NoStartMenuShortcut', 'NoDesktopShortcut', 'NoWallpaper', 'NoBootLogo'\)")) 'only the -Repair switch reopens a closure (the plan and the write both), and -Repair survives the restart'
Check (($common -match "8 = 'smu-error'") -and ($src -match "\`$ioRemedy = '; remedy: run install\.cmd -Repair") -and ($src -notmatch "if \(\`$ioClosed\) \{ \`$ioClosed \+= '; remedy")) 'every reason the DPM guard persists is a closure (3, 4, 8), and verify names the repair only where the driver left a record'
Check (($common -match "legacy_record = 'DpmLastReason'; any_reason = \`$true") -and ($common -match "\(\`$record -eq \`$c\.record\) -and \`$c\.any_reason")) 'a durable record takes any reason the driver writes; only the legacy record is held to the three names'
$marker = @(foreach ($s in $scripts) { if ((@($s.text -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n") -match 'InteropSession|InteropBoot') { $s.name } })
Check ($marker.Count -eq 0) "the BD-059 session marker stays under KMD ownership: no installer code names it$(if ($marker.Count) { ': ' + ($marker -join ', ') })"
$pos = @("Invoke-Change 'DiInstallDriverW INSTALL_AS_SET", "Add-DwmObservation 'after the driver package'", "if (`$driverSetAction -ne 'verify-binding')", "Add-DwmObservation 'end of phase 2'", "Save-Phase 'installed'") | ForEach-Object { $src.IndexOf($_) }
Check (($pos[0] -gt 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($pos[2] -lt $pos[3]) -and ($pos[3] -lt $pos[4]) -and ($src -match "Set-StateValue \`$state 'dwm_observations'")) 'DWM observations before and right after the driver package (ahead of the restart-pending branch) and at the end of phase 2 go into the state'
$pos = @('$installInputs = Get-InstallInputs $PSBoundParameters $early', '$was = Set-InstallPackage $state $packageVersion', 'Save-InstallInputs $state $installInputs', "if (`$state.phase -eq 'new')", "Save-Phase 'driver-pending-restart'", 'Clear-InstallInputs $state', "Save-Phase 'installed'") | ForEach-Object { $src.IndexOf($_) }
Check ((@($pos | Where-Object { $_ -lt 0 }).Count -eq 0) -and ($pos[0] -lt $pos[1]) -and ($pos[1] -lt $pos[2]) -and ($pos[2] -lt $pos[3]) -and ($pos[3] -lt $pos[4]) -and ($pos[4] -lt $pos[5]) -and ($pos[5] -lt $pos[6]) -and ([regex]::Matches($src, 'Clear-InstallInputs').Count -eq 1)) 'install inputs: restored first, bound to the package that takes an unfinished install over, saved before any restart for every install action, cleared only when phase 2 completes'
'BD-060 session rules (test-session-checks.ps1 under 5.1, HKCU scratch key)'
$rs = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-session-checks.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Inf', (Join-Path $Package 'payload\kmd\bc250kmd.inf'), '-WorkRoot', $WorkBase)
$rs.text
Check ($rs.code -eq 0) "driver-set staging, legacy outcomes, install inputs across a restart, DWM baseline: exit $($rs.code)"

'GPU desktop verification selects the desktop from SYSTEM and interactive callers'
$rs = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-verify-desktop-session.ps1'), '-Installer', (Join-Path $Package 'installer'))
$rs.text
Check ($rs.code -eq 0) "GPU desktop session selection and absent-desktop refusal: exit $($rs.code)"

'unattended console (test-console.ps1 under 5.1, synthetic engine only)'
$consoleWork = Join-Path $WorkBase ('console-' + [Guid]::NewGuid().ToString('N'))
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-console.ps1'), '-Installer', (Join-Path $Package 'installer'), '-WorkRoot', $consoleWork)
$r.text
Check ($r.code -eq 0) "console EOF, defaults, arguments, exit codes and no final pause: exit $($r.code)"

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
        Check (($r.text -match 'would: copy payload\\tools') -and ($r.text -match 'would: DiInstallDriverW INSTALL_AS_SET') -and ($r.text -match 'Dry run complete')) "$($c.name): phase 2 runs again"
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

'an install over an older release: the files of that release that this package does not install (BD-089)'
# The install root of this case is a folder under the work base (the state file names it, as a real installation does).
# Its manifest.json is the one an older release left there: this package's manifest with two more files. One of them
# has on disk the bytes that manifest records, the other one was changed after that install and must stay.
$oldRoot = Join-Path $WorkBase ('install-root-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$orphan = Join-Path $oldRoot 'desktop\bc250d3d_old.dll'
$orphanChanged = Join-Path $oldRoot 'tools\old_tool.exe'
foreach ($p in @($orphan, $orphanChanged)) { [void][IO.Directory]::CreateDirectory((Split-Path $p)) }
[IO.File]::WriteAllText($orphan, 'the desktop driver of the older release')
[IO.File]::WriteAllText($orphanChanged, 'a tester put their own file here')
$someoneElse = Join-Path $oldRoot 'desktop\bc250d3d.dll.b20orig'
[IO.File]::WriteAllText($someoneElse, 'a file of nobody''s release, next to ours')
$recorded = Join-Path $WorkBase 'bd089-recorded.bin'
[IO.File]::WriteAllText($recorded, 'what the older release installed there')
$recordedSha = (Get-FileHash -LiteralPath $recorded -Algorithm SHA256).Hash
Remove-Item -LiteralPath $recorded -Force
$oldVersion = '0.7.197.100-tester.1'
$oldManifest = $m | ConvertTo-Json -Depth 10 | ConvertFrom-Json
$oldManifest.version = $oldVersion
$oldManifest.files = @(@($oldManifest.files) + @(
        [pscustomobject]@{ path = 'payload/desktop/bc250d3d_old.dll'; sha256 = (Get-FileHash -LiteralPath $orphan -Algorithm SHA256).Hash; size = 1 }
        [pscustomobject]@{ path = 'payload/tools/old_tool.exe'; sha256 = $recordedSha; size = 1 }))
[IO.File]::WriteAllText((Join-Path $oldRoot 'manifest.json'), ($oldManifest | ConvertTo-Json -Depth 10))
$dir = Join-Path $WorkBase ('state-bd089-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($dir)
$st = [ordered]@{ schema = 1; phase = 'verified'; package_version = $oldVersion; install_root = $oldRoot; testsigning_set_by_installer = $true; updated_utc = '2026-10-03T00:00:00Z' }
[IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
$env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
Check ($r.code -eq 0) "install over $oldVersion`: exit $($r.code)"
Check ($r.text -match ("files of the installed release " + [regex]::Escape($oldVersion) + " that " + [regex]::Escape($pkgVersion) + " does not install: 2 \(1 to remove, 1 changed after that install and kept, 0 already gone\)")) 'the plan names the count before any change'
Check ($r.text -match ('to remove in the files stage: ' + [regex]::Escape($orphan))) 'the plan names the file it removes, before the first step'
if ($driverStoreUmd) {
    Check ($r.text -match ('would: remove legacy UMD ' + [regex]::Escape($orphan) + ' after the new INF binding')) 'legacy UMD removal follows new INF binding'
} else {
    Check ($r.text -match ('would: remove ' + [regex]::Escape($orphan) + ': installed by ' + [regex]::Escape($oldVersion))) 'the files stage removes it and names the release that installed it'
}
Check ($r.text -match ('kept: ' + [regex]::Escape($orphanChanged) + ' was installed by ' + [regex]::Escape($oldVersion))) 'a file whose bytes are not the ones of that install is kept and reported'
# Nothing is removed by a wildcard: every removal line names one of the two files of the older manifest.
$removePattern = if ($driverStoreUmd) { '(?m)^\s*\[dry run\] would: remove legacy UMD (.+?) after the new INF binding' } else { '(?m)^\s*\[dry run\] would: remove (.+?): installed by ' }
$removals = @([regex]::Matches($r.text, $removePattern) | ForEach-Object { $_.Groups[1].Value })
Check (((@($removals) -join ' | ') -eq $orphan)) "one removal, and only the file the older manifest names: $(@($removals) -join ', ')"
Check ((Test-Path -LiteralPath $orphan) -and (Test-Path -LiteralPath $orphanChanged) -and (Test-Path -LiteralPath $someoneElse)) 'the dry run removes nothing, the file that belongs to nobody''s release included'
Check ($r.text -notmatch 'would: pnputil /delete-driver') 'no driver package is deleted while the package the GPU uses cannot be read'
if ($r.code -ne 0) { $r.text }
# The same install root with this package's own manifest: a repair must take nothing away.
[IO.File]::Copy((Join-Path $Package 'manifest.json'), (Join-Path $oldRoot 'manifest.json'), $true)
$st.package_version = $pkgVersion
[IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
$env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-Repair') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
# A DriverStore package removes the legacy router paths and Vulkan duplicates at every install, a repair too (da931d14,
# the walk-through above checks it): those are registry values, not files of the install root, so that one line is
# not a removal here.
Check (($r.code -eq 0) -and ($r.text -match ("files of the installed release " + [regex]::Escape($pkgVersion) + " that " + [regex]::Escape($pkgVersion) + " does not install: 0")) -and ($r.text -notmatch 'would: remove (?!legacy router paths and duplicate global Vulkan entries; retain route policy)')) "a repair of the installed version removes nothing (exit $($r.code))"
Remove-Item -LiteralPath $dir -Recurse -Force
Remove-Item -LiteralPath $oldRoot -Recurse -Force
# The invariants of the removal, in the source: the files stage takes the rows of the plan and nothing else, it reads the
# SHA256 again before each removal, and no step of an install removes a whole folder of the install root.
Check (($src -match '(?m)^foreach \(\$row in @\(\$script:OrphanRows \| Where-Object \{ \$_\.state -eq ''remove'' \}\)\) \{') -and ($src -match 'if \(\$now -ne \$row\.sha256\)') -and ($src -match 'Remove-PathOrSchedule \$row\.path')) 'install.ps1 removes only the rows of the plan and checks each SHA256 again before the removal'
Check (($src -notmatch 'Remove-PathOrSchedule \$InstallRoot') -and ($src -notmatch 'Get-ChildItem[^\r\n]*\$InstallRoot[^\r\n]*-Recurse[^\r\n]*Remove')) 'install.ps1 never removes the install root or sweeps it by wildcard'
Check ($common -match '(?m)^function Get-OrphanFilePlan \{') 'the orphan plan is a function of common.ps1 (pure, unit-tested in test-engine-units.ps1)'

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
# BD-122: the probe says why the last start failed, read from Parameters\StartRecord with no gate and without the
# driver. This PC has no such key, so the probe must say that in plain words rather than print nothing.
Check ($r.text -match 'start record: no record: this driver version does not keep one') 'probe: the start record is read and named, absent on a PC without the driver'
$sc = [IO.File]::ReadAllText((Join-Path $Package 'payload\tools\start-confirm.ps1'))
Check ($sc -notmatch "cli health|& `\$cli health") 'start-confirm reads the start health through bc250control.dll, not the CLI'
# release-sources.json holds one row per flavour (schema 4); this package carries the flavour its manifest names.
$cliArgs = @((Join-Path $PSScriptRoot 'test-cli-commands.ps1'), '-Cli', (Join-Path $Package 'payload\tools\bc250kmd_cli.exe'), '-Sources', (Join-Path $PSScriptRoot 'release-sources.json'))
if ($m.flavour) { $cliArgs += @('-Flavour', [string]$m.flavour) }
$r = Invoke-Ps51 $cliArgs
$r.text
Check ($r.code -eq 0) "packaged bc250kmd_cli.exe answers every form of cli-commands.json, one build with the control DLL: exit $($r.code)"
Check ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\bc250control.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'payload\control\bc250control.dll')).Hash) 'tools\bc250control.dll is the control application''s DLL'

'GPU firmware: not in the package, downloaded at install time (test-firmware.ps1 under 5.1, real download into a scratch folder)'
Check (-not (Test-Path -LiteralPath (Join-Path $Package 'payload\firmware'))) 'no payload\firmware in the package'
Check (-not @(Get-ChildItem -LiteralPath $Package -Recurse -File -Filter '*.bin' | Where-Object { $_.FullName -ine (Join-Path $Package 'payload\kmd\amdgpu-state.bin') }).Count) 'no firmware binary in the public package; state catalog is included'
Check (-not @($m.files | Where-Object { $_.path -like 'payload/firmware/*' }).Count) 'manifest files: no firmware'
Check (@($m.firmware.files).Count -eq 9 -and [string]$m.firmware.commit -match '^[0-9a-f]{40}$') "manifest firmware: $(@($m.firmware.files).Count) files at $($m.firmware.commit)"
Check (@($m.components | Where-Object { $_.role -eq 'firmware' -and -not $_.package_path }).Count -eq 9) 'manifest components: 9 firmware files with install path and SHA256, no package path'
Check (Test-DriverStoreFirmware $m.firmware) 'manifest firmware: immutable Driver Store contract'
Check (@($m.components | Where-Object { $_.role -eq 'firmware' -and $_.install_path -like 'DriverStore (bc250kmd.inf)\*.bin' }).Count -eq 8) 'eight firmware binaries belong to the Driver Store package'
$fwWork = Join-Path $WorkBase ('firmware-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test-firmware.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Manifest', (Join-Path $Package 'manifest.json'), '-WorkRoot', $fwWork) -TimeoutSeconds 600
"  (pid $($r.pid), exit $($r.code), no window)"
$r.text
Check ($r.code -eq 0) "firmware download, fallback, offline folder and wrong-hash refusal: exit $($r.code)"
Check (([regex]::Matches($r.text, 'SHA256 [0-9A-F]{64} ok  https://git\.kernel\.org/')).Count -eq 9) 'all 9 files came from the first address (git.kernel.org), not only from the fallback'

'the extras: llama.cpp, Ollama and the models (test-components.ps1 under 5.1, HKCU scratch key, no network)'
$cmPath = Join-Path $Package 'installer\components.json'
if (-not (Test-Path -LiteralPath $cmPath -PathType Leaf)) {
    Check ($null -eq $m.optional_components) 'this package offers no extras, and manifest.json says so'
} else {
    Check ($null -ne $m.optional_components -and [string]$m.optional_components.file -eq 'installer/components.json') 'manifest.json names installer/components.json under optional_components'
    Check (@($m.files | Where-Object { $_.path -eq 'installer/components.json' }).Count -eq 1) 'manifest files: installer/components.json is pinned like every other package file'
    Check (-not @(Get-ChildItem -LiteralPath $Package -Recurse -File -Filter '*.gguf').Count) 'no model file in the package'
    $cmWork = Join-Path $WorkBase ('components-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
    $r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-components.ps1'), '-Installer', (Join-Path $Package 'installer'), '-Source', (Join-Path $PSScriptRoot 'components.json'), '-WorkRoot', $cmWork)
    $r.text
    Check ($r.code -eq 0) "manifest checks, switch combinations, offline folder, hash refusal, install, hooks and uninstall cleanup: exit $($r.code)"
    Remove-Item -LiteralPath $cmWork -Recurse -Force -ErrorAction SilentlyContinue
    'install -DryRun -DryRunIgnoreBoard: the extras are offered and nothing is downloaded'
    $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard')
    Check ($r.text -match '\[dry run\] would ask: Install llama\.cpp for this board\?') 'the llama.cpp question is asked, with its default'
    Check ($r.text -match '\[dry run\] would ask which models to download') 'the model question comes after the components'
    Check ($r.text -match 'models: none') 'no model is chosen by itself'
    Check ($r.text -notmatch 'doing: get .+\.zip') 'a dry run downloads no component'
    'install -DryRun -NoComponents: no question at all'
    $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-NoComponents')
    Check ($r.code -eq 0) "-NoComponents exit $($r.code)"
    Check ($r.text -notmatch 'would ask: Install llama') '-NoComponents asks nothing'
    Check ($r.text -match 'extras: none') '-NoComponents installs nothing'
    'install -DryRun -Components llama -Models gemma12b: the chosen ones, no question'
    $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-Components', 'llama', '-Models', 'gemma12b')
    # Only the extras' questions: the test-signing and BitLocker questions of the computer that runs this test are
    # not the components' business.
    Check ($r.text -notmatch 'would ask: Install |would ask which models') 'with both switches nothing is asked'
    $unpinned = @($m.optional_components.unpinned)
    if ($unpinned -contains 'llama') {
        Check ($r.text -match 'cannot be installed from this package') 'a component without a SHA-256 pin in this package is refused, not installed unchecked'
    } else {
        Check ($r.text -match 'extras to install: llama') 'the component named on the command line is the one installed'
        Check ($r.text -match 'models to download into .+gemma') 'the model named on the command line is the one downloaded'
    }
}

'install -DryRun -DryRunIgnoreBoard -FirmwareDir (offline): a good folder, then a folder with a changed file'
# -NoComponents: this case is about the firmware folder, and the extras would print the licence and the
# download address of a component zip, which the "no address" check below would read as a download.
$r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-NoComponents', '-FirmwareDir', (Join-Path $fwWork 'download'))
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
    try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-NoComponents') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq 0) "$($c.name): exit $($r.code)"
    Check ($r.text -match "from the first run of this install \(driver-pending-restart\): -FirmwareDir $([regex]::Escape($goodDir)) -DpmMaxMHz 1200 -NoControlApp") "$($c.name): the folder, the setting and the switch come from the state"
    # -NoComponents above, so no licence or component address is in this text: every https:// here would be a
    # firmware download, which this case must not make.
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
        Check (($r.text -match 'would: DiInstallDriverW INSTALL_AS_SET') -and ($r.text -notmatch 'would: bcdedit /set')) 'takeover at testsigning-active: phase 2 runs, test signing is not set again'
    }
    if ($r.code -ne $c.code) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

# The setup window's contract with the engine (GUI plan, docs/gui/interfaces-setup.md).
'locale: structured package inventory and unknown readings, five languages'
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-driver-store-locale.ps1'), '-Installer', (Join-Path $Package 'installer'))
$r.text
Check ($r.code -eq 0) 'driver-store locale regression checks pass'
$r = Invoke-Ps51 @((Join-Path $PSScriptRoot 'test-boot-locale.ps1'), '-Installer', (Join-Path $Package 'installer'))
$r.text
Check ($r.code -eq 0) 'boot and BitLocker locale regression checks pass'

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
# The setup window against this package's real engine: a plan run and a dry run through the window's own engine client,
# each screen in four languages without internals (--smoke-engine shows no window). Every package carries the window
# from 0.7.216.100-tester.28 on (build-release.ps1 -SetupApp), so a package without it fails here.
# The unpacked release holds setup.exe in its top folder, one level above the package root;
# a kept repair set or a prepared folder holds its copy in the package root itself (common.ps1, Get-PackageSetupExe).
$setupExe = @((Join-Path $Package 'setup.exe'), (Join-Path $Package '..\setup.exe') | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | ForEach-Object { [IO.Path]::GetFullPath($_) })[0]
Check ([bool]$setupExe) 'the package carries the setup window (setup.exe in the unpacked release''s top folder)'
if ($setupExe) {
    'setup window against the real engine (--smoke-engine: plan and dry run, no window)'
    foreach ($c in @(
            @{ name = 'plan'; flags = @('--plan'); engine = @('-Plan', '-DryRunIgnoreBoard', '-FirmwareDir', $goodDir); expect = @('result: bound', 'outcome: planned', 'view: Plan result.planned.title nothing-changed', 'decision: install') }
            @{ name = 'dry run'; flags = @(); engine = @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning', '-BitLocker', 'HaveKey', '-FirmwareDir', $goodDir); expect = @('result: bound', 'message: result.dry-run-complete', 'mutated: false', 'view: Information result.dry-run-complete.title') })) {
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
    # The setup window is the one file of the zip outside the package root, so the manifest carries it under "setup"
    # instead of in files[] (build-release.ps1). Both hashes are the exe that was packaged.
    Check (($cr0.continuation.setup_exe -eq 'setup.exe') -and ($m.setup) -and ([string]$m.setup.file -eq 'setup.exe') -and
           (([string]$m.setup.sha256) -eq (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash)) 'the compatibility record names the setup window as the continuation, manifest.json hashes it under "setup"'
    Check (@($m.files | Where-Object { $_.path -like '*amdgpu_wddm_setup.exe' -or $_.path -eq 'setup.exe' }).Count -eq 0) 'the package root holds no second copy of the setup window'
}
if (Test-Path -LiteralPath $fwWork) { Remove-Item -LiteralPath $fwWork -Recurse -Force }

'package records: compatibility, release notes, witness writer, planned restarts only'
$cr = Get-Content -LiteralPath (Join-Path $Package 'compatibility.json') -Raw | ConvertFrom-Json
Check (($cr.schema -eq 'amdgpu-wddm.compatibility/1') -and $cr.no_live_rebind.method -eq 'di-install-as-set' -and -not $cr.no_live_rebind.reboot_directive -and (@($m.files | Where-Object { $_.path -eq 'compatibility.json' }).Count -eq 1)) 'compatibility.json: INSTALL_AS_SET recorded, listed in manifest.json'
$notesText = Get-Content -LiteralPath (Join-Path $Package 'RELEASE-NOTES.md') -Raw
Check (@('New', 'Fixed', 'Known issues', 'Settings affected' | Where-Object { $notesText -notmatch "(?m)^## $_\s*$" }).Count -eq 0) 'RELEASE-NOTES.md has New / Fixed / Known issues / Settings affected'
Check ((Get-FileHash -LiteralPath (Join-Path $Package 'payload\tools\release-witness.ps1')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Package 'installer\release-witness.ps1')).Hash) 'release-witness.ps1 ships next to the installed start-confirm task'
Check (($sc0 -match "Write-RunningReleaseWitness -InstallRoot \(Split-Path \`$here\) -RecordedBy 'start-confirm'") -and ($sc0.IndexOf('Write-RunningReleaseWitness') -gt $sc0.IndexOf('if ($Probe) {'))) 'the start-confirm task writes the running-release witness (not in -Probe)'
Check (($src -match "Write-RunningReleaseWitness -InstallRoot \`$InstallRoot -RecordedBy 'verify'")) 'verify writes the running-release witness'
$forced = @(foreach ($s in $scripts) { if ($s.text -match 'Restart-Computer|shutdown(\.exe)?\s+/r') { $s.name } })
Check ($forced.Count -eq 0) "no installer script forces a restart (planned ExitWindowsEx after the user's yes only)$(if ($forced.Count) { ': ' + ($forced -join ', ') })"

'uninstall refuses an unreadable store before any removal'
$badInventory = Join-Path $WorkBase 'driver-store-invalid.json'
[IO.File]::WriteAllText($badInventory, 'not JSON')
$env:AMDGPU_WDDM_TEST_DRIVER_STORE = $badInventory
try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\uninstall.ps1'), '-DryRun') }
finally { $env:AMDGPU_WDDM_TEST_DRIVER_STORE = $inventoryFixture }
Check (($r.code -eq 6) -and ($r.text -match 'No removal was started') -and ($r.text -notmatch 'would:|doing:|Uninstall complete')) 'failed inventory cannot start removal or report success'

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
Check (($r.text -match 'would: remove .+\\System32\\bc250umd\.dll') -and ($r.text -match 'would: remove .+\\SysWOW64\\bc250umd\.dll')) 'both stubs of an earlier release removed (neither was there before the install)'
Check ($r.text -match "would: remove '[^']+\\wow64\\vulkan\\radeon_icd\.json' from HKLM:\\SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers") 'the WOW6432Node Khronos entry removed'
# The H.264 encoder MFT keys go at every uninstall, whether this release registered the transform or not. On this
# computer none of them is there, and the step says so instead of naming keys that do not exist.
Check ($r.text -match 'would: remove the H\.264 encoder MFT registration \(no key of ours present\)') 'the encoder registration is removed at uninstall'
$mftAt = $r.text.IndexOf('would: remove the H.264 encoder MFT registration')
$rootAt = $r.text.IndexOf('would: remove ' + (Join-Path $env:ProgramFiles 'amdgpu-wddm'))
Check (($mftAt -gt 0) -and ($rootAt -gt $mftAt)) 'the encoder keys go before the install root, so no COM registration points at a DLL that is gone'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'uninstall dry run with a state: no change, no elevation'
Check (($r.text -match 'would: remove the Start menu shortcut .*\\Nagi Control App\.lnk') -and ($r.text -match 'would: remove the desktop shortcut .*\\Nagi Control App\.lnk')) 'the uninstall removes the new names'
Check ($r.text -match 'shortcuts of an older release \(amdgpu-wddm Control\): none') 'the uninstall looks for the old names too'
# BD-089, the clean slate: every item of ours goes, and the uninstaller ends with the list of what is on the computer.
Check ($r.text -match "would: unregister our scheduled task\(s\): (none on this computer \(this release registers 'amdgpu-wddm start confirm'\)|[^\r\n]*amdgpu-wddm)") 'every scheduled task of ours is unregistered, not only the one of this release'
Check ($r.text -match 'would: remove HKLM:\\SOFTWARE\\amdgpu-wddm \(router policy, application profile, release record\), HKLM:\\SOFTWARE\\WOW6432Node\\amdgpu-wddm and HKCU:\\Software\\amdgpu-wddm') 'the policy keys of both registry views and the app preferences of this account go'
Check ($r.text -match 'would: remove the bc250kmd service entry and its settings in HKLM:\\SYSTEM\\CurrentControlSet\\Services\\bc250kmd') 'the service key goes with its Parameters, which a next install would otherwise keep as the tester''s own settings'
Check (($r.text -match 'would: remove certificate [0-9A-F]{40} from LocalMachine Root and TrustedPublisher') -and ($r.text -match 'no other test certificate of ours in Root or TrustedPublisher|would: remove \d+ more test certificate\(s\) of ours')) 'the certificate of this release goes, and so does one of an older release of ours'
$fpItems = @('install root', 'System32 stub', 'SysWOW64 stub', 'driver store', 'driver service', 'policy keys', 'Vulkan registration', 'H.264 encoder keys', 'scheduled task', 'RunOnce entry', 'shortcuts', 'installer state', 'per-user data', 'certificates', 'GPU firmware', 'control application data')
$fpMissing = @($fpItems | Where-Object { $r.text -notmatch ('(?m)^\s*\[(gone|kept|LEFT)\] ' + [regex]::Escape($_) + '\s') })
Check (($r.text -match '== What is left of this release') -and -not $fpMissing.Count) "the uninstaller ends with the list of what is left, all $($fpItems.Count) items$(if ($fpMissing.Count) { '; missing: ' + ($fpMissing -join ', ') })"
Check ($r.text -match 'Dry run complete: nothing was changed, so the list above is this computer as it is now \(\d+ item\(s\) of this release\)') 'the dry run says that the list is the computer as it is now'
if ($r.code -ne 0) { $r.text }
Remove-Item -LiteralPath $dir -Recurse -Force
$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
# BD-163, the folder of a single-file setup by name. No run of this gate may leave it behind. A folder that was
# already there is named instead of failing the gate: it belongs to an earlier run of a setup exe on this computer,
# only an administrator can remove it, and while it stands it hides this check.
if (($before | ConvertFrom-Json).setuproot) {
    "  note: $script:SetupRoot was already there before this run. An earlier single-file setup left it, an"
    "  administrator removes it, and until then this check cannot see a run that leaves such a folder behind."
} else {
    Check (-not (Test-Path -LiteralPath $script:SetupRoot)) "no unpacked single-file setup is left behind ($script:SetupRoot)"
}
# The reading itself, against a folder of this test: a setup folder that appears is seen, by name.
$probe = Join-Path $WorkBase 'setuproot-probe'
Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue
$realRoot = $script:SetupRoot
$script:SetupRoot = $probe
$p0 = Get-Footprint
[void][IO.Directory]::CreateDirectory($probe)
$p1 = Get-Footprint
Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue
$script:SetupRoot = $realRoot
Check ((-not ($p0 | ConvertFrom-Json).setuproot) -and (($p1 | ConvertFrom-Json).setuproot) -and ($p0 -ne $p1)) `
    'the footprint reading sees a setup folder that appears, by name'
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
