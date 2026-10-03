# Host test of a built package on a computer WITHOUT a BC-250 (the development PC): the dry runs of install and
# uninstall must run every check, refuse cleanly and change nothing. Never run the real install here.
#   pwsh -File tools\release\test-dryrun.ps1 -Package <unpacked package folder>
# Asserts: all installer scripts parse under Windows PowerShell 5.1; install -DryRun exits 2 (preflight refusal) with
# a 'fail' line for the BC-250 GPU and "Nothing was changed"; uninstall -DryRun exits 0 with nothing to remove;
# %ProgramData%\amdgpu-wddm, HKLM\SOFTWARE\amdgpu-wddm, the RunOnce entry and the scheduled task are the same
# before and after.
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Get-Footprint {
    [ordered]@{
        programdata = Test-Path -LiteralPath (Join-Path $env:ProgramData 'amdgpu-wddm')
        software    = Test-Path -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm'
        runonce     = [bool](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'amdgpu-wddm-installer' -ErrorAction SilentlyContinue)
        task        = [bool](Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue)
        service     = [bool](Get-Service -Name bc250kmd -ErrorAction SilentlyContinue)
        firmware    = Test-Path -LiteralPath 'C:\BC250\firmware'
        programfiles = Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'amdgpu-wddm')
    } | ConvertTo-Json -Compress
}
'parse (Windows PowerShell 5.1)'
& $ps51 -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test-parse51.ps1') -Directory (Join-Path $Package 'installer')
Check ($LASTEXITCODE -eq 0) 'every installer script parses under 5.1'
'ASCII only (5.1 reads a BOM-less script in the ANSI code page)'
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Package 'installer') -File) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    Check (-not ($b | Where-Object { $_ -gt 127 } | Select-Object -First 1)) "$($f.Name) is ASCII"
}
$before = Get-Footprint
'install -DryRun'
$out = & $ps51 -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Package 'installer\install.ps1') -DryRun 2>&1 | Out-String
$code = $LASTEXITCODE
$out
Check ($code -eq 2) "install -DryRun exit $code (2 = preflight refusal)"
Check ($out -match '\[fail\]\s+BC-250 GPU\s+no device PCI\\VEN_1002&DEV_13FE') 'the BC-250 GPU check fails on this computer'
Check ($out -match 'package integrity\s+\d+ files match manifest\.json') 'package integrity passes'
Check ($out -match 'Nothing was changed') 'refusal says nothing was changed'
Check ($out -notmatch 'doing:') 'no change was made (no "doing:" line)'
'install -DryRun -DryRunIgnoreBoard (walks phases 1 and 2, prints every change)'
$out = & $ps51 -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Package 'installer\install.ps1') -DryRun -DryRunIgnoreBoard 2>&1 | Out-String
$code = $LASTEXITCODE
$out
Check ($code -eq 0) "walk-through exit $code"
Check ($out -match 'would: bcdedit /set \{current\} testsigning on') 'phase 1 shows the test-signing change'
Check ($out -match 'would: pnputil /add-driver') 'phase 2 shows the driver install'
Check ($out -match 'would: set \d+ DWORD values in .*DpmMode=1 DpmMaxMHz=1500') 'phase 2 shows DPM on, 1500 MHz'
Check ($out -match "would: scheduled task 'amdgpu-wddm start confirm'") 'phase 2 shows the start-confirm task'
Check ($out -match 'Dry run complete') 'walk-through completes'
Check ($out -notmatch 'doing:') 'walk-through made no change'
'uninstall -DryRun'
$out = & $ps51 -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Package 'installer\uninstall.ps1') -DryRun 2>&1 | Out-String
$code = $LASTEXITCODE
$out
Check ($code -eq 0) "uninstall -DryRun exit $code"
Check ($out -notmatch 'doing:') 'uninstall dry run made no change'
$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
