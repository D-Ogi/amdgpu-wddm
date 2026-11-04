# The engine of setup.exe --apply (docs/design/self-update.md): applies one checked update that the control
# application downloaded. Windows PowerShell 5.1, as administrator. The setup window starts it and reads its result
# file; it shows no window and asks no question.
#
#   apply.ps1 -UpdateDir <folder the control application filled> -ResultFile <file> [-InstallRoot <dir>]
#
# Steps: the manifest and its signature are copied into the protected update folder and checked there (a folder the
# user can write is never read twice); the plan is made again against what is installed now; only the assets of the
# plan are copied and checked; then the apps pack switches the control folder, the data packs switch their pointers,
# and a driver change writes the transaction and hands the release package to its own setup window.
#
# Result (one JSON object): outcome applied | up-to-date | refused | failed | handoff | rolled-back, with reason,
# message (a string id of the setup window), release, restart, handoff_setup and detail.
# Exit codes: 0 applied, up-to-date or handoff; 2 refused; 3 rolled back; 6 failed.

param(
    [Parameter(Mandatory)][string]$UpdateDir,
    [Parameter(Mandatory)][string]$ResultFile,
    [string]$InstallRoot,
    [string]$UpdateRoot,
    [switch]$NoAdminCheck,          # host tests only: a scratch install root and update root, no administrator
    [string[]]$Pinned               # host tests only: the pinned signers of the run
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here 'nagi-update.ps1')
if (-not $UpdateRoot) { $UpdateRoot = Get-NagiUpdateRoot }
if (-not $Pinned) { $Pinned = $script:NagiPinnedSigners }
$script:Log = New-Object Collections.Generic.List[string]
function Log([string]$Text) { $script:Log.Add("$([DateTime]::UtcNow.ToString('o')) $Text") }

function Finish([string]$Outcome, [string]$Reason, [string]$Message, [hashtable]$More = @{}) {
    $r = [ordered]@{ schema = 'nagi.apply-result/1'; outcome = $Outcome; reason = $Reason; message = $Message
        release = $script:Release; restart = $false; handoff_setup = $null; detail = $null; log = @($script:Log) }
    foreach ($k in $More.Keys) { $r[$k] = $More[$k] }
    [IO.File]::WriteAllText($ResultFile, ($r | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    try { [IO.File]::WriteAllText((Join-Path $UpdateRoot 'last-apply.json'), ($r | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false)) } catch { }
    $code = switch ($Outcome) { 'refused' { 2 } 'rolled-back' { 3 } 'failed' { 6 } default { 0 } }
    exit $code
}

# The update root: administrators and SYSTEM change it, users read it (the control application reads installed.json).
# A folder that someone else made first, or that is a link, is moved aside, as app-profiles.ps1 does with the
# profile folder: its owner could otherwise change what the next apply reads.
$script:UpdateRootSddl = 'O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)'
function Initialize-UpdateRoot {
    if ($NoAdminCheck) { [void][IO.Directory]::CreateDirectory($UpdateRoot); return }
    if (Test-Path -LiteralPath $UpdateRoot) {
        $item = Get-Item -LiteralPath $UpdateRoot -Force
        $owner = (Get-Acl -LiteralPath $UpdateRoot).GetOwner([Security.Principal.SecurityIdentifier]).Value
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $owner -notin 'S-1-5-32-544', 'S-1-5-18') {
            $aside = "$UpdateRoot.untrusted-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))"
            [IO.Directory]::Move($UpdateRoot, $aside)
            Log "moved $UpdateRoot aside to $aside (owner $owner)"
        }
    }
    [void][IO.Directory]::CreateDirectory($UpdateRoot)
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetSecurityDescriptorSddlForm($script:UpdateRootSddl)
    Set-Acl -LiteralPath $UpdateRoot -AclObject $acl
}

function Copy-Checked([string]$Name, [string]$To) {
    $src = Join-Path $UpdateDir $Name
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "the update folder has no $Name" }
    Copy-Item -LiteralPath $src -Destination $To -Force
    return (Join-Path $To $Name)
}

$script:Release = $null
try {
    if (-not $NoAdminCheck) {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            Finish 'refused' 'not-admin' 'apply.failed' @{ detail = 'apply.ps1 needs administrator rights' }
        }
    }
    if (-not $InstallRoot) {
        try { $InstallRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot } catch { }
        if (-not $InstallRoot) { $InstallRoot = Join-Path $env:ProgramFiles 'amdgpu-wddm' }
    }
    Initialize-UpdateRoot
    # An apps switch that a stop interrupted ends first, so that the plan reads a whole layout.
    $repair = Repair-NagiAppsSwitch -InstallRoot $InstallRoot
    if ($repair -ne 'none' -and $repair -ne 'clean') { Log "an interrupted apps switch was $repair" }

    $incoming = Join-Path $UpdateRoot ('incoming\' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($incoming)
    $mPath = Copy-Checked $script:NagiManifestAsset $incoming
    $sPath = Copy-Checked $script:NagiSignatureAsset $incoming
    $sig = Test-NagiManifestSignature -ManifestPath $mPath -SignaturePath $sPath -Pinned $Pinned
    if (-not $sig.ok) { Finish 'refused' 'signature' 'apply.refused-signature' @{ detail = $sig.reason } }
    Log "signature: pinned signer $($sig.thumbprint)"
    $read = Read-NagiUpdateManifest $mPath
    if (-not $read.ok) { Finish 'refused' 'manifest' 'apply.refused-signature' @{ detail = $read.reason } }
    $m = $read.manifest
    $script:Release = [string]$m.release
    $installed = Get-NagiInstalledState -InstallRoot $InstallRoot -UpdateRoot $UpdateRoot
    $plan = Get-NagiApplyPlan -Manifest $m -Installed $installed
    Log "installed $($installed.release) (driver $($installed.driver.release), apps $($installed.apps.version), layout $($installed.layout.kind)); plan $($plan.outcome) $($plan.reason): $(@($plan.actions | ForEach-Object { $_.name }) -join ', ')"
    if ($plan.outcome -eq 'up-to-date') { Finish 'up-to-date' $null 'apply.up-to-date' }
    if ($plan.outcome -eq 'refused') { Finish 'refused' $plan.reason "apply.refused-$($plan.reason)" }

    # Only the assets of the plan, each checked after the copy into the protected folder.
    $assets = @{}
    foreach ($c in @($plan.actions)) {
        $p = Copy-Checked ([string]$c.asset) $incoming
        $bad = Test-NagiAssetFile $p $c
        if ($bad) { Finish 'refused' 'hash' 'apply.refused-hash' @{ detail = $bad } }
        $assets[[string]$c.name] = $p
    }

    $record = $null
    $rp = Join-Path $UpdateRoot 'installed.json'
    if (Test-Path -LiteralPath $rp) { try { $record = Get-Content -LiteralPath $rp -Raw | ConvertFrom-Json } catch { $record = $null } }
    $newRecord = [ordered]@{ schema = $script:NagiInstalledSchema; release = $installed.release; driver_content_id = $installed.driver.content_id
        apps = $null; data = [ordered]@{}; updated_utc = $null }
    if ($record -and [string]$record.driver_content_id -eq [string]$installed.driver.content_id) {
        if ($record.apps -and $installed.apps.source -eq 'update') { $newRecord.apps = $record.apps }
        if ($record.PSObject.Properties['data'] -and $record.data) { foreach ($p in $record.data.PSObject.Properties) { $newRecord.data[$p.Name] = $p.Value } }
    }
    $driverAction = @($plan.actions | Where-Object { $_.kind -eq 'driver' }) | Select-Object -First 1
    $appsAction = @($plan.actions | Where-Object { $_.kind -eq 'apps' }) | Select-Object -First 1

    if ($appsAction) {
        $apps = Join-Path $InstallRoot 'apps'
        [void][IO.Directory]::CreateDirectory($apps)
        $folder = [string]$appsAction.version
        $dest = Join-Path $apps $folder
        $layout = Get-NagiAppsLayout -InstallRoot $InstallRoot
        if ($layout.kind -eq 'junction' -and $layout.target_leaf -eq $folder) { Finish 'failed' 'in-use' 'apply.failed' @{ detail = "apps\$folder is the folder in use" } }
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
        Expand-NagiPack -Zip $assets['apps'] -Destination $dest -Files $appsAction.apps.files
        Log "apps pack unpacked into $dest, $(@($appsAction.apps.files).Count) files checked"
        $classic = $null
        if ($layout.kind -eq 'classic') {
            $classic = $(if ($installed.apps.version) { "$($installed.apps.version)-installed" } else { 'installed-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') })
        }
        $previous = $(if ($layout.kind -eq 'junction') { $layout.target_leaf } else { $classic })
        try { $sw = Switch-NagiApps -InstallRoot $InstallRoot -To $folder -ClassicName $classic }
        catch {
            Repair-NagiAppsSwitch -InstallRoot $InstallRoot | Out-Null
            Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
            Finish 'refused' 'busy' 'apply.busy' @{ detail = "the control folder was not switched: $($_.Exception.Message)" }
        }
        Log "apps: $sw to $folder (previous $previous)"
        $exe = 'amdgpu_wddm_control.exe'
        try { $named = [string](Get-Content -LiteralPath (Join-Path $InstallRoot 'manifest.json') -Raw | ConvertFrom-Json).control_app_exe; if ($named) { $exe = $named } } catch { }
        $health = Test-NagiAppsHealth (Join-Path (Join-Path $InstallRoot 'control') $exe)
        Log "apps health: $($health.detail)"
        if (-not $health.ok) {
            $back = 'not possible'
            if ($previous) { $back = Switch-NagiApps -InstallRoot $InstallRoot -To $previous }
            Finish 'rolled-back' 'apps-health' 'apply.rolled-back' @{ detail = "$($health.detail); switched back: $back" }
        }
        $newRecord.apps = [ordered]@{ version = $folder; folder = $folder; content_id = [string]$appsAction.content_id
            kmd_abis = @($appsAction.apps.kmd_abis); state_schemas = @($appsAction.apps.state_schemas); previous = $previous }
        $present = @(Get-ChildItem -LiteralPath $apps -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        foreach ($old in (Get-NagiAppsPrunePlan -Present $present -Current $folder -Previous $previous)) {
            try { Remove-Item -LiteralPath (Join-Path $apps $old) -Recurse -Force -ErrorAction Stop; Log "removed apps\$old" } catch { Log "apps\$old stays: $($_.Exception.Message)" }
        }
    }

    foreach ($c in @($plan.actions | Where-Object { $_.kind -eq 'data' })) {
        $dir = Join-Path $UpdateRoot "data\$($c.name)"
        $dest = Join-Path $dir ([string]$c.version)
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
        [void][IO.Directory]::CreateDirectory($dir)
        Expand-NagiPack -Zip $assets[[string]$c.name] -Destination $dest -Files $c.data.files
        Set-NagiDataPointer $dir ([string]$c.version)
        $newRecord.data[[string]$c.name] = [ordered]@{ version = [string]$c.version; content_id = [string]$c.content_id }
        Log "data pack $($c.name) $($c.version) in force"
    }

    if (-not $driverAction) { $newRecord.release = $script:Release }
    $newRecord.updated_utc = [DateTime]::UtcNow.ToString('o')
    Write-NagiJournal $rp $newRecord

    if ($driverAction) {
        # The release package goes to its own setup window, which installs it as every upgrade (restarts, continuation,
        # start confirmation). The transaction is written first, so that the start-confirm task can decide after the
        # restart even when this window is gone.
        $pkgDir = Join-Path $UpdateRoot "driver\$($driverAction.version)"
        if (Test-Path -LiteralPath $pkgDir) { Remove-Item -LiteralPath $pkgDir -Recurse -Force }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($assets['driver'], $pkgDir)
        $top = @(Get-ChildItem -LiteralPath $pkgDir -Directory) | Select-Object -First 1
        $root = $(if ($top -and (Test-Path -LiteralPath (Join-Path $top.FullName 'setup.exe'))) { $top.FullName } else { $pkgDir })
        $pm = Join-Path $root 'files\manifest.json'
        if (-not (Test-Path -LiteralPath $pm)) { Finish 'refused' 'hash' 'apply.refused-hash' @{ detail = 'the release package has no files\manifest.json' } }
        $pmHash = Get-NagiFileSha256 $pm
        if ($driverAction.driver.package_manifest_sha256 -and $pmHash -ne ([string]$driverAction.driver.package_manifest_sha256).ToUpperInvariant()) {
            Finish 'refused' 'hash' 'apply.refused-hash' @{ detail = "the package manifest is $pmHash, the update manifest says $($driverAction.driver.package_manifest_sha256)" }
        }
        $appsComp = @($m.components | Where-Object { $_.kind -eq 'apps' }) | Select-Object -First 1
        $layout = Get-NagiAppsLayout -InstallRoot $InstallRoot
        $txn = New-NagiTransaction -Installed $installed -Driver $driverAction -Apps $appsComp -AppsRollback ($layout.kind -eq 'junction')
        Save-NagiTransaction $txn $UpdateRoot
        Log "transaction $($txn.id): $($txn.from.release) -> $($txn.to.release), handoff to $root\setup.exe"
        Finish 'handoff' $null 'apply.driver-handoff' @{ restart = $true; handoff_setup = (Join-Path $root 'setup.exe') }
    }
    Finish 'applied' $null 'apply.done' @{ detail = "$(@($plan.actions | ForEach-Object { $_.name }) -join ', ') applied" }
} catch {
    Log "stopped: $($_.Exception.Message)"
    Finish 'failed' 'error' 'apply.failed' @{ detail = $_.Exception.Message }
}
