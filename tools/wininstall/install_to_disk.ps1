# Puts Windows on a disk attached to this PC so that the disk can then be moved into the BC-250.
#
#   1. default: shrink the existing data partition and create a new NTFS partition in the freed space
#      (nothing else is deleted or formatted); with -WipeDisk: ERASE THE WHOLE DISK and create a fresh
#      GPT layout (EFI system partition, MSR, Windows), the rest stays unallocated
#   2. DISM-apply the image to it (no Windows Setup, so no TPM / Secure Boot / CPU checks and no $OEM$ scripts)
#   3. back up the disk's EFI system partition, then write boot files and a fresh BCD to it (test signing on)
#   4. inject extra drivers, copy the answer file and the first-logon payload
#
# Without -Execute it only reports what it would do. Must run elevated. All output goes to -LogPath too.

param(
    [Parameter(Mandatory)][int]$DiskNumber,
    [Parameter(Mandatory)][string]$ExpectedDiskName,      # guard: FriendlyName must match (wildcards allowed)
    [Parameter(Mandatory)][char]$DataLetter,              # guard: the data partition to shrink
    [Parameter(Mandatory)][string]$ImageFile,             # install.wim / install.esd
    [Parameter(Mandatory)][string]$PayloadDir,            # from make_payload.ps1: unattend.xml + BC250\
    [Parameter(Mandatory)][string]$WorkDir,               # ESP backup and logs
    [string]$DriverDir,
    [string]$EditionName = 'Windows 11 Pro',
    [int]$WindowsSizeGB = 200,
    [switch]$WipeDisk,                                    # erase the whole disk instead of shrinking
    [int64]$ExpectedDiskSizeGB = 0,                       # guard for -WipeDisk: size must match within 1 GB
    [char]$WindowsLetter = 'W',
    [char]$EspLetter = 'S',
    [switch]$Execute
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$log = Join-Path $WorkDir ("install-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
function Say([string]$m) { $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; Write-Host $line; Add-Content -Path $log -Value $line }
function Run([string]$exe, [string[]]$argv) {
    Say "> $exe $($argv -join ' ')"
    & $exe @argv 2>&1 | ForEach-Object { Add-Content -Path $log -Value "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

try {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)   # not the group name: it is localized
    if (-not $admin) { throw 'not elevated' }

    # ---- guards -------------------------------------------------------------------------------------------
    $disk = Get-Disk -Number $DiskNumber
    if ($disk.FriendlyName -notlike $ExpectedDiskName) { throw "disk $DiskNumber is '$($disk.FriendlyName)', expected '$ExpectedDiskName'" }
    if ($disk.PartitionStyle -ne 'GPT') { throw 'disk is not GPT' }
    if ($disk.IsBoot -or $disk.IsSystem) { throw 'refusing to touch the disk this PC boots from' }
    $data = Get-Partition -DriveLetter $DataLetter
    if ($data.DiskNumber -ne $DiskNumber) { throw "${DataLetter}: is not on disk $DiskNumber" }
    $esp = Get-Partition -DiskNumber $DiskNumber | Where-Object GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    if (@($esp).Count -ne 1) { throw 'expected exactly one EFI system partition on the disk' }
    if ($WipeDisk) {
        if ($disk.BusType -ne 'USB') { throw "-WipeDisk is only allowed on a USB-attached disk, this one is $($disk.BusType)" }
        if ([math]::Abs($disk.Size / 1GB - $ExpectedDiskSizeGB) -gt 1) { throw "disk size $([math]::Round($disk.Size/1GB)) GB does not match -ExpectedDiskSizeGB $ExpectedDiskSizeGB" }
        $foreign = Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.DriveLetter -and $_.DriveLetter -ne $DataLetter }
        if ($foreign) { throw "disk has other lettered volumes: $($foreign.DriveLetter -join ', ')" }
    }
    foreach ($l in $WindowsLetter, $EspLetter) { if (Test-Path "${l}:\") { throw "drive letter ${l}: is in use" } }
    foreach ($p in $ImageFile, (Join-Path $PayloadDir 'unattend.xml'), (Join-Path $PayloadDir 'BC250\firstlogon.ps1')) {
        if (-not (Test-Path $p)) { throw "missing: $p" }
    }
    Say "disk $DiskNumber '$($disk.FriendlyName)' $([math]::Round($disk.Size/1GB)) GB, data ${DataLetter}: = partition $($data.PartitionNumber), ESP = partition $($esp.PartitionNumber)"

    $need = [int64]$WindowsSizeGB * 1GB
    if ($WipeDisk) {
        Say "plan: ERASE disk $DiskNumber (serial '$($disk.SerialNumber)'), all of these partitions go:"
        Get-Partition -DiskNumber $DiskNumber | ForEach-Object { Say ("   partition {0} {1,-9} {2,9:N2} GB {3}" -f $_.PartitionNumber, $_.Type, ($_.Size / 1GB), $(if ($_.DriveLetter) { "$($_.DriveLetter):" })) }
        Say "      then: GPT, EFI 300 MB (${EspLetter}:), MSR 16 MB, Windows $WindowsSizeGB GB NTFS (${WindowsLetter}:), rest unallocated"
    } else {
        $sup = Get-PartitionSupportedSize -DriveLetter $DataLetter
        $newDataSize = $data.Size - $need
        if ($newDataSize -lt $sup.SizeMin) { throw "cannot shrink ${DataLetter}: by $WindowsSizeGB GB (minimum size $([math]::Round($sup.SizeMin/1GB,1)) GB)" }
        Say "plan: shrink ${DataLetter}: $([math]::Round($data.Size/1GB,1)) -> $([math]::Round($newDataSize/1GB,1)) GB, new NTFS partition $WindowsSizeGB GB as ${WindowsLetter}:"
    }

    Run dism.exe @('/English', '/Get-ImageInfo', "/ImageFile:$ImageFile")
    $images = Get-WindowsImage -ImagePath $ImageFile
    $image = @($images | Where-Object ImageName -eq $EditionName)
    if ($image.Count -ne 1) { throw "edition '$EditionName' not found exactly once: $($images.ImageName -join ', ')" }
    $index = $image[0].ImageIndex
    Say "image: index $index '$($image[0].ImageName)'"

    if (-not $Execute) { Say 'dry run: nothing changed. Re-run with -Execute.'; return }

    # ---- 1. partition -------------------------------------------------------------------------------------
    if ($WipeDisk) {
        Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
        if ((Get-Disk -Number $DiskNumber).PartitionStyle -eq 'RAW') { Initialize-Disk -Number $DiskNumber -PartitionStyle GPT }
        # Initialize-Disk may create an MSR on its own: drop it so that the order is EFI, MSR, Windows
        Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Remove-Partition -Confirm:$false
        $esp = New-Partition -DiskNumber $DiskNumber -Size 300MB -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
        $esp | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'SYSTEM' -Confirm:$false | Out-Null
        New-Partition -DiskNumber $DiskNumber -Size 16MB -GptType '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' | Out-Null
        $win = New-Partition -DiskNumber $DiskNumber -Size $need -DriveLetter $WindowsLetter
    } else {
        Resize-Partition -DriveLetter $DataLetter -Size $newDataSize
        $win = New-Partition -DiskNumber $DiskNumber -Size ($need - 16MB) -DriveLetter $WindowsLetter
    }
    Format-Volume -DriveLetter $WindowsLetter -FileSystem NTFS -NewFileSystemLabel 'BC250WIN' -Confirm:$false | Out-Null
    Say "created partition $($win.PartitionNumber) as ${WindowsLetter}:"

    # ---- 2. image -----------------------------------------------------------------------------------------
    Run dism.exe @('/English', '/Apply-Image', "/ImageFile:$ImageFile", "/Index:$index", "/ApplyDir:${WindowsLetter}:\")

    # ---- 3. boot ------------------------------------------------------------------------------------------
    $before = (bcdedit /enum firmware) -join "`n"
    $esp | Add-PartitionAccessPath -AccessPath "${EspLetter}:"
    if (-not $WipeDisk) {
        $backup = Join-Path $WorkDir ("esp-backup-{0:yyyyMMdd-HHmmss}" -f (Get-Date))
        robocopy.exe "${EspLetter}:\" $backup /E /R:1 /W:1 /NFL /NDL /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "ESP backup failed (robocopy exit code $LASTEXITCODE)" }
        Say "ESP backed up to $backup"
    }
    $oldBcd = "${EspLetter}:\EFI\Microsoft\Boot\BCD"
    if (Test-Path $oldBcd) {
        attrib -s -h -r $oldBcd
        Move-Item $oldBcd ("$oldBcd.before-bc250-{0:yyyyMMdd-HHmmss}" -f (Get-Date))
    }
    # /addlast: if bcdboot touches this PC's firmware boot menu at all, the new entry must not come first
    Run bcdboot.exe @("${WindowsLetter}:\Windows", '/s', "${EspLetter}:", '/f', 'UEFI', '/addlast')
    $store = "${EspLetter}:\EFI\Microsoft\Boot\BCD"
    Run bcdedit.exe @('/store', $store, '/set', '{default}', 'testsigning', 'on')
    Run bcdedit.exe @('/store', $store, '/set', '{default}', 'bootstatuspolicy', 'IgnoreAllFailures')
    Run bcdedit.exe @('/store', $store, '/set', '{default}', 'description', 'Windows 11 (BC-250 lab, test signing)')
    Run bcdedit.exe @('/store', $store, '/enum', 'all')
    $after = (bcdedit /enum firmware) -join "`n"
    if ($before -ne $after) {
        Say 'WARNING: this PC''s firmware boot entries changed. Before/after are in the log.'
        Add-Content $log "---- firmware before`n$before`n---- firmware after`n$after"
    } else { Say 'this PC''s firmware boot entries are unchanged' }

    # ---- 4. drivers and payload ---------------------------------------------------------------------------
    if ($DriverDir) { Run dism.exe @('/English', "/Image:${WindowsLetter}:\", '/Add-Driver', "/Driver:$DriverDir", '/Recurse') }
    New-Item -ItemType Directory -Force "${WindowsLetter}:\Windows\Panther" | Out-Null
    Copy-Item (Join-Path $PayloadDir 'unattend.xml') "${WindowsLetter}:\Windows\Panther\unattend.xml" -Force
    Copy-Item (Join-Path $PayloadDir 'BC250') "${WindowsLetter}:\" -Recurse -Force
    Say 'answer file and first-logon payload copied'

    Write-VolumeCache -DriveLetter $WindowsLetter
    $esp | Remove-PartitionAccessPath -AccessPath "${EspLetter}:"
    Say 'DONE'
}
catch {
    Say "FAILED: $_"
    exit 1
}
