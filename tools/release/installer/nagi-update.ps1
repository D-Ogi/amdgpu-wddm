# Nagi self-update, stage 1 (docs/design/self-update.md): the update manifest, its signature, the apply plan, the
# atomic switch of the apps folder, the data packs and the driver transaction with its rollback decision.
# Windows PowerShell 5.1, the shell of the installer. Dot-sourced by apply.ps1 (the engine of setup --apply), by
# start-confirm.ps1 (the rollback step at the end of each confirmation run), by build-release.ps1 (the signature
# check after signing) and by the host tests test-nagi-update.ps1 and test-nagi-update-release.ps1.
# The pure functions take everything they read as parameters; the functions that change the disk take the paths.

$script:NagiUpdateSchema = 'nagi.update/1'
$script:NagiInstalledSchema = 'nagi.installed/1'
$script:NagiTxnSchema = 'nagi.update-transaction/1'
$script:NagiSwitchSchema = 'nagi.apps-switch/1'
$script:NagiManifestAsset = 'nagi-update.json'
$script:NagiSignatureAsset = 'nagi-update.json.p7s'
# The certificates that may sign an update manifest, by SHA-1 thumbprint. The control application holds the same list
# (SelfUpdate.cs PinnedSigners), and test-nagi-update.ps1 holds the two lists together. A new publisher certificate
# (the EV certificate) is added here and there in one release before the first manifest it signs: the installed
# release checks the next one, so the list of the installed release is the one that counts.
$script:NagiPinnedSigners = @('71B3BFDDB5EFC22F0C133AD55D865D48AD352981')   # the release test certificate (new-release-cert.ps1)
$script:NagiKinds = @('driver', 'apps', 'data')
$script:NagiRollbackAfterFailedBoots = 2
$script:NagiTxnMaxDays = 7
$script:NagiAppsKeep = 2               # the current apps folder and the one before it, for a rollback

# ---- versions ------------------------------------------------------------------------------------------------------
# The tag grammar of UpdateCheck.cs ReleaseVersion: A.B.C.D, A.B.C.D-tester.N, A.B.C.D-tester.NvR (R from 2). A stable
# release ranks above every tester build of its base, tester.N below tester.NvR.
function ConvertTo-NagiRelease([string]$Text) {
    if ($Text -notmatch '^v?(\d{1,6})\.(\d{1,6})\.(\d{1,6})\.(\d{1,6})(-tester\.(\d{1,6})(v(\d{1,3}))?)?$') { return $null }
    $issue = 1
    if ($Matches[8]) { $issue = [int]$Matches[8]; if ($issue -lt 2) { return $null } }
    $tester = [int]::MaxValue
    if ($Matches[6]) { $tester = [int]$Matches[6] }
    return , @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4], $tester, $issue)
}
# -1, 0 or 1; $null when either side is not of the grammar.
function Compare-NagiRelease([string]$A, [string]$B) {
    $x = ConvertTo-NagiRelease $A; $y = ConvertTo-NagiRelease $B
    if ($null -eq $x -or $null -eq $y) { return $null }
    for ($i = 0; $i -lt 6; $i++) { if ($x[$i] -ne $y[$i]) { if ($x[$i] -lt $y[$i]) { return -1 } else { return 1 } } }
    return 0
}

# ---- the manifest --------------------------------------------------------------------------------------------------
# Reads and checks nagi-update.json. Returns @{ ok; reason; manifest }. Every field the plan and the apply read is
# checked here, so that a later step never meets a malformed value.
function Read-NagiUpdateManifest([string]$Path) {
    function No([string]$Why) { return [pscustomobject]@{ ok = $false; reason = $Why; manifest = $null } }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return No 'the update manifest is missing' }
    if ((Get-Item -LiteralPath $Path).Length -gt 1MB) { return No 'the update manifest is larger than 1 MiB' }
    try { $m = [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json } catch { return No "the update manifest is not JSON: $($_.Exception.Message)" }
    if ($m.schema -ne $script:NagiUpdateSchema) { return No "unknown manifest schema '$($m.schema)'" }
    if ($null -eq (ConvertTo-NagiRelease ([string]$m.release))) { return No "release '$($m.release)' is not of the tag grammar" }
    if ($null -eq (ConvertTo-NagiRelease ([string]$m.min_installed))) { return No "min_installed '$($m.min_installed)' is not of the tag grammar" }
    $names = @{}
    foreach ($c in @($m.components)) {
        if (-not $c) { return No 'an empty component' }
        if ([string]$c.name -notmatch '^[a-z][a-z0-9-]{0,39}$') { return No "component name '$($c.name)' is not allowed" }
        if ($names.ContainsKey([string]$c.name)) { return No "component $($c.name) is named twice" }
        $names[[string]$c.name] = $true
        if ($script:NagiKinds -notcontains [string]$c.kind) { return No "component $($c.name) has the unknown kind '$($c.kind)'" }
        if ($null -eq (ConvertTo-NagiRelease ([string]$c.version))) { return No "component $($c.name) has the version '$($c.version)'" }
        if ([string]$c.asset -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$') { return No "component $($c.name) names the asset '$($c.asset)'" }
        if ([string]$c.sha256 -notmatch '^[0-9A-Fa-f]{64}$') { return No "component $($c.name) has no SHA256" }
        if ([string]$c.content_id -notmatch '^[0-9A-Fa-f]{64}$') { return No "component $($c.name) has no content_id" }
        $size = 0L
        if (-not [long]::TryParse([string]$c.size, [ref]$size) -or $size -le 0 -or $size -gt 4GB) { return No "component $($c.name) has the size '$($c.size)'" }
        if ($c.kind -eq 'driver') {
            if ($c.name -ne 'driver') { return No 'the driver component must be named driver' }
            if ([string]$c.driver.kmd_abi -notmatch '^0x[0-9A-Fa-f]{8}$') { return No 'the driver component has no kmd_abi' }
            if ([string]$c.driver.catalog_id -notmatch '^[0-9a-f]{64}$') { return No 'the driver component has no catalog_id' }
            $schema = 0
            if (-not [int]::TryParse([string]$c.driver.state_schema, [ref]$schema) -or $schema -lt 1) { return No 'the driver component has no state_schema' }
        }
        if ($c.kind -in 'apps', 'data') {
            $body = $(if ($c.kind -eq 'apps') { $c.apps } else { $c.data })
            if (-not $body -or @($body.files).Count -eq 0) { return No "component $($c.name) lists no files" }
            foreach ($f in @($body.files)) {
                if (-not (Test-NagiRelativePath ([string]$f.path))) { return No "component $($c.name) names the file '$($f.path)'" }
                if ([string]$f.sha256 -notmatch '^[0-9A-Fa-f]{64}$') { return No "component $($c.name) has no SHA256 for $($f.path)" }
            }
            if ($c.kind -eq 'apps') {
                if ($c.name -ne 'apps') { return No 'the apps component must be named apps' }
                if (@($c.apps.kmd_abis | Where-Object { [string]$_ -match '^0x[0-9A-Fa-f]{8}$' }).Count -eq 0) { return No 'the apps component names no kmd_abis' }
                if (@($c.apps.state_schemas).Count -eq 0) { return No 'the apps component names no state_schemas' }
            }
        }
    }
    if (-not $names.ContainsKey('driver')) { return No 'the manifest has no driver component' }
    if (-not $names.ContainsKey('apps')) { return No 'the manifest has no apps component' }
    return [pscustomobject]@{ ok = $true; reason = $null; manifest = $m }
}

# A path inside a pack: relative, forward slashes, no '..', no drive, no empty part.
function Test-NagiRelativePath([string]$Path) {
    if (-not $Path -or $Path.Length -gt 200) { return $false }
    if ($Path -match '[\\:*?"<>|]' -or $Path.StartsWith('/')) { return $false }
    foreach ($p in $Path.Split('/')) { if ($p -eq '' -or $p -eq '.' -or $p -eq '..') { return $false } }
    return $true
}

# ---- the signature -------------------------------------------------------------------------------------------------
# nagi-update.json.p7s is a detached CMS SignedData (PKCS #7) over the exact bytes of nagi-update.json, SHA-256, as
# signtool sign /p7 /p7ce DetachedSignedData /p7co 1.2.840.113549.1.7.1 writes it. The check is the signature itself
# (CheckSignature with verifySignatureOnly) and the signer's certificate against the pinned list: the test certificate
# is not in the trusted roots of every computer, and a chain to a trusted root alone would admit any publisher.
function Test-NagiManifestSignature {
    param([Parameter(Mandatory)][string]$ManifestPath, [Parameter(Mandatory)][string]$SignaturePath, [string[]]$Pinned = $script:NagiPinnedSigners)
    function No([string]$Why) { return [pscustomobject]@{ ok = $false; reason = $Why; thumbprint = $null } }
    if (-not (Test-Path -LiteralPath $SignaturePath -PathType Leaf)) { return No 'the signature file is missing' }
    if ((Get-Item -LiteralPath $SignaturePath).Length -gt 64KB) { return No 'the signature file is larger than 64 KiB' }
    Add-Type -AssemblyName System.Security
    $content = [Security.Cryptography.Pkcs.ContentInfo]::new([IO.File]::ReadAllBytes($ManifestPath))
    $cms = [Security.Cryptography.Pkcs.SignedCms]::new($content, $true)
    try { $cms.Decode([IO.File]::ReadAllBytes($SignaturePath)) } catch { return No "the signature is not a CMS message: $($_.Exception.Message)" }
    if ($cms.SignerInfos.Count -ne 1) { return No "the signature has $($cms.SignerInfos.Count) signers, one is required" }
    $signer = $cms.SignerInfos[0]
    if (-not $signer.Certificate) { return No 'the signature carries no signer certificate' }
    if ($signer.DigestAlgorithm.Value -ne '2.16.840.1.101.3.4.2.1') { return No "the digest is $($signer.DigestAlgorithm.FriendlyName), SHA-256 is required" }
    try { $signer.CheckSignature($true) } catch { return No "the signature does not match the manifest: $($_.Exception.Message)" }
    $thumb = $signer.Certificate.Thumbprint.ToUpperInvariant()
    if (@($Pinned | ForEach-Object { ([string]$_).ToUpperInvariant() }) -notcontains $thumb) {
        return [pscustomobject]@{ ok = $false; reason = "the signer $thumb is not a pinned publisher"; thumbprint = $thumb }
    }
    return [pscustomobject]@{ ok = $true; reason = $null; thumbprint = $thumb }
}

# The SHA-256 of a file in upper-case hexadecimal. .NET directly: Get-FileHash comes from a module, and a Windows
# PowerShell started from PowerShell 7 can inherit a module path whose Utility module it cannot load.
function Get-NagiFileSha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '') } finally { $stream.Dispose(); $sha.Dispose() }
}

# The SHA-256 and the size of one downloaded asset against its component row.
function Test-NagiAssetFile([string]$Path, $Component) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "$($Component.asset) is missing" }
    $len = (Get-Item -LiteralPath $Path).Length
    if ($len -ne [long]$Component.size) { return "$($Component.asset) has $len bytes, the manifest says $($Component.size)" }
    $h = Get-NagiFileSha256 $Path
    if ($h -ne ([string]$Component.sha256).ToUpperInvariant()) { return "$($Component.asset) has SHA256 $h, the manifest says $($Component.sha256)" }
    return $null
}

# ---- the installed state -------------------------------------------------------------------------------------------
function Get-NagiUpdateRoot { return (Join-Path $env:ProgramData 'amdgpu-wddm\update') }

# What is installed now, from the install root's manifest.json (the last full install) and from installed.json of the
# update folder (the apps and data packs applied after it). An apps record counts only while the control folder is the
# junction to that record's folder: a full install or a manual change after the update voids it.
function Get-NagiInstalledState {
    param([Parameter(Mandatory)][string]$InstallRoot, [string]$UpdateRoot = (Get-NagiUpdateRoot))
    $m = $null
    $mp = Join-Path $InstallRoot 'manifest.json'
    if (Test-Path -LiteralPath $mp -PathType Leaf) { try { $m = Get-Content -LiteralPath $mp -Raw | ConvertFrom-Json } catch { $m = $null } }
    $rec = $null
    $rp = Join-Path $UpdateRoot 'installed.json'
    if (Test-Path -LiteralPath $rp -PathType Leaf) { try { $rec = Get-Content -LiteralPath $rp -Raw | ConvertFrom-Json; if ($rec.schema -ne $script:NagiInstalledSchema) { $rec = $null } } catch { $rec = $null } }
    $layout = Get-NagiAppsLayout -InstallRoot $InstallRoot
    return (Get-NagiInstalledView -Manifest $m -Record $rec -Layout $layout)
}

# Pure: the installed view from the manifest of the last full install, the update record and the apps layout.
function Get-NagiInstalledView($Manifest, $Record, $Layout) {
    $uc = $null
    if ($Manifest -and $Manifest.PSObject.Properties['update_components']) { $uc = $Manifest.update_components }
    $release = $(if ($Manifest) { [string]$Manifest.release } else { $null })
    $abi = $(if ($Manifest) { [string]$Manifest.kmd_abi } else { $null })
    $kmd = $null
    if ($Manifest) { $kmd = @($Manifest.components | Where-Object { $_ -and $_.package_path -eq 'payload/kmd/bc250kmd.sys' } | ForEach-Object { [string]$_.sha256 }) | Select-Object -First 1 }
    $schema = 2      # every release before the update identities wrote state schema 2 (state-package.ps1)
    $catalog = $null
    if ($uc -and $uc.driver) {
        if ($uc.driver.PSObject.Properties['state_schema'] -and $uc.driver.state_schema) { $schema = [int]$uc.driver.state_schema }
        if ($uc.driver.PSObject.Properties['catalog_id']) { $catalog = [string]$uc.driver.catalog_id }
    }
    $driver = [pscustomobject]@{ release = $release; content_id = $(if ($uc -and $uc.driver) { [string]$uc.driver.content_id } else { $null })
        kmd_abi = $abi; kmd_sha256 = $kmd; state_schema = $schema; catalog_id = $catalog }
    # The apps of the last full install: they shipped with its driver, so they speak its ABI and its state schema.
    $apps = [pscustomobject]@{ version = $release; content_id = $(if ($uc -and $uc.apps) { [string]$uc.apps.content_id } else { $null })
        kmd_abis = @($(if ($abi) { $abi })); state_schemas = @($schema); source = 'install' }
    $effective = $release
    $data = @{}
    if ($Record -and $driver.content_id -and [string]$Record.driver_content_id -eq $driver.content_id) {
        # The record belongs to this driver install: the apps and data it applied are in force.
        if ($Record.apps -and $Layout -and $Layout.kind -eq 'junction' -and $Layout.target_leaf -eq [string]$Record.apps.folder) {
            $apps = [pscustomobject]@{ version = [string]$Record.apps.version; content_id = [string]$Record.apps.content_id
                kmd_abis = @($Record.apps.kmd_abis); state_schemas = @($Record.apps.state_schemas | ForEach-Object { [int]$_ }); source = 'update' }
        }
        if ($Record.PSObject.Properties['data'] -and $Record.data) {
            foreach ($p in $Record.data.PSObject.Properties) { $data[$p.Name] = [pscustomobject]@{ version = [string]$p.Value.version; content_id = [string]$p.Value.content_id } }
        }
        if ((Compare-NagiRelease ([string]$Record.release) $release) -eq 1) { $effective = [string]$Record.release }
    }
    return [pscustomobject]@{ release = $effective; driver = $driver; apps = $apps; data = $data; layout = $Layout }
}

# ---- the apply plan ------------------------------------------------------------------------------------------------
# Pure: what an update does, from the checked manifest and the installed view. outcome: apply, up-to-date or refused
# (reason: downgrade, too-old, incompatible). The driver component is the whole release package and the installer
# installs it, apps included; so a plan with the driver never lists the apps pack. tools/release/nagi-update-plan-cases.json
# holds the cases that this function and SelfUpdate.cs Plan must both answer the same way.
function Get-NagiApplyPlan($Manifest, $Installed) {
    function Plan([string]$Outcome, [string]$Reason, $Actions, [bool]$Restart) {
        return [pscustomobject]@{ outcome = $Outcome; reason = $Reason; actions = @($Actions); restart = $Restart }
    }
    $cmp = Compare-NagiRelease ([string]$Manifest.release) ([string]$Installed.release)
    if ($null -eq $cmp) { return (Plan 'refused' 'unknown-installed' @() $false) }
    if ($cmp -lt 0) { return (Plan 'refused' 'downgrade' @() $false) }
    if ($cmp -eq 0) { return (Plan 'up-to-date' $null @() $false) }
    if ((Compare-NagiRelease ([string]$Installed.release) ([string]$Manifest.min_installed)) -lt 0) { return (Plan 'refused' 'too-old' @() $false) }
    $byName = @{}
    foreach ($c in @($Manifest.components)) { $byName[[string]$c.name] = $c }
    $drv = $byName['driver']; $app = $byName['apps']
    $driverChanged = (-not $Installed.driver.content_id) -or ([string]$drv.content_id -ne [string]$Installed.driver.content_id)
    $appsChanged = $driverChanged -or (-not $Installed.apps.content_id) -or ([string]$app.content_id -ne [string]$Installed.apps.content_id)
    # The pair that runs after the update: the new driver or the installed one, and the new apps or the installed ones.
    $finalAbi = $(if ($driverChanged) { [string]$drv.driver.kmd_abi } else { [string]$Installed.driver.kmd_abi })
    $finalSchema = $(if ($driverChanged) { [int]$drv.driver.state_schema } else { [int]$Installed.driver.state_schema })
    $abis = $(if ($appsChanged) { @($app.apps.kmd_abis) } else { @($Installed.apps.kmd_abis) })
    $schemas = $(if ($appsChanged) { @($app.apps.state_schemas | ForEach-Object { [int]$_ }) } else { @($Installed.apps.state_schemas) })
    if (@($abis | Where-Object { ([string]$_).ToUpperInvariant() -eq $finalAbi.ToUpperInvariant() }).Count -eq 0 -or $schemas -notcontains $finalSchema) {
        return (Plan 'refused' 'incompatible' @() $false)
    }
    $actions = New-Object Collections.ArrayList
    if ($driverChanged) { [void]$actions.Add($drv) }
    elseif ($appsChanged) { [void]$actions.Add($app) }
    foreach ($c in @($Manifest.components | Where-Object { $_.kind -eq 'data' })) {
        $have = Get-NagiDataEntry $Installed.data ([string]$c.name)
        if (-not $have -or [string]$have.content_id -ne [string]$c.content_id) { [void]$actions.Add($c) }
    }
    if ($actions.Count -eq 0) { return (Plan 'up-to-date' $null @() $false) }
    return (Plan 'apply' $null $actions.ToArray() $driverChanged)
}

# One installed data pack of the view: a hashtable (Get-NagiInstalledView) or an object read from JSON.
function Get-NagiDataEntry($Data, [string]$Name) {
    if ($null -eq $Data) { return $null }
    if ($Data -is [Collections.IDictionary]) { return $Data[$Name] }
    $p = $Data.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}

# ---- the apps folder -----------------------------------------------------------------------------------------------
# <install root>\control is the folder that every shortcut, task and script names. Before the first apps update it is
# a plain folder (layout classic). An apps update turns it into a junction to <install root>\apps\<release>, and every
# later update moves that junction only (layout junction).
function Get-NagiAppsLayout([Parameter(Mandatory)][string]$InstallRoot) {
    $control = Join-Path $InstallRoot 'control'
    if (-not (Test-Path -LiteralPath $control)) { return [pscustomobject]@{ kind = 'absent'; target = $null; target_leaf = $null } }
    $item = Get-Item -LiteralPath $control -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) { return [pscustomobject]@{ kind = 'classic'; target = $null; target_leaf = $null } }
    $target = Get-NagiJunctionTarget $control
    return [pscustomobject]@{ kind = 'junction'; target = $target; target_leaf = $(if ($target) { Split-Path $target -Leaf } else { $null }) }
}
function Get-NagiJunctionTarget([string]$Path) {
    $t = @((Get-Item -LiteralPath $Path -Force).Target) | Select-Object -First 1
    if (-not $t) { return $null }
    $t = [string]$t
    if ($t.StartsWith('\??\')) { $t = $t.Substring(4) }
    return $t
}
function Test-NagiReparsePoint([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    return ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
}
function New-NagiJunction([string]$Link, [string]$Target) {
    New-Item -ItemType Junction -Path $Link -Value $Target -ErrorAction Stop | Out-Null
}
# A junction goes as a link only: RemoveDirectory on a reparse point never touches the folder it points to. A real
# folder is never removed here.
function Remove-NagiJunction([string]$Link) {
    if (-not (Test-Path -LiteralPath $Link)) { return }
    if (-not (Test-NagiReparsePoint $Link)) { throw "$Link is a real folder, not a junction: it is not removed" }
    [IO.Directory]::Delete($Link)
}
function Write-NagiJournal([string]$Path, $Journal) {
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllText($tmp, ($Journal | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
}

# The switch of <install root>\control to apps\<To>, with a journal (apps\switch.json) so that a stop at any point
# ends in one whole layout: Repair-NagiAppsSwitch finishes or undoes it. The steps:
#   new      control.new is a junction to the new folder; control is unchanged
#   moved    control is gone: a junction is now control.prev, a classic folder is now apps\<ClassicName>
#   switched control.new is control
#   done     control.prev is removed (a link only)
# -StopAfter <step> stops after that step, as a crash there would (host tests only).
function Switch-NagiApps {
    param([Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$To, [string]$ClassicName, [string]$StopAfter)
    $apps = Join-Path $InstallRoot 'apps'
    $control = Join-Path $InstallRoot 'control'
    $new = "$control.new"; $prev = "$control.prev"
    $journalPath = Join-Path $apps 'switch.json'
    $target = Join-Path $apps $To
    if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw "apps\$To is not there" }
    Repair-NagiAppsSwitch -InstallRoot $InstallRoot | Out-Null
    $layout = Get-NagiAppsLayout -InstallRoot $InstallRoot
    if ($layout.kind -eq 'junction' -and $layout.target_leaf -eq $To) { return 'unchanged' }
    if ($layout.kind -eq 'classic') {
        if ($ClassicName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,80}$') { throw 'a classic control folder needs a name for its versioned place' }
        if (Test-Path -LiteralPath (Join-Path $apps $ClassicName)) { throw "apps\$ClassicName is there already" }
    }
    $journal = [ordered]@{ schema = $script:NagiSwitchSchema; to = $To; from = $(if ($layout.kind -eq 'junction') { $layout.target_leaf } elseif ($layout.kind -eq 'classic') { $ClassicName } else { $null })
        layout = $layout.kind; step = 'begin'; utc = [DateTime]::UtcNow.ToString('o') }
    Write-NagiJournal $journalPath $journal
    if (Test-Path -LiteralPath $new) { Remove-NagiJunction $new }
    New-NagiJunction $new $target
    $journal.step = 'new'; Write-NagiJournal $journalPath $journal
    if ($StopAfter -eq 'new') { throw 'stopped after new' }
    if ($layout.kind -eq 'junction') { [IO.Directory]::Move($control, $prev) }
    elseif ($layout.kind -eq 'classic') { [IO.Directory]::Move($control, (Join-Path $apps $ClassicName)) }
    $journal.step = 'moved'; Write-NagiJournal $journalPath $journal
    if ($StopAfter -eq 'moved') { throw 'stopped after moved' }
    [IO.Directory]::Move($new, $control)
    $journal.step = 'switched'; Write-NagiJournal $journalPath $journal
    if ($StopAfter -eq 'switched') { throw 'stopped after switched' }
    if (Test-Path -LiteralPath $prev) { Remove-NagiJunction $prev }
    $journal.step = 'done'; Write-NagiJournal $journalPath $journal
    return 'switched'
}

# Ends an interrupted switch in one whole layout. control there: the switch is finished or never happened, and only
# the helper links go. control gone: the new link goes in when it is there, else the old one comes back.
# Returns none, finished, undone or clean.
function Repair-NagiAppsSwitch([Parameter(Mandatory)][string]$InstallRoot) {
    $apps = Join-Path $InstallRoot 'apps'
    $control = Join-Path $InstallRoot 'control'
    $new = "$control.new"; $prev = "$control.prev"
    $journalPath = Join-Path $apps 'switch.json'
    if (-not (Test-Path -LiteralPath $journalPath)) { return 'none' }
    $j = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    if ($j.step -eq 'done') { return 'clean' }
    $result = 'finished'
    if (-not (Test-Path -LiteralPath $control)) {
        if (Test-NagiReparsePoint $new) { [IO.Directory]::Move($new, $control) }
        elseif (Test-NagiReparsePoint $prev) { [IO.Directory]::Move($prev, $control); $result = 'undone' }
        elseif ($j.layout -eq 'classic' -and $j.from -and (Test-Path -LiteralPath (Join-Path $apps $j.from))) { [IO.Directory]::Move((Join-Path $apps $j.from), $control); $result = 'undone' }
        else { throw "the control folder is gone and the journal $journalPath names nothing to restore" }
    } elseif ($j.step -in 'begin', 'new') { $result = 'undone' }
    if (Test-NagiReparsePoint $new) { Remove-NagiJunction $new }
    if (Test-NagiReparsePoint $prev) { Remove-NagiJunction $prev }
    $j.step = 'done'; $j | Add-Member -NotePropertyName repaired -NotePropertyValue $result -Force
    Write-NagiJournal $journalPath $j
    return $result
}

# Unpacks an apps or data pack into a new folder and checks every file against the manifest's list: no file more, no
# file less, each with its SHA-256. A folder that does not check is removed again.
function Expand-NagiPack {
    param([Parameter(Mandatory)][string]$Zip, [Parameter(Mandatory)][string]$Destination, [Parameter(Mandatory)]$Files)
    if (Test-Path -LiteralPath $Destination) { throw "$Destination is there already" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $want = @{}
    foreach ($f in @($Files)) { $want[[string]$f.path] = ([string]$f.sha256).ToUpperInvariant() }
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $names = @($archive.Entries | Where-Object { $_.FullName -notmatch '/$' } | ForEach-Object { $_.FullName })
        foreach ($n in $names) { if (-not $want.ContainsKey($n) -or -not (Test-NagiRelativePath $n)) { throw "the pack holds $n, which the manifest does not list" } }
        foreach ($k in $want.Keys) { if ($names -notcontains $k) { throw "the pack has no $k" } }
        [void][IO.Directory]::CreateDirectory($Destination)
        try {
            foreach ($e in $archive.Entries) {
                if ($e.FullName -match '/$') { continue }
                $out = Join-Path $Destination ($e.FullName -replace '/', '\')
                [void][IO.Directory]::CreateDirectory((Split-Path $out))
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $false)
                $h = Get-NagiFileSha256 $out
                if ($h -ne $want[$e.FullName]) { throw "$($e.FullName) has SHA256 $h, the manifest says $($want[$e.FullName])" }
            }
        } catch { Remove-Item -LiteralPath $Destination -Recurse -Force -ErrorAction SilentlyContinue; throw }
    } finally { $archive.Dispose() }
}

# The new control application starts and names itself (--version, exit 0) within the bound.
function Test-NagiAppsHealth([string]$Exe, [int]$TimeoutSeconds = 30) {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return [pscustomobject]@{ ok = $false; detail = "$Exe is missing" } }
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.Arguments = '--version'; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    try {
        $p = [Diagnostics.Process]::Start($psi)
        $out = $p.StandardOutput.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { try { $p.Kill() } catch { }; return [pscustomobject]@{ ok = $false; detail = "$Exe --version did not end in $TimeoutSeconds s" } }
        $text = ([string]$out.Result).Trim()
        return [pscustomobject]@{ ok = ($p.ExitCode -eq 0 -and $text.Length -gt 0); detail = "$Exe --version: exit $($p.ExitCode), '$text'" }
    } catch { return [pscustomobject]@{ ok = $false; detail = "$Exe --version: $($_.Exception.Message)" } }
}

# Pure: which apps folders go after a switch to $Current. The current one and the one before it stay; every other
# folder of a release name goes. A folder that is not of the tag grammar (a classic folder's name) stays only when it
# is the previous one.
function Get-NagiAppsPrunePlan([string[]]$Present, [string]$Current, [string]$Previous) {
    return @($Present | Where-Object { $_ -ne $Current -and $_ -ne $Previous })
}

# ---- data packs ----------------------------------------------------------------------------------------------------
# A data pack goes into <update root>\data\<name>\<version>, and data\<name>\current.json names the folder in force.
# The pointer file is replaced in one step (File.Replace), so a reader sees the old folder or the new one.
function Set-NagiDataPointer([string]$Dir, [string]$Version) {
    $p = Join-Path $Dir 'current.json'
    Write-NagiJournal $p ([ordered]@{ schema = 'nagi.data-current/1'; version = $Version; folder = $Version; utc = [DateTime]::UtcNow.ToString('o') })
}

# ---- the driver transaction ----------------------------------------------------------------------------------------
# <update root>\transaction.json: written before setup hands the new release package to the installer, read by the
# start-confirm task after every start. The observations of each boot (the KMD image that ran, the confirmation's exit
# code, the installer's verify phase) decide: commit, wait, rollback, manual, supersede or expire.
function New-NagiTransaction {
    param([Parameter(Mandatory)]$Installed, [Parameter(Mandatory)]$Driver, [Parameter(Mandatory)]$Apps, [bool]$AppsRollback, [string]$Utc = ([DateTime]::UtcNow.ToString('o')))
    return [ordered]@{
        schema = $script:NagiTxnSchema; id = [Guid]::NewGuid().ToString('N'); created_utc = $Utc; state = 'driver-pending'
        from = [ordered]@{ release = $Installed.driver.release; kmd_sha256 = $Installed.driver.kmd_sha256; kmd_abi = $Installed.driver.kmd_abi
            state_schema = $Installed.driver.state_schema; catalog_id = $Installed.driver.catalog_id; apps_folder = $Installed.layout.target_leaf }
        to = [ordered]@{ release = [string]$Driver.version; kmd_sha256 = [string]$Driver.driver.kmd_sha256; kmd_abi = [string]$Driver.driver.kmd_abi
            state_schema = [int]$Driver.driver.state_schema; catalog_id = [string]$Driver.driver.catalog_id; apps_kmd_abis = @($Apps.apps.kmd_abis) }
        apps_rollback = $AppsRollback
        boots = @()
        decision = $null
    }
}

# Pure: the transaction with one observation merged into the record of its boot (one record a boot).
function Add-NagiTxnObservation {
    param([Parameter(Mandatory)]$Txn, [Parameter(Mandatory)][string]$Boot, [string]$KmdSha256, [Nullable[int]]$Confirm, [string]$Verify)
    $boots = New-Object Collections.ArrayList
    $found = $false
    foreach ($b in @($Txn.boots)) {
        if (-not $b) { continue }
        if ([string]$b.boot -eq $Boot) {
            $found = $true
            $b = [pscustomobject]@{ boot = $Boot; kmd_sha256 = $(if ($KmdSha256) { $KmdSha256 } else { $b.kmd_sha256 })
                confirm = $(if ($null -ne $Confirm) { $Confirm } else { $b.confirm }); verify = $(if ($Verify) { $Verify } else { $b.verify }) }
        }
        [void]$boots.Add($b)
    }
    if (-not $found) { [void]$boots.Add([pscustomobject]@{ boot = $Boot; kmd_sha256 = $KmdSha256; confirm = $Confirm; verify = $Verify }) }
    $Txn.boots = $boots.ToArray()
    return $Txn
}

# Pure: the decision over a transaction's observations.
#   commit     one boot of the new KMD with a confirmed start (exit 0, or 6: the guard reset after a healthy start)
#              and a verified install
#   rollback   NagiRollbackAfterFailedBoots boots of the new KMD that failed (exit 1, 3 or 5, or verify-failed), and the
#              old driver can take the state back: the same state schema and catalog, and apps that speak its ABI
#   manual     the same failures, but the rule above does not hold: the user gets the notice, nothing is changed
#   supersede  a KMD that is neither the old nor the new one ran: another install took over
#   expire     no decision after NagiTxnMaxDays days
#   wait       anything else
function Get-NagiTxnDecision {
    param([Parameter(Mandatory)]$Txn, [DateTime]$Now = [DateTime]::UtcNow)
    if ($Txn.state -ne 'driver-pending') { return 'none' }
    $to = ([string]$Txn.to.kmd_sha256).ToUpperInvariant(); $from = ([string]$Txn.from.kmd_sha256).ToUpperInvariant()
    $failed = 0
    foreach ($b in @($Txn.boots)) {
        if (-not $b -or -not $b.kmd_sha256) { continue }
        $k = ([string]$b.kmd_sha256).ToUpperInvariant()
        if ($k -ne $to -and $k -ne $from) { return 'supersede' }
        if ($k -ne $to) { continue }
        $ok = ($null -ne $b.confirm -and [int]$b.confirm -in 0, 6) -and ([string]$b.verify -eq 'verified')
        if ($ok) { return 'commit' }
        $bad = ($null -ne $b.confirm -and [int]$b.confirm -in 1, 3, 5) -or ([string]$b.verify -eq 'verify-failed')
        if ($bad) { $failed++ }
    }
    if ($failed -ge $script:NagiRollbackAfterFailedBoots) {
        $possible = $from -match '^[0-9A-F]{64}$' -and ([int]$Txn.from.state_schema -eq [int]$Txn.to.state_schema) -and
            ([string]$Txn.from.catalog_id) -and ([string]$Txn.from.catalog_id -eq [string]$Txn.to.catalog_id) -and
            ([bool]$Txn.apps_rollback -or (@($Txn.to.apps_kmd_abis) -contains [string]$Txn.from.kmd_abi))
        if ($possible) { return 'rollback' } else { return 'manual' }
    }
    $created = [DateTime]::MinValue
    if ([DateTime]::TryParse([string]$Txn.created_utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]'AdjustToUniversal, AssumeUniversal', [ref]$created) -and
        ($Now - $created).TotalDays -gt $script:NagiTxnMaxDays) { return 'expire' }
    return 'wait'
}

function Read-NagiTransaction([string]$UpdateRoot = (Get-NagiUpdateRoot)) {
    $p = Join-Path $UpdateRoot 'transaction.json'
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    try { $t = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json; if ($t.schema -eq $script:NagiTxnSchema) { return $t } } catch { }
    return $null
}
function Save-NagiTransaction($Txn, [string]$UpdateRoot = (Get-NagiUpdateRoot)) {
    [void][IO.Directory]::CreateDirectory($UpdateRoot)
    Write-NagiJournal (Join-Path $UpdateRoot 'transaction.json') $Txn
}

# The rollback of the driver: Windows puts the device back on the driver package it ran before the update
# (DiRollbackDriver, newdev.h, SDK 10.0.26100). The previous package stays in the driver store for that reason (the
# installer keeps the package of the previous repair set, BD-089). The binding changes at the next start; the next run
# of this step reads which KMD ran and closes the transaction.
function Invoke-NagiDriverRollback([string]$InstanceId) {
    if (-not ('NagiUpdate.Rollback' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace NagiUpdate {
public static class Rollback {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classGuid, IntPtr hwnd);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string instanceId, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true)]
    static extern bool DiRollbackDriver(IntPtr set, ref SP_DEVINFO_DATA data, IntPtr hwnd, int flags, out bool needReboot);
    const int ROLLBACK_FLAG_NO_UI = 1;
    public static int Run(string instanceId, out bool needReboot) {
        needReboot = false;
        IntPtr set = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
        if (set == new IntPtr(-1)) return Marshal.GetLastWin32Error();
        try {
            var d = new SP_DEVINFO_DATA(); d.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiOpenDeviceInfoW(set, instanceId, IntPtr.Zero, 0, ref d)) return Marshal.GetLastWin32Error();
            if (!DiRollbackDriver(set, ref d, IntPtr.Zero, ROLLBACK_FLAG_NO_UI, out needReboot)) return Marshal.GetLastWin32Error();
            return 0;
        } finally { SetupDiDestroyDeviceInfoList(set); }
    }
}
}
'@
    }
    $reboot = $false
    $code = [NagiUpdate.Rollback]::Run($InstanceId, [ref]$reboot)
    return [pscustomobject]@{ ok = ($code -eq 0); code = $code; need_reboot = $reboot }
}

# One step of the transaction, called by start-confirm.ps1 at its end (SYSTEM) and by the installer's verify. It never
# throws and never changes the caller's outcome: it returns one line for the caller's log. The actions are script
# blocks so that the host test drives the whole step without a device:
#   RollbackDriver { param($txn) } -> @{ ok; detail }       SwitchApps { param($folder) } -> 'switched' or a reason
function Invoke-NagiUpdateConfirmStep {
    param([string]$UpdateRoot = (Get-NagiUpdateRoot), [Parameter(Mandatory)][string]$Boot, [string]$KmdSha256, [Nullable[int]]$Confirm, [string]$Verify,
        [scriptblock]$RollbackDriver, [scriptblock]$SwitchApps, [DateTime]$Now = [DateTime]::UtcNow)
    try {
        $txn = Read-NagiTransaction $UpdateRoot
        if (-not $txn) { return 'update transaction: none' }
        if ($txn.state -eq 'rollback-pending') {
            # The rollback ran in an earlier boot: the KMD of this boot says whether Windows took the old package back.
            if ($KmdSha256 -and ([string]$KmdSha256).ToUpperInvariant() -eq ([string]$txn.from.kmd_sha256).ToUpperInvariant()) { $txn.state = 'rolled-back' }
            elseif ($KmdSha256) { $txn.state = 'manual'; $txn.decision = 'the rollback did not bring the previous driver back' }
            Save-NagiTransaction $txn $UpdateRoot
            return "update transaction $($txn.id): $($txn.state)"
        }
        if ($txn.state -ne 'driver-pending') { return "update transaction $($txn.id): $($txn.state)" }
        $txn = Add-NagiTxnObservation -Txn $txn -Boot $Boot -KmdSha256 $KmdSha256 -Confirm $Confirm -Verify $Verify
        $d = Get-NagiTxnDecision -Txn $txn -Now $Now
        $txn.decision = $d
        switch ($d) {
            'commit' { $txn.state = 'committed' }
            'supersede' { $txn.state = 'superseded' }
            'expire' { $txn.state = 'expired' }
            'manual' { $txn.state = 'manual' }
            'rollback' {
                $line = ''
                if ($txn.apps_rollback -and $txn.from.apps_folder -and $SwitchApps) { $line = " apps: $(& $SwitchApps ([string]$txn.from.apps_folder))" }
                $r = $(if ($RollbackDriver) { & $RollbackDriver $txn } else { [pscustomobject]@{ ok = $false; detail = 'no rollback action' } })
                $txn.state = $(if ($r.ok) { 'rollback-pending' } else { 'manual' })
                $txn.decision = "rollback: $($r.detail)$line"
            }
        }
        Save-NagiTransaction $txn $UpdateRoot
        return "update transaction $($txn.id): $d -> $($txn.state)"
    } catch { return "update transaction: not read ($($_.Exception.Message))" }
}

# The step with the real actions of this computer, for start-confirm.ps1 (SYSTEM) and the installer's verify. The
# driver rollback is DiRollbackDriver on the one device whose service is bc250kmd: Windows installs the package that
# the device had before the update again, from the driver store (the installer keeps the previous package there,
# BD-089). The apps go back to the folder of the transaction's from side. Never throws.
function Invoke-NagiUpdateStepHere {
    param([Parameter(Mandatory)][string]$InstallRoot, [string]$Boot, [string]$KmdSha256, [Nullable[int]]$Confirm, [string]$Verify)
    if (-not $Boot) { return 'update transaction: not read (no boot identity)' }
    $rollback = {
        param($txn)
        $dev = @(Get-CimInstance Win32_PnPEntity -Filter "Service='bc250kmd'" -ErrorAction SilentlyContinue)
        if ($dev.Count -ne 1) { return [pscustomobject]@{ ok = $false; detail = "$($dev.Count) devices with the service bc250kmd, one is required" } }
        $r = Invoke-NagiDriverRollback ([string]$dev[0].DeviceID)
        return [pscustomobject]@{ ok = $r.ok; detail = "DiRollbackDriver: code $($r.code), restart needed $($r.need_reboot)" }
    }.GetNewClosure()
    $switch = {
        param($folder)
        try { return (Switch-NagiApps -InstallRoot $InstallRoot -To $folder) } catch { return "not switched: $($_.Exception.Message)" }
    }.GetNewClosure()
    return (Invoke-NagiUpdateConfirmStep -Boot $Boot -KmdSha256 $KmdSha256 -Confirm $Confirm -Verify $Verify -RollbackDriver $rollback -SwitchApps $switch)
}
