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
Check ($r.text -match 'would: set \d+ DWORD values in .*DpmMode=1 DpmMaxMHz=1500') 'phase 2 shows DPM on, 1500 MHz'
Check ($r.text -match "would: scheduled task 'amdgpu-wddm start confirm'") 'phase 2 shows the start-confirm task'
Check ($r.text -match 'would: copy payload\\control') 'phase 2 installs the control application'
Check ($r.text -match 'would: Start menu shortcut .*amdgpu-wddm Control\.lnk') 'phase 2 shows the Start menu shortcut'
Check ($r.text -match 'DesktopRouter: CpuUmdPath, DwmForceCpu 1') 'desktop on the CPU route (BD-058)'
Check ($r.text -match 'Dry run complete') 'walk-through completes'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'walk-through: no change, no elevation'

Check ($r.text -match 'would: copy payload\\system32\\bc250umd\.dll .*same SHA256: kept; in use: replaced by rename') 'stub copy is the safe replacement'
Check ($r.text -match 'C:\\BC250 itself is not changed') 'firmware step leaves C:\BC250 itself alone'
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
