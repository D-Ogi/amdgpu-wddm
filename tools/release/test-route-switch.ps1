# Host test of BD-296: the installer's writer of the desktop route's kill switch DwmForceCpu.
#
# Contracts:
#   1. RouteSwitch.cs compiles under Windows PowerShell 5.1 (Add-Type, C# 5), as the installer loads it.
#   2. Write-RouteSwitch writes both copies, the router's (DesktopRouter) and the driver's (Parameters), in one call, and
#      removes both for $null. Without the driver's key it writes neither copy (negative control: the router's copy
#      keeps its value). The writes go to a scratch tree under HKCU, which the test removes again.
#   3. Get-RouteSwitchValue: the value of the router's plan entry as the installer writes it into both copies.
#   4. The legacy Parameters cleanup (BD-191) keeps the driver's copy: the name is outside the state catalog.
# Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-route-switch.ps1
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$repo = Split-Path (Split-Path $PSScriptRoot)
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

# 1. The compile of the installer.
Initialize-RouteSwitch
Check ($PSVersionTable.PSVersion.Major -eq 5) "Windows PowerShell 5.1 ($($PSVersionTable.PSVersion))"
Check ([bool]('AmdgpuWddm.RouteSwitch' -as [type])) 'RouteSwitch.cs compiles with Add-Type'

# 2. Write against a scratch tree.
$name = [AmdgpuWddm.RouteSwitch]::ValueName
$scratch = 'Software\amdgpu-wddm-test\bd296-' + [Guid]::NewGuid().ToString('N')
$root = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($scratch)
try {
    $r = $root.CreateSubKey([AmdgpuWddm.RouteSwitch]::RouterPath); $r.SetValue($name, 0, 'DWord'); $r.Dispose()
    $refused = $false
    try { [void][AmdgpuWddm.RouteSwitch]::Write($root, [Nullable[uint32]]1) } catch { $refused = $_.Exception.Message -match 'neither copy' }
    $c = [AmdgpuWddm.RouteSwitch]::Read($root)
    Check ($refused -and $c.Router.Number -eq 0 -and -not $c.DriverKey) 'no driver key: refused, the router copy keeps 0 (negative control)'
    $root.CreateSubKey([AmdgpuWddm.RouteSwitch]::DriverPath).Dispose()
    $line = [AmdgpuWddm.RouteSwitch]::Write($root, [Nullable[uint32]]1)
    $c = [AmdgpuWddm.RouteSwitch]::Read($root)
    Check ($c.Router.Dword -and $c.Router.Number -eq 1 -and $c.Driver.Dword -and $c.Driver.Number -eq 1 -and -not $c.Mismatch) "Write 1: both copies 1 ($line)"
    [void][AmdgpuWddm.RouteSwitch]::Write($root, [Nullable[uint32]]$null)
    $c = [AmdgpuWddm.RouteSwitch]::Read($root)
    Check (-not $c.Router.Present -and -not $c.Driver.Present) 'Write null: both copies removed'
} finally {
    $root.Dispose()
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($scratch, $false)
}
Check (-not (Test-Path "HKCU:\$scratch")) 'the scratch tree is gone'

# 3. The value of the router's plan entry.
Check ($null -eq (Get-RouteSwitchValue $null)) 'no entry: absent'
Check ((Get-RouteSwitchValue ([pscustomobject]@{ value = 0 })) -eq 0) 'a 0 stays 0'
Check ((Get-RouteSwitchValue ([pscustomobject]@{ value = 1 })) -eq 1) 'a 1 stays 1'
Check ((Get-RouteSwitchValue ([pscustomobject]@{ value = 7 })) -eq 7) 'another number stays itself'
Check ((Get-RouteSwitchValue ([pscustomobject]@{ value = '0' })) -eq 1) 'a string kept by the plan is the CPU route for the router: 1'
$plan = Get-RegistryDefaultPlan -Defaults ([pscustomobject]@{ DwmForceCpu = 0; RequireKmdSwitches = 1 }) -Previous ([pscustomobject]@{ DwmForceCpu = 0 }) -Current @{ DwmForceCpu = 1 }
Check ((Get-RouteSwitchValue @($plan | Where-Object { $_.name -eq 'DwmForceCpu' })[0]) -eq 1) 'a value the tester set is kept, and goes into both copies'

# 4. The legacy cleanup keeps the driver's copy.
$catalog = Get-Content -LiteralPath (Join-Path $repo 'driver\state\catalog.json') -Raw
Check (-not $catalog.Contains('"DwmForceCpu"')) 'DwmForceCpu is outside the state catalog'
$names = [pscustomobject]@{ values = (New-Object 'Collections.Generic.HashSet[string]'); subkeys = (New-Object 'Collections.Generic.HashSet[string]') }
$legacy = Get-LegacyParametersPlan -ValueNames @('DwmForceCpu') -Names $names
Check (@($legacy.kept_values) -contains 'DwmForceCpu') 'the legacy Parameters plan keeps DwmForceCpu'

if ($fail) { "route-switch: FAIL, $fail check(s)"; exit 1 }
'route-switch: PASS'
