# The compatibility record of a release package (GUI plan C7, B10): compatibility.json at the package root, written by
# build-release.ps1 and listed in manifest.json like every other file. It states what a later engine needs to know
# before it may install this package over a newer one (a rollback, G-RB from Ph 3): the engine contract and the
# result version the package's installer speaks, the no-live-rebind boundary (the INF Reboot directive in every
# install section, BD-060), the firmware set with its hashes, the settings table and the KMD it carries. A version
# number, a common signer or an earlier rollback on the lab admit nothing; only a record that verifies does.
# Windows PowerShell 5.1 syntax only (the installer dot-sources it); build-release.ps1 (PowerShell 7) uses it too.

$script:CompatibilitySchema = 'amdgpu-wddm.compatibility/1'
$script:CompatibilityFile = 'compatibility.json'
$script:SettingsTableSchema = 1          # registry-defaults.json "schema" that this engine reads

# The record for a package folder whose INF, installer and payload are final (manifest.json not needed yet).
function New-CompatibilityRecord {
    param([Parameter(Mandatory)][string]$PackageRoot, [Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)]$Firmware,
        [Parameter(Mandatory)][string]$KmdBuild, [Parameter(Mandatory)][string]$KmdAbi, [Parameter(Mandatory)][string]$DriverVer)
    $infRel = 'payload/kmd/bc250kmd.inf'
    $inf = Join-Path $PackageRoot ($infRel -replace '/', '\')
    $lines = [IO.File]::ReadAllLines($inf)
    $tableRel = 'installer/registry-defaults.json'
    $table = Join-Path $PackageRoot ($tableRel -replace '/', '\')
    $tbl = Get-Content -LiteralPath $table -Raw | ConvertFrom-Json
    $setup = Join-Path $PackageRoot ($script:SetupExeRelative)
    return [ordered]@{
        schema = $script:CompatibilitySchema
        version = $Version
        engine = [ordered]@{ contract = $script:EngineContract; result_schema = $script:ResultSchema; event_schema = $script:EventSchema
            witness_schema = $script:WitnessSchema }
        no_live_rebind = [ordered]@{ inf = $infRel; inf_sha256 = (Get-Sha256 $inf); reboot_directive = [bool](Test-InfDefersDeviceRestart $lines)
            install_sections = [string[]](Get-InfInstallSections $lines) }
        firmware = [ordered]@{ commit = [string]$Firmware.commit; install_dir = [string]$Firmware.install_dir
            files = @(@($Firmware.files) | ForEach-Object { [ordered]@{ name = [string]$_.name; sha256 = ([string]$_.sha256).ToUpperInvariant() } }) }
        settings = [ordered]@{ table = $tableRel; table_schema = $tbl.schema; table_sha256 = (Get-Sha256 $table)
            groups = @($tbl.defaults.PSObject.Properties | ForEach-Object { $_.Name }) }
        kmd = [ordered]@{ build = $KmdBuild; abi = $KmdAbi; driver_ver = $DriverVer; sys_sha256 = (Get-Sha256 (Join-Path $PackageRoot 'payload\kmd\bc250kmd.sys')) }
        continuation = [ordered]@{ closure = $true; setup_exe = $(if (Test-Path -LiteralPath $setup) { $script:SetupExeRelative -replace '\\', '/' } else { $null }) }
    }
}

# Does a package's record verify against the package itself and against what this engine speaks? Returns ok and the
# reason ids (compat.*) with a technical detail for the log. -FirmwareDir: also require the complete firmware set there
# (a kept repair set or a prepared folder). Package integrity (manifest.json) is checked first: the record is one of
# its files.
function Test-CompatibilityRecord {
    param([Parameter(Mandatory)][string]$PackageRoot, [string]$FirmwareDir, $CheckedManifest)
    $reasons = New-Object System.Collections.ArrayList
    $details = New-Object System.Collections.ArrayList
    function Add-Reason([string]$Id, [string]$Detail) { [void]$reasons.Add($Id); [void]$details.Add("${Id}: $Detail") }
    # -CheckedManifest: the caller has just checked the package's integrity (install.ps1's preflight) and passes the manifest.
    $m = $CheckedManifest
    if (-not $m) {
        $integrity = Test-PackageManifest -PackageRoot $PackageRoot
        if (-not $integrity.ok) { Add-Reason 'compat.package-damaged' $integrity.detail; return [pscustomobject]@{ ok = $false; reasons = @($reasons); detail = ($details -join ' | '); record = $null } }
        $m = $integrity.manifest
    }
    $p = Join-Path $PackageRoot $script:CompatibilityFile
    $listed = @($m.files | Where-Object { $_.path -eq $script:CompatibilityFile }).Count -eq 1
    $rec = $null
    if (-not (Test-Path -LiteralPath $p) -or -not $listed) { Add-Reason 'compat.missing' "no $($script:CompatibilityFile) listed in manifest.json (a package built before the record existed)" }
    else {
        # A content hash says only that the file is the one built; the record must still be one JSON object of the
        # supported schema. JSON null, a number, a string, an array or an empty file is no record (the text is checked
        # first: Windows PowerShell unrolls a one-element array into its element).
        $raw = [IO.File]::ReadAllText($p).Trim().TrimStart([char]0xFEFF)
        if ($raw -notmatch '^\{') { Add-Reason 'compat.unknown-schema' "not a JSON object: $(if ($raw.Length -gt 40) { $raw.Substring(0, 40) + '...' } elseif ($raw) { $raw } else { 'empty' })" }
        else {
            try { $rec = ConvertFrom-Json -InputObject $raw } catch { Add-Reason 'compat.unreadable' $_.Exception.Message }
            if ($null -ne $rec -and -not ($rec -is [pscustomobject])) { Add-Reason 'compat.unknown-schema' 'not a JSON object'; $rec = $null }
        }
    }
    if ($rec -and $rec.schema -ne $script:CompatibilitySchema) { Add-Reason 'compat.unknown-schema' "schema $($rec.schema)"; $rec = $null }
    if ($rec) {
        if ([string]$rec.version -ne [string]$m.version) { Add-Reason 'compat.other-package' "record of $($rec.version) in package $($m.version)" }
        if ([string]$rec.engine.contract -ne $script:EngineContract -or [string]$rec.engine.result_schema -ne $script:ResultSchema) { Add-Reason 'compat.engine' "engine $($rec.engine.contract), result $($rec.engine.result_schema); this engine speaks $($script:EngineContract), $($script:ResultSchema)" }
        $inf = Join-Path $PackageRoot 'payload\kmd\bc250kmd.inf'
        $infOk = (Test-Path -LiteralPath $inf) -and (Test-InfDefersDeviceRestart ([IO.File]::ReadAllLines($inf)))
        if (-not $rec.no_live_rebind.reboot_directive -or -not $infOk) { Add-Reason 'compat.live-rebind' 'the INF lacks the Reboot directive in an install section: the package can restart a started GPU under the desktop (BD-060)' }
        elseif ((Get-Sha256 $inf) -ne [string]$rec.no_live_rebind.inf_sha256) { Add-Reason 'compat.live-rebind' 'the INF is not the one the record describes' }
        $want = @(@($m.firmware.files) | ForEach-Object { "$($_.name)=$(([string]$_.sha256).ToUpperInvariant())" } | Sort-Object)
        $have = @(@($rec.firmware.files) | ForEach-Object { "$($_.name)=$(([string]$_.sha256).ToUpperInvariant())" } | Sort-Object)
        if (-not $want.Count -or (($want -join ';') -ne ($have -join ';'))) { Add-Reason 'compat.firmware' 'the firmware set of the record differs from manifest.json' }
        elseif ($FirmwareDir) {
            $bad = Test-FirmwareFolder $m.firmware $FirmwareDir
            if ($bad.Count) { Add-Reason 'compat.firmware-incomplete' "${FirmwareDir}: $($bad -join '; ')" }
        }
        if ([string]$rec.settings.table_schema -ne [string]$script:SettingsTableSchema) { Add-Reason 'compat.settings' "settings table schema $($rec.settings.table_schema), this engine reads $($script:SettingsTableSchema)" }
        $kmd = @($m.components | Where-Object { $_ -and $_.package_path -eq 'payload/kmd/bc250kmd.sys' }) | Select-Object -First 1
        if (-not $kmd -or ([string]$kmd.sha256).ToUpperInvariant() -ne ([string]$rec.kmd.sys_sha256).ToUpperInvariant()) { Add-Reason 'compat.kmd' 'the KMD image differs from the record' }
    }
    return [pscustomobject]@{ ok = ($reasons.Count -eq 0); reasons = @($reasons); detail = ($details -join ' | '); record = $rec }
}
