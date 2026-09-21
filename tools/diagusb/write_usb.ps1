#Requires -Version 7.0
#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Turn one specific USB stick into the BC-250 diagnostic stick. Erases that stick completely.

.DESCRIPTION
    The script refuses to touch a disk unless every one of these holds:
      - the disk is a USB disk (BusType USB),
      - it is neither the boot disk nor the system disk, and does not hold the source tree,
      - it is at most 128 GB,
      - its FriendlyName matches -ExpectedName, which you have to type out yourself,
      - -IUnderstandThisErasesTheDisk is given (not needed for -WhatIf).
    Then: clear the disk, MBR, one active FAT32 partition of at most 32 GB labelled BC250DIAG,
    copy the tree built by build_usb.py, and compare file count and total bytes afterwards.

    Run it with -WhatIf first. That checks all the guards, prints the plan and changes nothing.

.PARAMETER DiskNumber
    Disk number from Get-Disk. Never assume it stayed the same: check it after every replug.

.PARAMETER SourceDir
    The usbroot directory produced by build_usb.py.

.PARAMETER ExpectedName
    The FriendlyName the disk must have, for example 'Samsung Flash Drive FIT'. Wildcards allowed.

.EXAMPLE
    Get-Disk | Format-Table Number, FriendlyName, BusType, Size, IsBoot, IsSystem

.EXAMPLE
    .\write_usb.ps1 -DiskNumber 8 -SourceDir P:\BC-250\scratch\usbroot -ExpectedName 'Samsung Flash Drive FIT' -WhatIf

.EXAMPLE
    .\write_usb.ps1 -DiskNumber 8 -SourceDir P:\BC-250\scratch\usbroot -ExpectedName 'Samsung Flash Drive FIT' -IUnderstandThisErasesTheDisk
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory)][int]$DiskNumber,
    [Parameter(Mandatory)][string]$SourceDir,
    [Parameter(Mandatory)][string]$ExpectedName,
    [switch]$IUnderstandThisErasesTheDisk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# robocopy reports its result in the exit code, and anything up to 7 means success. Without this,
# PowerShell 7.3+ turns that non-zero exit into a terminating error.
$PSNativeCommandUseErrorActionPreference = $false

$MaxDiskSize = 128GB
$MaxPartitionSize = 32GB
$Label = 'BC250DIAG'

function Fail([string]$Message) {
    throw "REFUSED: $Message"
}

function Get-TreeStats([string]$Path) {
    # Windows drops "System Volume Information" onto a fresh volume on its own; it is not ours.
    $skip = Join-Path $Path 'System Volume Information\'
    $files = Get-ChildItem -LiteralPath $Path -Recurse -File -Force |
        Where-Object { -not $_.FullName.StartsWith($skip, [StringComparison]::OrdinalIgnoreCase) }
    [pscustomobject]@{
        Count = @($files).Count
        Bytes = [int64](@($files) | Measure-Object -Property Length -Sum).Sum
    }
}

# ---- guards ---------------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    Fail "source directory '$SourceDir' does not exist"
}
$source = (Resolve-Path -LiteralPath $SourceDir).ProviderPath
foreach ($needed in 'efi', 'boot\grub\grub.cfg', 'bc250\diag.py', 'bc250diag.apkovl.tar.gz') {
    if (-not (Test-Path -LiteralPath (Join-Path $source $needed))) {
        Fail "'$source' does not look like a usbroot tree: $needed is missing. Run build_usb.py."
    }
}

$disk = Get-Disk -Number $DiskNumber -ErrorAction SilentlyContinue
if (-not $disk) { Fail "there is no disk number $DiskNumber" }
if ($disk.BusType -ne 'USB') { Fail "disk $DiskNumber is a $($disk.BusType) disk, not USB" }
if ($disk.IsBoot) { Fail "disk $DiskNumber is the boot disk" }
if ($disk.IsSystem) { Fail "disk $DiskNumber is the system disk" }
if ($disk.Size -gt $MaxDiskSize) {
    Fail "disk $DiskNumber is $([math]::Round($disk.Size / 1GB, 1)) GB, the limit is $($MaxDiskSize / 1GB) GB"
}
if ($disk.FriendlyName -notlike $ExpectedName) {
    Fail "disk $DiskNumber is '$($disk.FriendlyName)', you asked for '$ExpectedName'"
}

# The source tree must not live on the disk that is about to be erased. A UNC path never does.
if ($source -match '^([A-Za-z]):') {
    $sourcePartition = Get-Partition -DriveLetter $Matches[1] -ErrorAction SilentlyContinue
    if ($sourcePartition -and $sourcePartition.DiskNumber -eq $DiskNumber) {
        Fail "the source tree is on disk $DiskNumber itself"
    }
}

$stats = Get-TreeStats $source
$partitionSize = [math]::Min($disk.Size, $MaxPartitionSize)
$volumes = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
        Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter):" })

Write-Host "Plan"
Write-Host "  disk $DiskNumber  '$($disk.FriendlyName)'  $([math]::Round($disk.Size / 1GB, 1)) GB  $($disk.BusType)  $($disk.PartitionStyle)"
Write-Host "  erase everything on it, including the volume(s): $(if ($volumes) { $volumes -join ', ' } else { '<none with a letter>' })"
Write-Host "  MBR, one active FAT32 partition of $([math]::Round($partitionSize / 1GB, 1)) GB labelled $Label"
Write-Host "  copy $($stats.Count) files, $([math]::Round($stats.Bytes / 1MB)) MiB from $source"

if ($WhatIfPreference) {
    Write-Host "  -WhatIf: the guards passed, nothing was changed."
}
elseif (-not $IUnderstandThisErasesTheDisk) {
    Fail "add -IUnderstandThisErasesTheDisk once you have checked the plan above"
}

if (-not $PSCmdlet.ShouldProcess("disk $DiskNumber ($($disk.FriendlyName))", "ERASE and write the BC-250 diagnostic stick")) {
    return
}

# ---- write ----------------------------------------------------------------------------------

Set-Disk -Number $DiskNumber -IsReadOnly $false
Set-Disk -Number $DiskNumber -IsOffline $false
Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
# Removable media come out of Clear-Disk still initialized (MBR); only a RAW disk needs this.
if ((Get-Disk -Number $DiskNumber).PartitionStyle -eq 'RAW') {
    Initialize-Disk -Number $DiskNumber -PartitionStyle MBR -Confirm:$false
}
elseif ((Get-Disk -Number $DiskNumber).PartitionStyle -ne 'MBR') {
    Fail "disk $DiskNumber is still $((Get-Disk -Number $DiskNumber).PartitionStyle) after Clear-Disk"
}

$partition = if ($disk.Size -le $MaxPartitionSize) {
    New-Partition -DiskNumber $DiskNumber -UseMaximumSize -IsActive -AssignDriveLetter
}
else {
    New-Partition -DiskNumber $DiskNumber -Size $MaxPartitionSize -IsActive -AssignDriveLetter
}
$null = Format-Volume -Partition $partition -FileSystem FAT32 -NewFileSystemLabel $Label -Confirm:$false

# The volume takes a moment to show up under its letter. Always the root, "X:" alone would mean
# whatever the current directory of that drive happens to be.
$target = "$($partition.DriveLetter):\"
for ($i = 0; $i -lt 30 -and -not (Test-Path -LiteralPath $target); $i++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path -LiteralPath $target)) { Fail "the new volume $target did not appear" }

# /NFL /NDL /NJH /NJS /NP: no file, directory or job listing. Only the counts below are printed.
robocopy $source $target /E /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
$code = $LASTEXITCODE
if ($code -gt 7) { Fail "robocopy failed with exit code $code" }

Write-VolumeCache -DriveLetter $partition.DriveLetter

$copied = Get-TreeStats $target
Write-Host "Copied"
Write-Host "  source: $($stats.Count) files, $($stats.Bytes) bytes"
Write-Host "  stick : $($copied.Count) files, $($copied.Bytes) bytes  (robocopy exit code $code)"
if ($copied.Count -ne $stats.Count -or $copied.Bytes -ne $stats.Bytes) {
    Fail "the stick does not match the source tree, do not use it"
}
Write-Host "OK: $target is the BC-250 diagnostic stick. Eject it before unplugging."
