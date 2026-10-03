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
Check ($r.text -match 'AppRouter: Mode=allowlist \(new\); Allow=\[dxdiag\.exe\] \(new\)') 'D3D11 allowlist from the defaults table'
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
        Check ($r.text -match "would: RunOnce entry 'amdgpu-wddm-installer' -> .*verify\.cmd") "$($c.name): RunOnce verify armed again"
        Check ($r.text -match 'would: .*\\Release: Version') "$($c.name): Release\Version rewritten"
    } else {
        Check ($r.text -notmatch 'would: copy payload|would: pnputil') "$($c.name): nothing is installed"
    }
    if ($r.code -ne 0 -or $r.text -notmatch $c.expect) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

Check ($src -notmatch 'New-Item -Path [^\r\n]*-Force') 'install.ps1 never runs New-Item -Force on a registry key (it deletes the key''s values)'

'verify before and after the restart (test state, phase installed)'
foreach ($c in @(
        @{ name = 'verify before the restart'; utc = [DateTime]::UtcNow.ToString('o'); code = 7; expect = 'Restart pending: the installation finished' }
        @{ name = 'verify after the restart'; utc = '2000-01-01T00:00:00.0000000Z'; code = 3; expect = 'BC-250 GPU not found' })) {
    $dir = Join-Path $WorkBase ('state-verify-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
    [void][IO.Directory]::CreateDirectory($dir)
    $st = [ordered]@{ schema = 1; phase = 'installed'; package_version = $pkgVersion; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = $c.utc }
    [IO.File]::WriteAllText((Join-Path $dir 'state.json'), ($st | ConvertTo-Json))
    $env:AMDGPU_WDDM_TEST_STATE_DIR = $dir
    try { $r = Invoke-Ps51 @((Join-Path $Package 'installer\install.ps1'), '-DryRun', '-Verify') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
    Check ($r.code -eq $c.code) "$($c.name): exit $($r.code) (expected $($c.code))"
    Check ($r.text -match $c.expect) "$($c.name): '$($c.expect)'"
    Check ($r.text -notmatch 'waiting for the start-confirm task') "$($c.name): no wait for the task"
    if ($r.code -ne $c.code) { $r.text }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

'start-confirm.ps1 -Probe (read-only; compiles the bc250control.dll interop and calls Bc250StartHealth READ)'
$r = Invoke-Ps51 @((Join-Path $Package 'payload\tools\start-confirm.ps1'), '-Probe')
$r.text
Check ($r.code -eq 5) "probe on a PC without the driver: exit $($r.code) (5 = no start-health reading)"
Check ($r.text -match 'probe: fallback view: device problem -1, driver version  \(expected 0x000700C5\), LastStage ') 'probe: fallback view runs bc250kmd_cli info/stages and expects 0x000700C5'
Check ($r.text -match 'probe: start health no reading: start health read refused, status 0x[0-9A-F]{8}') 'probe: the DLL loads and Bc250StartHealth answers (no device)'
Check ($r.text -match 'probe: DpmMode no key') 'probe: DPM state read from the registry'
$sc = [IO.File]::ReadAllText((Join-Path $Package 'payload\tools\start-confirm.ps1'))
Check ($sc -notmatch "cli health|& `\$cli health") 'start-confirm does not use the CLI health command (absent from the 0cbef549 CLI)'
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
if (Test-Path -LiteralPath $fwWork) { Remove-Item -LiteralPath $fwWork -Recurse -Force }

'uninstall -DryRun'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\uninstall.ps1'), '-DryRun')
$r.text
Check ($r.code -eq 0) "uninstall -DryRun exit $($r.code)"
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'uninstall dry run: no change, no elevation'

$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
