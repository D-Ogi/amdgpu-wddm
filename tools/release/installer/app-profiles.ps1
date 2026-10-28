# The machine profile directory, <system drive>\ProgramData\amdgpu-wddm\profiles (docs/design/app-profiles.md).
# The Mesa drivers, DXVK and vkd3d-proton read their game profiles from it at run time, in their own file formats:
#   drirc.d\*.conf       Mesa drirc XML, as /etc/drirc.d
#   dxvk.conf            dxvk.conf with an [executable] section for each option
#   vkd3d-proton.conf    config flags per executable
# A driver reads the directory and a file only when SYSTEM, Administrators or TrustedInstaller own them, no other
# account can change them and neither is a link or a junction. This file makes the directory so: administrators and
# SYSTEM full control, users and app containers read. %ProgramData%\amdgpu-wddm itself lets users add folders, so a
# user can make "profiles" before the first install; the installer then moves that folder aside, because resetting
# its owner would make the user's files trusted. An uninstall keeps the directory: its files belong to the
# administrator, not to a release.
# Dot-sourced by install.ps1 after common.ps1 (Invoke-Change, Write-Info, Write-Warn2, $script:DryRunMode).

# Owner Administrators; protected: SYSTEM and Administrators full control, Users, ALL APPLICATION PACKAGES and ALL
# RESTRICTED APPLICATION PACKAGES read and execute (a game from the Store runs in an app container). Inherited by
# every folder and file below.
$script:AppProfileSddl = 'O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)(A;OICI;0x1200a9;;;S-1-15-2-2)'
$script:AppProfileTrustedSids = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
$script:AppProfileRootOverride = $null                         # host tests: a fixture folder

# The path the drivers read: the drive of the Windows folder, not %ProgramData% of this process.
function Get-AppProfileRoot {
    if ($script:AppProfileRootOverride) { return $script:AppProfileRootOverride }
    $windows = [Environment]::GetFolderPath('Windows')
    return (Join-Path ($windows.Substring(0, 2) + '\ProgramData') 'amdgpu-wddm\profiles')
}

# Why the drivers would refuse this directory, or $null when they would read it. The same rule as the drivers:
# a trusted owner, no write access for another account, no reparse point.
function Get-AppProfileRefusal([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) { return 'it is not a directory' }
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return 'it is a reparse point (a link or a junction)' }
    return (Get-AppProfileAclRefusal (Get-Acl -LiteralPath $Path))
}

function Get-AppProfileAclRefusal($acl) {
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin $script:AppProfileTrustedSids) { return "its owner $owner is not SYSTEM, Administrators or TrustedInstaller" }
    $rights = [Security.AccessControl.FileSystemRights]
    $write = $rights::WriteData -bor $rights::AppendData -bor $rights::DeleteSubdirectoriesAndFiles -bor $rights::Delete -bor
        $rights::ChangePermissions -bor $rights::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -ne 'Allow' -or ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly)) { continue }
        $sid = $rule.IdentityReference.Value
        if ($sid -in $script:AppProfileTrustedSids -or $sid -in 'S-1-3-0', 'S-1-3-4') { continue }   # CREATOR OWNER, OWNER RIGHTS
        if ([int]$rule.FileSystemRights -band ([int]$write -bor 0x10000000 -bor 0x40000000)) { return "it gives write access to $sid" }
    }
    return $null
}

function New-AppProfileSecurity {
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetSecurityDescriptorSddlForm($script:AppProfileSddl)
    return $acl
}

# Makes the directory and drirc.d below it, or brings an existing trusted directory to the rule. Idempotent.
function Initialize-AppProfileDirectory {
    $root = Get-AppProfileRoot
    $sub = Join-Path $root 'drirc.d'
    if ($script:DryRunMode) { Write-Info "(dry run) profile directory $root"; return }
    [void][IO.Directory]::CreateDirectory((Split-Path $root))
    if (Test-Path -LiteralPath $root) {
        $refusal = Get-AppProfileRefusal $root
        if ($refusal) {
            $aside = "$root.refused-$((Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))"
            Write-Warn2 "$root is moved to $aside, because the drivers would refuse it: $refusal. Its files are not read; copy the ones you want back as an administrator."
            # A link is moved as a link: the folder it points to stays where it is.
            [IO.Directory]::Move($root, $aside)
        }
    }
    if (-not (Test-Path -LiteralPath $root)) {
        [void](New-Object IO.DirectoryInfo $root).Create((New-AppProfileSecurity))
    } else {
        (New-Object IO.DirectoryInfo $root).SetAccessControl((New-AppProfileSecurity))
    }
    if ((Test-Path -LiteralPath $sub) -and ((Get-Item -LiteralPath $sub -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "$sub is a reparse point (a link or a junction). Remove it as an administrator and install again."
    }
    if (-not (Test-Path -LiteralPath $sub)) { [void][IO.Directory]::CreateDirectory($sub) }
    # Everything below inherits the rule of the directory; owners stay as they are, and a file that an account other
    # than SYSTEM, Administrators or TrustedInstaller owns is still refused by the drivers.
    if (@(Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue).Count) {
        $n = Invoke-Native icacls.exe @((Join-Path $root '*'), '/reset', '/T', '/C', '/Q')
        if ($n.code -ne 0) { Write-Warn2 "icacls /reset under ${root}: $($n.text)" }
    }
    $readme = Join-Path $root 'README.txt'
    [IO.File]::WriteAllText($readme, (@(
        'Game profiles of the amdgpu-wddm driver. Administrators edit these files; the drivers read them when a program starts.',
        '',
        '  drirc.d\*.conf      Mesa drirc XML, the format of /etc/drirc.d (RADV and the other Mesa drivers)',
        '  dxvk.conf           dxvk.conf, every option in an [executable] section (D3D11)',
        '  vkd3d-proton.conf   [executable] sections with add = and remove = config flags (D3D12)',
        '',
        'A file that does not parse is not applied, and the driver log names the line.',
        'A file must be owned by Administrators: create it from an elevated program, or run',
        '  icacls <file> /setowner *S-1-5-32-544',
        'The settings of the control application for a program win over these files.',
        'Rules: docs/design/app-profiles.md in the amdgpu-wddm repository.') -join "`r`n") + "`r`n")
    $refusal = Get-AppProfileRefusal $root
    if ($refusal) { throw "the profile directory $root is still refused after the install: $refusal" }
    Write-Info "profile directory $root (administrators edit, users read)"
}
