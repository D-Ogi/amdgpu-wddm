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
param([Parameter(Mandatory)][string]$Package)
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
Check ($r.text -match 'Dry run complete') 'walk-through completes'
Check ($r.text -notmatch 'doing:|Administrator rights are needed') 'walk-through: no change, no elevation'

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
