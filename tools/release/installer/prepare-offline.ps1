# Prepares a folder for an installation without network (GUI plan WU-051, G-OFF; the setup window's --prepare-offline,
# or prepare-offline.cmd): this package and the GPU firmware files, each SHA256 checked, in one folder. On the BC-250,
# install.cmd (or the setup window) started from that folder takes the firmware from its firmware\ folder and needs no
# network, also after the restarts of the installation (the folder is staged into the continuation closure first).
#
# It runs on any Windows PC that has this package and, without -FirmwareDir, internet. It writes nothing but the
# destination folder: it is not an install action, takes no engine lock and needs no administrator rights. The folder
# is built as <destination>.partial and renamed only when every file has checked, so a folder named <destination> is
# always complete. An existing prepared folder (offline-set.json) is replaced; any other non-empty folder is refused.
# Layout (docs/gui/interfaces-setup.md, "Offline set"): the package files as manifest.json lists them, firmware\ with
# the files of manifest.json "firmware", and offline-set.json. A kept repair set has the same layout without the last.
# The extras of the release (installer\components.json: our llama.cpp build, our Ollama build, and the models offered
# with them) go into components\ and models\ of the same folder, each SHA256 checked: an install from that folder
# needs no internet for them either. Without -Components the extras the installer offers by default are fetched;
# without -Models no model is.
# Windows PowerShell 5.1 syntax only.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$FirmwareDir,                   # the firmware files from a folder instead of a download
    [string[]]$Components,                  # the extras to fetch as well (ids of components.json); default: the default ones
    [switch]$NoComponents,                  # no extra at all
    [string[]]$Models,                      # the models to fetch as well (ids of components.json); default: none
    [switch]$NoModels,                      # no model at all
    [string]$ComponentDir,                  # the extras and the models from a folder instead of a download
    [switch]$Gui,
    [string]$InvocationId,
    [string]$EventsFile,
    [string]$ResultFile,
    [string]$DeadlineUtc                    # ISO 8601 UTC: stop at the first stop point after this time
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
. (Join-Path $here 'engine.ps1')
. (Join-Path $here 'release-witness.ps1')
. (Join-Path $here 'components.ps1')
$script:OfflineSetSchema = 'amdgpu-wddm.offline-set/1'
$script:OfflineSetFile = 'offline-set.json'
$script:Partial = $null
function Remove-Partial { if ($script:Partial -and (Test-Path -LiteralPath $script:Partial)) { Remove-Item -LiteralPath $script:Partial -Recurse -Force -ErrorAction SilentlyContinue } }
trap {
    Write-StepFailure $_
    Remove-Partial
    Exit-Engine -Code 6 -Outcome 'failed' -MessageId 'result.step-failed' -Detail ([string]$_.Exception.Message) -Step $script:CurrentStep
}
Initialize-Engine -Gui ([bool]$Gui) -InvocationId $InvocationId -EventsFile $EventsFile -ResultFile $ResultFile -DeadlineUtc $DeadlineUtc -Mode 'prepare-offline'
# Only the destination is written: no install action, no old copies scheduled for deletion at a restart.
$script:OnFirstChange = $null
$script:ScheduleOldCopies = $false
$script:EngineAction = 'prepare-offline'
# Only <destination>.partial is written before 'finish': every stage boundary before it is a stop point, and a stop
# removes the partial folder. The rename in 'finish' is not stopped.
$script:OnStop = { Remove-Partial }
function Enter-Stage([string]$Id, [string]$Text) {
    if ($Id -ne 'finish') { Invoke-CancelPoint "stage:$Id" } else { Write-DeadlineNotice "stage:$Id" }
    Write-Step $Text; Write-EngineEvent 'stage' ([ordered]@{ id = $Id; text = $Text })
}
function Stop-Refused([string]$CheckId, [string]$MessageId, [string]$Detail) {
    Write-Fail $Detail
    Write-Host 'Nothing was prepared.' -ForegroundColor Red
    Remove-Partial
    $script:EngineFailedChecks = @($CheckId)
    Write-EngineEvent 'check' ([ordered]@{ id = $CheckId; result = 'fail'; name = $CheckId; detail = $Detail })
    Exit-Engine -Code 2 -Outcome 'refused' -MessageId $MessageId -Detail $Detail
}

$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination).TrimEnd('\')
if ($FirmwareDir) { $FirmwareDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($FirmwareDir) }
if ($ComponentDir) { $ComponentDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ComponentDir) }
Write-Host 'amdgpu-wddm: prepare a folder for an installation without internet' -ForegroundColor White
Write-EngineEvent 'start' ([ordered]@{ mode = $script:EngineMode; gui = [bool]$Gui; dry_run = $false; package = $package; contract = $script:EngineContract; destination = $Destination; deadline_utc = $script:EngineDeadlineText; job = $script:EngineJob })

Enter-Stage 'prepare-check' 'Package and destination'
$integrity = Test-PackageManifest -PackageRoot $package
if (-not $integrity.ok) { Stop-Refused 'package.damaged' 'result.package-damaged' "package $package`: $($integrity.detail)" }
$m = $integrity.manifest
$script:EnginePackageVersion = [string]$m.version
Write-Info $integrity.detail
$pkgFull = [IO.Path]::GetFullPath($package).TrimEnd('\')
if ((Test-SamePath $Destination $pkgFull) -or $Destination.StartsWith($pkgFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    Stop-Refused 'prepare.destination-inside' 'result.prepare-destination' "the destination $Destination is this package's own folder or inside it"
}
$replace = $false
if (Test-Path -LiteralPath $Destination) {
    if (-not (Test-Path -LiteralPath $Destination -PathType Container)) { Stop-Refused 'prepare.destination-file' 'result.prepare-destination' "$Destination is a file" }
    if (Test-Path -LiteralPath (Join-Path $Destination $script:OfflineSetFile)) { $replace = $true; Write-Info "$Destination is a prepared folder: it is replaced when the new one is complete" }
    elseif (@(Get-ChildItem -LiteralPath $Destination -Force).Count) { Stop-Refused 'prepare.destination-not-empty' 'result.prepare-destination' "$Destination is not empty; choose an empty or a new folder" }
}
$fw = $m.firmware
if (-not $fw -or -not @($fw.files).Count) { Stop-Refused 'firmware.no-list' 'result.package-damaged' 'manifest.json has no firmware list' }
$script:Partial = $Destination + '.partial'
if (Test-Path -LiteralPath $script:Partial) { Remove-Item -LiteralPath $script:Partial -Recurse -Force }

Enter-Stage 'firmware' 'GPU firmware'
if ($FirmwareDir) {
    $bad = Test-FirmwareFolder $fw $FirmwareDir
    if ($bad.Count) { Stop-Refused 'firmware.folder-bad' 'result.firmware-folder-bad' "$FirmwareDir`: $($bad -join '; ')" }
    $fwSource = $FirmwareDir
    Write-Info "$(@($fw.files).Count) files in $FirmwareDir match the pinned SHA256"
} else {
    $fwSource = Join-Path $script:Partial 'firmware-download'
    try { [void](Get-FirmwareStaged -Firmware $fw -Staging $fwSource) }
    catch { Stop-Refused 'firmware.unreachable' 'result.firmware-unreachable' $_.Exception.Message }
}
# The extras (components.ps1) go into <destination>\components and <destination>\models, each SHA256 checked, so an
# install from this folder needs no internet for them either. Without -Components the extras that the installer offers
# by default are fetched; without -Models no model is (a model is gigabytes, and nobody chose one here). A release
# whose extras have no pin yet, and a run that asked for none, add no stage at all.
$componentSet = @()
$modelSet = @()
$cm = $null
$cmProblems = @()
try {
    $cm = Read-ComponentsManifest $here
    if ($null -ne $cm) {
        $cmProblems = @(Test-ComponentsManifest $cm)
        if ($cmProblems.Count) { $cm = $null }
    }
} catch { $cmProblems = @("installer\components.json does not parse: $($_.Exception.Message)") }
$wantComponents = @()
$wantModels = @()
$skipped = @()
if ($null -ne $cm) {
    $req = Resolve-ComponentRequest -Manifest $cm -Requested $Components -NoComponents ([bool]$NoComponents) -CanAsk $false -DefaultsWithoutAsking $true
    foreach ($u in @($req.unknown)) { $skipped += "-Components names '$u', which this release does not have" }
    foreach ($id in @($req.ids)) {
        $c = Get-ComponentById $cm $id
        $problem = Get-ComponentPinProblem $c
        if ($problem) { $skipped += "$([string]$c.title): $problem"; continue }
        $wantComponents += $id
    }
    $mreq = Resolve-ModelRequest -Manifest $cm -Requested $Models -NoModels ([bool]$NoModels) -CanAsk $false -ComponentIds $wantComponents
    foreach ($u in @($mreq.unknown)) { $skipped += "-Models names '$u', which this release does not offer" }
    foreach ($u in @($mreq.unusable)) { $skipped += "-Models names '$u', which none of the prepared extras can use" }
    $wantModels = @($mreq.ids)
}
if ($cmProblems.Count -or @($wantComponents).Count -or @($wantModels).Count -or @($skipped).Count) {
    Enter-Stage 'components' 'Extras: llama.cpp, Ollama, models'
    if ($cmProblems.Count) { Write-Warn2 "the extras of this package cannot be read ($($cmProblems -join '; ')): none of them is prepared" }
    foreach ($one in $skipped) { Write-Warn2 "$one`: it is not prepared" }
    foreach ($id in $wantComponents) {
        $c = Get-ComponentById $cm $id
        try {
            # The zips of a folder that was itself prepared are in its components\ subfolder.
            [void](Get-ComponentAssetStaged -Manifest $cm -Component $c -Staging (Get-SetComponentDir $script:Partial) -FromDir (Get-ComponentSourceDir $ComponentDir))
            $componentSet += [ordered]@{ id = $id; name = [string]$c.asset.name; sha256 = ([string]$c.asset.sha256).ToUpperInvariant() }
        } catch { Write-Warn2 "$([string]$c.title) is not prepared: $($_.Exception.Message)" }
    }
    foreach ($id in $wantModels) {
        $mdl = Get-ComponentModelById $cm $id
        Write-Info "$([string]$mdl.file) ($(Format-ComponentSize ([long]$mdl.size)), licence $([string]$mdl.licence): $([string]$mdl.licence_url))"
        try {
            # The model files of a folder that was itself prepared are in its models\ subfolder, not next to the zips.
            [void](Get-ModelFile -Model $mdl -ModelsDir (Get-SetModelDir $script:Partial) -FromDir (Get-ModelSourceDir $ComponentDir))
            $modelSet += [ordered]@{ id = $id; file = [string]$mdl.file; sha256 = ([string]$mdl.sha256).ToUpperInvariant() }
        } catch { Write-Warn2 "$([string]$mdl.file) is not prepared: $($_.Exception.Message)" }
    }
    Write-Info "prepared: $(@($componentSet).Count) extra(s), $(@($modelSet).Count) model(s)"
    # A folder that nothing could be put into is not left behind: an install reads a folder only when it has files.
    foreach ($d in @((Get-SetComponentDir $script:Partial), (Get-SetModelDir $script:Partial))) {
        if ((Test-Path -LiteralPath $d -PathType Container) -and -not @(Get-ChildItem -LiteralPath $d -Force).Count) { Remove-Item -LiteralPath $d -Force }
    }
}
Set-CancelAvailable $true 'before-copy'
Invoke-CancelPoint 'before-copy'

Enter-Stage 'copy' 'Copy and check'
$script:CurrentStep = "copy the package and the firmware into $($script:Partial)"
[void](Save-ContinuationClosure -PackageRoot $package -Manifest $m -FirmwareDir $fwSource -Destination $script:Partial)
if (-not $FirmwareDir) { Remove-Item -LiteralPath $fwSource -Recurse -Force }
$set = [ordered]@{
    schema = $script:OfflineSetSchema
    version = [string]$m.version
    name = [string]$m.name
    manifest_sha256 = (Get-Sha256 (Join-Path $script:Partial 'manifest.json'))
    prepared_utc = [DateTime]::UtcNow.ToString('o')
    firmware = [ordered]@{ commit = [string]$fw.commit; dir = 'firmware'; source = $(if ($FirmwareDir) { 'folder' } else { 'download' })
        files = @(@($fw.files) | ForEach-Object { [ordered]@{ name = [string]$_.name; sha256 = ([string]$_.sha256).ToUpperInvariant() } }) }
    components = [ordered]@{ dir = $script:ComponentSetDir; source = $(if ($ComponentDir) { 'folder' } else { 'download' }); files = @($componentSet) }
    models = [ordered]@{ dir = $script:ModelSetDir; source = $(if ($ComponentDir) { 'folder' } else { 'download' }); files = @($modelSet) }
}
[IO.File]::WriteAllText((Join-Path $script:Partial $script:OfflineSetFile), ($set | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
# The whole folder once more, as install.cmd will see it.
$check = Test-PackageManifest -PackageRoot $script:Partial
if (-not $check.ok) { throw "prepared folder: $($check.detail)" }
$bad = Test-FirmwareFolder $fw (Join-Path $script:Partial 'firmware')
if ($bad.Count) { throw "prepared folder firmware: $($bad -join '; ')" }
Invoke-CancelPoint 'after-copy'
Set-CancelAvailable $false 'finish'

Enter-Stage 'finish' 'Finish'
$script:CurrentStep = "rename $($script:Partial) -> $Destination"
if ($replace) {
    $old = $Destination + '.old-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    [IO.Directory]::Move($Destination, $old)
    try { [IO.Directory]::Move($script:Partial, $Destination) } catch { [IO.Directory]::Move($old, $Destination); throw }
    Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
} else {
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Force }   # the empty folder
    [IO.Directory]::Move($script:Partial, $Destination)
}
$script:Partial = $null
Write-Host ''
Write-Host "Prepared: $Destination ($($m.version), $(@($m.files).Count) package files and $(@($fw.files).Count) firmware files checked; $(@($componentSet).Count) extra(s) and $(@($modelSet).Count) model(s))." -ForegroundColor Green
Write-Host 'On the BC-250, run install.cmd from that folder. It needs no internet.' -ForegroundColor Green
Exit-Engine -Code 0 -Outcome 'prepared' -MessageId 'result.offline-prepared' -Detail $Destination
