# Host test of installer\app-profiles.ps1, the machine profile directory (docs/design/app-profiles.md).
#   powershell -NoProfile -File tools\release\test-app-profiles.ps1 [-WorkRoot <empty folder>]
# Without elevation: the access rule against the descriptors that the drivers' own host tests use, and the refusal of
# a folder that the test user owns. Elevated (a test PC, never needed for the gate): also the directory that
# Initialize-AppProfileDirectory makes, an untrusted folder moved aside and a second run over its own result.
param([string]$WorkRoot = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\installer\common.ps1"
. "$PSScriptRoot\installer\app-profiles.ps1"
$script:checks = 0
function Check([bool]$ok, [string]$name) { $script:checks++; if (-not $ok) { throw "FAIL: $name" } }
function Acl([string]$sddl) { $a = New-Object Security.AccessControl.DirectorySecurity; $a.SetSecurityDescriptorSddlForm($sddl); return $a }

# The rule of the installer is one that the drivers read; the others are refused, as in their host tests.
Check (-not (Get-AppProfileAclRefusal (Acl $script:AppProfileSddl))) 'the installer rule is trusted'
Check (-not (Get-AppProfileAclRefusal (Acl 'O:BAG:SYD:(A;OICIIO;FA;;;CO)(A;;FA;;;OW)(D;;FA;;;BU)(A;;FA;;;BA)'))) 'templates and deny entries'
Check (-not (Get-AppProfileAclRefusal (Acl 'O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SYD:(A;;FA;;;SY)'))) 'TrustedInstaller as owner'
Check ([bool](Get-AppProfileAclRefusal (Acl 'O:BUG:SYD:(A;;FA;;;BA)'))) 'a user owner'
Check ([bool](Get-AppProfileAclRefusal (Acl 'O:BAG:SYD:(A;;FA;;;BA)(A;;FRFW;;;BU)'))) 'users may write'
Check ([bool](Get-AppProfileAclRefusal (Acl 'O:BAG:SYD:(A;CI;0x4;;;BU)'))) 'users may add folders'
Check ([bool](Get-AppProfileAclRefusal (Acl 'O:BAG:SYD:(A;;GA;;;BU)'))) 'users have generic all'
Check ((Get-AppProfileRoot) -match '^[A-Za-z]:\\ProgramData\\amdgpu-wddm\\profiles$') 'the root is on the drive of the Windows folder'

$work = if ($WorkRoot) { $WorkRoot } else { Join-Path ([IO.Path]::GetTempPath()) ("app-profiles-" + [guid]::NewGuid().ToString('N')) }
[void][IO.Directory]::CreateDirectory($work)
$mine = Join-Path $work 'mine'
[void][IO.Directory]::CreateDirectory($mine)
$r = Get-AppProfileRefusal $mine
$admin = Test-IsAdmin
if (-not $admin) { Check ($r -match 'owner') "a folder of the test user is refused ($r)" }

if ($admin) {
    $script:AppProfileRootOverride = Join-Path $work 'profiles'
    $root = $script:AppProfileRootOverride
    # A user's folder made before the install: moved aside, never made trusted.
    [void][IO.Directory]::CreateDirectory($root)
    $acl = Get-Acl -LiteralPath $root
    $acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-545'), 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    Set-Acl -LiteralPath $root -AclObject $acl
    [IO.File]::WriteAllText((Join-Path $root 'dxvk.conf'), "[game.exe]`r`nd3d11.samplerAnisotropy = 16`r`n")
    Initialize-AppProfileDirectory
    Check (-not (Get-AppProfileRefusal $root)) 'the directory is trusted'
    Check (-not (Test-Path -LiteralPath (Join-Path $root 'dxvk.conf'))) 'the file of the untrusted folder is not made trusted'
    Check (@(Get-ChildItem -LiteralPath $work -Directory -Filter 'profiles.refused-*').Count -eq 1) 'the untrusted folder is moved aside'
    Check (Test-Path -LiteralPath (Join-Path $root 'drirc.d') -PathType Container) 'drirc.d'
    Check ((Get-Acl -LiteralPath $root).AreAccessRulesProtected) 'the directory does not inherit from ProgramData'
    # An administrator's file, then a second run: the file stays and inherits the rule.
    [IO.File]::WriteAllText((Join-Path $root 'drirc.d\10-game.conf'), "<driconf/>`r`n")
    Initialize-AppProfileDirectory
    Check (Test-Path -LiteralPath (Join-Path $root 'drirc.d\10-game.conf')) 'a second run keeps the files'
    Check (@(Get-ChildItem -LiteralPath $work -Directory -Filter 'profiles.refused-*').Count -eq 1) 'a second run moves nothing'
    Check (-not (Get-AppProfileRefusal (Join-Path $root 'drirc.d'))) 'drirc.d is trusted'
}
if (-not $WorkRoot) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output "PASS app-profiles: $($script:checks) checks$(if (-not $admin) { ' (not elevated: the directory steps are not run)' })"
