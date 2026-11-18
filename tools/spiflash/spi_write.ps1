# Guarded unit A write/restore. The pinned JSON plan binds the exact change and recovery image.
# Physical setup is an operator precondition. This script never restores or retries automatically.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Image,
    [Parameter(Mandatory)][string]$ImageSha256,
    [string]$BaseSha256 = '',
    [Parameter(Mandatory)][string]$PlanFile,
    [Parameter(Mandatory)][string]$PlanSha256,
    [Parameter(Mandatory)][string]$OutRoot,
    [int]$MaxSectors = 16,
    [switch]$Restore,
    [switch]$SecondReadback,
    [string]$Chip = 'MX25L12835F/MX25L12845E/MX25L12865E',
    [string]$Flashrom = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$ChipSize = 16777216
$Sector = 4096
# One name for each purpose that has explicit consent for an SPI write on unit A.
$ApprovedPurposes = @('vcn-load', 'vcn-repair', 'p6-l1')
$phase = 'inputs'
$arrayWriteInvoked = $false
$statusMayHaveChanged = $false
$out = $null
$mutex = $null
$held = $false
$snapshotHandles = @()
$receipt = [ordered]@{ verdict = 'REFUSED'; phase = $phase; array_write_invoked = $false; status_probes = [ordered]@{} }

function Require([bool]$Condition, [string]$Why) { if (-not $Condition) { throw $Why } }
function Digest([byte[]]$Bytes) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
function Hash-Text([string]$Value) {
    Require ($Value -match '^[0-9a-fA-F]{64}$') 'expected a 64-digit SHA-256'
    $Value.ToLowerInvariant()
}
function Existing-File([string]$Value) {
    $p = (Resolve-Path -LiteralPath $Value).ProviderPath
    Require ([IO.File]::Exists($p)) "not a file: $Value"
    $p
}
function Private-Path([string]$Value) {
    $p = [IO.Path]::GetFullPath($Value)
    Require ([bool]$env:BC250_ROOT) 'set BC250_ROOT to the private workspace'
    $workspace = [IO.Path]::GetFullPath($env:BC250_ROOT).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    Require ($p.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase)) 'private path is outside BC250_ROOT'
    if (Test-Path -LiteralPath $p) {
        Require (((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "reparse path is not permitted: $Value"
    }
    $at = if ([IO.File]::Exists($p)) { [IO.Path]::GetDirectoryName($p) } else { $p }
    while ($at) {
        Require (-not (Test-Path -LiteralPath (Join-Path $at '.git'))) "image/output path is inside a repository: $Value"
        if (Test-Path -LiteralPath $at) {
            Require (((Get-Item -LiteralPath $at -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "reparse path is not permitted: $Value"
        }
        $parent = [IO.Directory]::GetParent($at)
        $at = if ($parent) { $parent.FullName } else { $null }
    }
    $p
}
function Note([string]$Text) {
    Write-Host $Text
    if ($out) { Add-Content -LiteralPath (Join-Path $out 'write-log.txt') -Value $Text }
}
function Invoke-Flashrom([string]$Name, [string[]]$Arguments) {
    $global:LASTEXITCODE = 0
    $text = & $Flashrom @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $out "$Name.txt"), $text)
    Note "$Name exit=$code"
    [pscustomobject]@{ Text = $text; Code = $code }
}
function Probe([string]$Name) {
    $r = Invoke-Flashrom $Name @('-p', 'ch341a_spi', '-c', $Chip, '-V')
    Require ($r.Code -eq 0) "$Name failed"
    Require ($r.Text -notmatch 'Multiple flash chip definitions match') 'multiple chip definitions match'
    $found = [regex]::Matches($r.Text, '(?m)^Found .* flash chip.*$')
    Require ($found.Count -eq 1) 'probe must identify exactly one chip'
    Require ($found[0].Value.Contains('"' + $Chip + '"')) 'probe chip differs from the plan'
    Require ($found[0].Value -match '\(16384 kB, SPI\)') 'probe size or bus differs from the plan'
    Require ($r.Text -match 'status register is (0x[0-9a-fA-F]{2})') 'probe omitted the status register'
    $status = $Matches[1]
    $receipt.status_probes[$Name] = $status
    # The reviewed unlock may clear BP/SRWD (mask 0xBC). Record this permitted change.
    Require (([Convert]::ToInt32($status.Substring(2), 16) -band 0x43) -eq 0x40) "$Name unexpected non-protection status bits: $status"
    if ($Name -eq 'probe-after') { Require ($status -eq '0x40') "protection remains set after access: $status" }
    $status
}
function Read-Image([string]$Name) {
    $file = Join-Path $out "$Name.bin"
    Require (-not (Test-Path -LiteralPath $file)) "read destination exists: $Name"
    $null = Probe ($Name + '-status')
    $script:statusMayHaveChanged = $true # flashrom 1.2 can unlock status bits even for -r.
    $r = Invoke-Flashrom $Name @('-p', 'ch341a_spi', '-c', $Chip, '-r', $file)
    Require ($r.Code -eq 0) "$Name command failed"
    Require ([IO.File]::Exists($file)) "$Name did not produce a file"
    $bytes = [IO.File]::ReadAllBytes($file)
    Require ($bytes.Length -eq $ChipSize) "$Name has the wrong length"
    [pscustomobject]@{ Bytes = $bytes; Sha256 = (Digest $bytes) }
}
function Hex-Bytes([string]$Value) {
    Require ($Value -match '^(?:[0-9a-fA-F]{2})+$') 'edit bytes must be nonempty hexadecimal pairs'
    ,([Convert]::FromHexString($Value))
}
function Difference([byte[]]$Before, [byte[]]$After) {
    $ranges = [Collections.Generic.List[object]]::new()
    $sectors = 0
    $changed = 0
    for ($sectorStart = 0; $sectorStart -lt $ChipSize; $sectorStart += $Sector) {
        # Skip equal sectors in native code rather than looping over every image byte in PowerShell.
        if ([Convert]::ToBase64String($Before, $sectorStart, $Sector) -ceq
            [Convert]::ToBase64String($After, $sectorStart, $Sector)) { continue }
        $sectors++
        $i = $sectorStart
        $end = $sectorStart + $Sector
        while ($i -lt $end) {
            if ($Before[$i] -eq $After[$i]) { $i++; continue }
            $start = $i
            while ($i -lt $end -and $Before[$i] -ne $After[$i]) { $i++; $changed++ }
            if ($ranges.Count -gt 0 -and ($ranges[-1].offset + $ranges[-1].length) -eq $start) {
                $ranges[-1].length += $i - $start
            } else { $ranges.Add([ordered]@{ offset = $start; length = $i - $start }) }
        }
    }
    [ordered]@{ bytes = $changed; sectors_4k = $sectors; ranges = @($ranges.ToArray()) }
}
function Snapshot([string]$Name, [byte[]]$Bytes) {
    $path = Join-Path $out $Name
    $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($Bytes, 0, $Bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    # The held read-only handle permits flashrom reads and denies replacement or concurrent writes.
    $script:snapshotHandles += [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Require ((Digest ([IO.File]::ReadAllBytes($path))) -eq (Digest $Bytes)) 'snapshot hash mismatch under held lock'
    $path
}
function Pin-File([string]$Path, [string]$Hash) {
    $script:snapshotHandles += [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Require ((Digest ([IO.File]::ReadAllBytes($Path))) -eq (Hash-Text $Hash)) "pinned file hash mismatch: $([IO.Path]::GetFileName($Path))"
}

try {
    $imgHash = Hash-Text $ImageSha256
    $planHash = Hash-Text $PlanSha256
    Require ($MaxSectors -ge 1 -and $MaxSectors -le ($ChipSize / $Sector)) 'MaxSectors must be 1..4096'
    $Image = Existing-File $Image
    $null = Private-Path $Image
    $PlanFile = Existing-File $PlanFile
    $OutRoot = Private-Path $OutRoot
    $planBytes = [IO.File]::ReadAllBytes($PlanFile)
    Require ((Digest $planBytes) -eq $planHash) 'plan hash mismatch'
    $plan = [Text.Encoding]::UTF8.GetString($planBytes) | ConvertFrom-Json -AsHashtable
    Require ($plan.schema -eq 1) 'unsupported plan schema'
    # A comparison operator filters a collection instead of comparing it, so the type is checked first.
    Require ($plan.unit -is [string] -and $plan.purpose -is [string] -and
             $plan.unit -ceq 'A' -and $plan.purpose -cin $ApprovedPurposes) 'plan is not for an approved unit A purpose'
    $mode = if ($Restore) { 'restore' } else { 'write' }
    Require ($plan.mode -eq $mode) 'plan mode differs from invocation'
    Require ((Hash-Text $plan.image_sha256) -eq $imgHash) 'plan image hash differs from invocation'
    Require ($plan.chip -eq $Chip -and $plan.chip_size -eq $ChipSize) 'plan chip or size differs'
    $img = [IO.File]::ReadAllBytes($Image)
    Require ($img.Length -eq $ChipSize) 'image has the wrong length'
    Require ((Digest $img) -eq $imgHash) 'image hash mismatch'
    $backupPath = Existing-File $plan.backup.path
    $null = Private-Path $backupPath
    $backup = [IO.File]::ReadAllBytes($backupPath)
    $backupHash = Hash-Text $plan.backup.sha256
    Require ($backup.Length -eq $ChipSize -and (Digest $backup) -eq $backupHash) 'backup identity mismatch'
    if ($Restore) {
        Require ($imgHash -eq $backupHash) 'restore image is not the identified backup'
    } else {
        $baseHash = Hash-Text $BaseSha256
        Require ((Hash-Text $plan.base_sha256) -eq $baseHash -and $backupHash -eq $baseHash) 'base/backup/plan hashes differ'
        Require ($plan.edits -is [array]) 'plan edits must be an array'
        $expected = [byte[]]$backup.Clone()
        $used = [Collections.Generic.HashSet[int]]::new()
        foreach ($edit in $plan.edits) {
            Require ($edit.offset -is [long] -or $edit.offset -is [int]) 'edit offset must be an integer'
            $old = Hex-Bytes $edit.before
            $new = Hex-Bytes $edit.after
            Require ($old.Length -eq $new.Length) 'edit lengths differ'
            Require ($edit.offset -ge 0 -and $edit.offset -le ($ChipSize - $old.Length)) 'edit outside image'
            Require (($used.Count + $old.Length) -le ($MaxSectors * $Sector)) 'declared edits exceed sector budget'
            for ($n = 0; $n -lt $old.Length; $n++) {
                $pos = [int]$edit.offset + $n
                Require ($used.Add($pos)) 'overlapping edits'
                Require ($backup[$pos] -eq $old[$n]) 'edit expected bytes differ from backup'
                $expected[$pos] = $new[$n]
            }
        }
        Require ((Digest $expected) -eq $imgHash) 'candidate differs outside the approved edits'
    }
    if (-not $Flashrom) {
        Require ([bool]$env:BC250_ROOT) 'set BC250_ROOT or give -Flashrom'
        $Flashrom = Join-Path $env:BC250_ROOT 'scratch\t39\spi-tools\root\ucrt64\bin\flashrom.exe'
    }
    $Flashrom = Existing-File $Flashrom
    Pin-File $Flashrom $plan.flashrom_sha256
    Require ($plan.flashrom_dependencies -is [array]) 'flashrom_dependencies must be an array'
    $dependencyNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($dependency in $plan.flashrom_dependencies) {
        $dependencyPath = Existing-File $dependency.path
        Require ([IO.Path]::GetDirectoryName($dependencyPath) -eq [IO.Path]::GetDirectoryName($Flashrom)) 'dependency must be beside flashrom'
        Require ($dependencyNames.Add([IO.Path]::GetFileName($dependencyPath))) 'duplicate dependency'
        Pin-File $dependencyPath $dependency.sha256
    }
    foreach ($required in @('libusb-1.0.dll', 'libftdi1.dll', 'libwinpthread-1.dll')) {
        Require ($dependencyNames.Contains($required)) "missing pinned dependency: $required"
    }
    Require ($plan.flashrom_version -eq 'flashrom v1.2') 'only the reviewed flashrom v1.2 contract is supported'
    $compatibility = Existing-File $plan.write_compatibility.path
    Require ((Digest ([IO.File]::ReadAllBytes($compatibility))) -eq (Hash-Text $plan.write_compatibility.sha256)) 'write compatibility review hash mismatch'

    $mutex = [Threading.Mutex]::new($false, 'Global\BC250-CH341A-SPI')
    $held = $mutex.WaitOne(0)
    Require $held 'another process owns the SPI programmer'
    $name = 'spi-write-unitA-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
    $null = New-Item -ItemType Directory -Path $OutRoot -Force
    $out = Join-Path $OutRoot $name
    $null = New-Item -ItemType Directory -Path $out
    [IO.File]::WriteAllBytes((Join-Path $out 'plan.json'), $planBytes)
    $snapshot = Snapshot 'candidate.bin' $img
    $backupSnapshot = Snapshot 'backup.bin' $backup
    $receipt.plan_sha256 = $planHash
    $receipt.image_sha256 = $imgHash
    $receipt.backup_sha256 = $backupHash
    $receipt.mode = $mode
    $receipt.write_exit = $null
    $receipt.readback_matches = $false
    $receipt.readbacks = 0
    $receipt.recovery = [ordered]@{ backup = $backupSnapshot; sha256 = $backupHash; automatic = $false }
    Note "plan=$planHash image=$imgHash mode=$mode"
    $phase = 'version'
    $version = Invoke-Flashrom 'version' @('--version')
    Require ($version.Code -eq 0 -and $version.Text -match '(?m)^flashrom v1\.2(?:\s|$)') 'flashrom version command failed or differs'
    $phase = 'probe'
    $receipt.status_before = Probe 'probe-before'
    $phase = 'base-read'
    $current = Read-Image 'before1'
    $second = Read-Image 'before2'
    Require ($current.Sha256 -eq $second.Sha256) 'base reads differ'
    if (-not $Restore) { Require ($current.Sha256 -eq $baseHash) 'chip base hash mismatch: obtain a fresh backup and rebuild the candidate' }
    $receipt.base_sha256 = $current.Sha256
    $diff = Difference $current.Bytes $img
    $diff | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'diff.json')
    Note "difference=$($diff.bytes) bytes, $($diff.sectors_4k) logical 4 KiB regions; physical erase extent is not bounded by this count"
    if (-not $Restore) { Require ($diff.sectors_4k -le $MaxSectors) 'changed-sector limit exceeded' }
    if ($diff.bytes -eq 0) {
        $receipt.status_after = Probe 'probe-after'
        $receipt.verdict = 'PASS'
        $receipt.readback_matches = $true
        $successMessage = 'SPI-WRITE VERDICT PASS: chip already holds the image; no array write invoked'
    } else {
        $phase = 'write'
        Require ((Digest ([IO.File]::ReadAllBytes($snapshot))) -eq $imgHash) 'snapshot hash changed before programming'
        $arrayWriteInvoked = $true
        $write = Invoke-Flashrom 'write' @('-p', 'ch341a_spi', '-c', $Chip, '-w', $snapshot)
        $receipt.write_exit = $write.Code
        $receipt.flashrom_verified = $write.Text -match '(?m)^Verifying flash\.\.\. VERIFIED\.\s*$'
        $phase = 'readback'
        # flashrom verifies the whole chip in the write command. One complete readback in a new
        # process is the independent check. A second one is a diagnostic option.
        $readbackNames = if ($SecondReadback) { @('after1', 'after2') } else { @('after1') }
        $allMatch = $true
        foreach ($readbackName in $readbackNames) {
            $after = Read-Image $readbackName
            $receipt.readbacks = [int]$receipt.readbacks + 1
            if ($after.Sha256 -ne $imgHash) { $allMatch = $false }
        }
        $receipt.readback_matches = $allMatch
        $receipt.status_after = Probe 'probe-after'
        Require ($write.Code -eq 0) 'write command failed; readback result is recorded separately'
        Require $receipt.flashrom_verified 'flashrom did not confirm its verification'
        Require $receipt.readback_matches 'readback hash mismatch'
        $receipt.verdict = 'PASS'
        $count = if ($receipt.readbacks -eq 1) { '1 complete readback equals' } else { "$($receipt.readbacks) complete readbacks equal" }
        $successMessage = "SPI-WRITE VERDICT PASS: write succeeded, flashrom verified it and $count $imgHash"
    }
    $phase = 'complete'
    $exitCode = 0
} catch {
    $receipt.verdict = if ($arrayWriteInvoked) { 'FAIL' } else { 'REFUSED' }
    $receipt.error = $_.Exception.Message
    $exitCode = if ($arrayWriteInvoked) { 1 } else { 2 }
    Write-Host "SPI-WRITE VERDICT $($receipt.verdict): $($receipt.error)"
    Write-Host "phase=$phase array_write_invoked=$arrayWriteInvoked status_register_changes_possible=$statusMayHaveChanged"
    if ($arrayWriteInvoked) {
        Write-Host 'Chip state needs recovery assessment. Keep S5. Use the pinned backup and an explicit restore plan; do not boot or retry automatically.'
    }
} finally {
    $receipt.phase = $phase
    $receipt.array_write_invoked = $arrayWriteInvoked
    $receipt.status_register_changes_possible = $statusMayHaveChanged
    if ($out) {
        try { $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'result.json') }
        catch { Write-Host "SPI-WRITE VERDICT FAIL: receipt write failed: $($_.Exception.Message)"; $exitCode = 1 }
    }
    foreach ($handle in $snapshotHandles) { $handle.Dispose() }
    if ($held) { $mutex.ReleaseMutex() }
    if ($mutex) { $mutex.Dispose() }
}
if ($exitCode -eq 0) { Write-Host $successMessage }
exit $exitCode
