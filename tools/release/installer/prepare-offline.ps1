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
# Windows PowerShell 5.1 syntax only.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$FirmwareDir,                   # the firmware files from a folder instead of a download
    [switch]$Gui,
    [string]$InvocationId,
    [string]$EventsFile,
    [string]$ResultFile
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
. (Join-Path $here 'engine.ps1')
$script:OfflineSetSchema = 'amdgpu-wddm.offline-set/1'
$script:OfflineSetFile = 'offline-set.json'
$script:Partial = $null
function Remove-Partial { if ($script:Partial -and (Test-Path -LiteralPath $script:Partial)) { Remove-Item -LiteralPath $script:Partial -Recurse -Force -ErrorAction SilentlyContinue } }
trap {
    Write-StepFailure $_
    Remove-Partial
    Exit-Engine -Code 6 -Outcome 'failed' -MessageId 'result.step-failed' -Detail ([string]$_.Exception.Message) -Step $script:CurrentStep
}
Initialize-Engine -Gui ([bool]$Gui) -InvocationId $InvocationId -EventsFile $EventsFile -ResultFile $ResultFile -Mode 'prepare-offline'
# Only the destination is written: no install action, no old copies scheduled for deletion at a restart.
$script:OnFirstChange = $null
$script:ScheduleOldCopies = $false
$script:EngineAction = 'prepare-offline'
function Enter-Stage([string]$Id, [string]$Text) { Write-Step $Text; Write-EngineEvent 'stage' ([ordered]@{ id = $Id; text = $Text }) }
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
Write-Host 'amdgpu-wddm: prepare a folder for an installation without internet' -ForegroundColor White
Write-EngineEvent 'start' ([ordered]@{ mode = $script:EngineMode; gui = [bool]$Gui; dry_run = $false; package = $package; contract = $script:EngineContract; destination = $Destination })

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
Set-CancelAvailable $true 'before-copy'
if (Test-CancelRequested) { Remove-Partial; Invoke-CancelPoint 'before-copy' }

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
}
[IO.File]::WriteAllText((Join-Path $script:Partial $script:OfflineSetFile), ($set | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
# The whole folder once more, as install.cmd will see it.
$check = Test-PackageManifest -PackageRoot $script:Partial
if (-not $check.ok) { throw "prepared folder: $($check.detail)" }
$bad = Test-FirmwareFolder $fw (Join-Path $script:Partial 'firmware')
if ($bad.Count) { throw "prepared folder firmware: $($bad -join '; ')" }
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
Write-Host "Prepared: $Destination ($($m.version), $(@($m.files).Count) package files and $(@($fw.files).Count) firmware files checked)." -ForegroundColor Green
Write-Host 'On the BC-250, run install.cmd from that folder. It needs no internet.' -ForegroundColor Green
Exit-Engine -Code 0 -Outcome 'prepared' -MessageId 'result.offline-prepared' -Detail $Destination
