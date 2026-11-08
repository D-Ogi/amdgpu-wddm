# Put a KMD image in place of the installed one, in the driver store folder the service loads from, so that the rest
# of the install (UMD registration, registry settings) stays as it is. The restart is the caller's (restart.ps1).
# -Restore puts the kept original back. One kit for every KMD lab arm: the image, its hash and the place of the kept
# original are parameters, never edits of this file.
# Refuses on a hash, signer or certificate mismatch, or on a running GPU client. Keeps the original once at -Keep
# (the first swap writes it, later swaps leave it as it is).
#   target.py push <sys> --to C:\BC250\tmp\<work>\kmd ; target.py ps swap-sys.ps1 -New <remote sys> -Sha <sha256>
#
# A keep file that holds an image older than the installed one is the trap of 2026-10-14: an arm restored
# 71C6B09C from a keep file another arm had written the day before, over the installed rc4. So a swap refuses
# when an existing keep file does not hold the image that is in the driver store right now, and says both
# hashes. -AcceptKeep is the way to chain a second swap on purpose, and -Sha on a -Restore makes the restore
# state what it expects to put back. Use a keep path of your own per arm. The check is for a swap only: a -Restore
# after this arm's own swap needs no -AcceptKeep (until 2026-10-14 ~15:00Z it refused, a03 of BD-170).
#
# A swap needs -Catalog, the state catalog that the new image compiles (the kit pins it at its freeze with
# tools/state/kmd_swap_catalog.py pin). The check below (a copy of the block of catalog-precheck.ps1, because
# target.py ps sends one file) compares it with the installed one before any change, because a KMD of another catalog fails DriverEntry with 0xC0000365 (A1 s3 of BD-223,
# 2026-11-07). A restore needs no -Catalog: it puts back the image of the installed package.
# Host control: test-swap-sys.ps1 next to this file (catalog refusal), and the kit host tests for the rest.
param(
    [switch]$Restore,
    [string]$New,
    [string]$Sha,
    [string]$Catalog,
    [string]$Keep = 'C:\BC250\tmp\kmd-orig\bc250kmd.sys',
    [string]$Signer = 'AFCA4B1A82081E848DD6E53A6550D29153FD2EB6',
    [switch]$AcceptKeep,
    # Tests only: the service key that the catalog check reads.
    [string]$ServiceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
)
$ErrorActionPreference = 'Stop'
# >>> state catalog check (kmd-swap)
function Test-StateCatalog([string]$Expected, [int]$Schema, [string]$ServiceKey) {
    # Returns 0 (match), 5 (refused) or 2 (usage or read error), and writes its lines to the output.
    function Value([string]$Key, [string]$Name) {
        if (-not (Test-Path -LiteralPath $Key)) { return $null }
        $p = Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction SilentlyContinue
        if ($null -eq $p) { return $null }
        return $p.$Name
    }
    $want = "$Expected".Trim().ToLowerInvariant()
    if ($want -notmatch '^[0-9a-f]{64}$') { Write-Output "ERROR: the expected catalog is not 64 hex digits: '$Expected'"; return 2 }
    try {
        $defaults = Join-Path $ServiceKey 'Parameters\DefaultsV2'
        $dSchema = Value $defaults 'Schema'
        $dCatalog = Value $defaults 'CatalogId'
        $state = Join-Path $ServiceKey 'SharedState\StateV2'
        $active = Value $state 'Active'
        $gCatalog = $null
        if ($active) { $gCatalog = Value (Join-Path $state "Generations\$active") 'CatalogId' }
    } catch { Write-Output "ERROR: registry read failed: $($_.Exception.Message)"; return 2 }
    Write-Output "expected   $want schema $Schema"
    Write-Output "DefaultsV2 $dCatalog schema $dSchema"
    if ($active) { Write-Output "StateV2    $gCatalog generation $active" } else { Write-Output 'StateV2    no active generation' }
    $why = @()
    if ($null -eq $dCatalog) { $why += 'DefaultsV2 has no CatalogId (no installed shared-state package)' }
    elseif ("$dCatalog".ToLowerInvariant() -ne $want) { $why += "DefaultsV2 CatalogId is $dCatalog" }
    if ($null -eq $dSchema -or [int]$dSchema -ne $Schema) { $why += "DefaultsV2 Schema is $dSchema, not $Schema" }
    if ($active) {
        if ($null -eq $gCatalog) { $why += "the active generation $active has no CatalogId" }
        elseif ("$gCatalog".ToLowerInvariant() -ne $want) {
            $why += "the active generation $active has the catalog $gCatalog (a package install whose first start did not run, or a damaged store)"
        }
    }
    if ($why.Count) {
        foreach ($w in $why) { Write-Output "REFUSED: $w" }
        Write-Output 'REFUSED: a KMD-only swap of this candidate cannot start here. Build it on the installed catalog, or install a package of it.'
        return 5
    }
    Write-Output "CATALOG MATCH $want"
    return 0
}
# <<< state catalog check (kmd-swap)
if (-not $Restore) {
    if (-not $Catalog) { "REFUSED: -Catalog is required for a swap (the state catalog that the new image compiles)"; exit 2 }
    $lines = @(Test-StateCatalog -Expected $Catalog -Schema 2 -ServiceKey $ServiceKey)
    $code = [int]$lines[-1]
    if ($lines.Count -gt 1) { $lines[0..($lines.Count - 2)] }
    if ($code -ne 0) { "REFUSED: catalog check exit $code; nothing changed"; exit 5 }
}
function Sha([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function DisableForSwap {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
    if ($devices.Count -ne 1) { throw 'REFUSED: expected exactly one GPU device' }
    $device = $devices[0]
    if ($device.Problem -ne 'CM_PROB_DISABLED') {
        $answer = & pnputil.exe /disable-device "$($device.InstanceId)" 2>&1
        $code = $LASTEXITCODE
        "pnputil disable exit $code :: $(($answer | Select-Object -Last 1) -join ' ')"
        if ($code -ne 0) { throw "REFUSED: disable native exit $code; image not copied" }
        Start-Sleep 5
    }
    $after = Get-PnpDevice -InstanceId $device.InstanceId
    if ($after.Problem -ne 'CM_PROB_DISABLED') { throw 'REFUSED: GPU still enabled; image not copied' }
    'device disable verified'
}
"utc $([DateTime]::UtcNow.ToString('o')) boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
if (-not $Restore -and (-not $New -or -not $Sha)) { "REFUSED: -New and -Sha are required (or -Restore)"; exit 2 }

$ip = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -Name ImagePath).ImagePath
$path = $ip -replace '^\\SystemRoot\\', "$env:SystemRoot\" -replace '^\\\?\?\\', '' -replace '^System32\\', "$env:SystemRoot\System32\"
"ImagePath $ip"
"resolved $path"
if (-not (Test-Path -LiteralPath $path)) { "REFUSED: image not found"; exit 3 }
$cur = Sha $path
"current $cur"

# The control app's logon notice (--start-notice, shown after an unclean end) is our own window, not a workload: close
# it first, so that it does not refuse every swap after a 0x116 (2026-10-29 08:00Z and 08:05Z).
Get-CimInstance Win32_Process -Filter "Name='amdgpu_wddm_control.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match '--start-notice' } |
    ForEach-Object { "closing the control app logon notice pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500
$names = 'hipbench|llama|ollama|amdgpu_wddm_|vkcube|vulkaninfo|q2rtx|quake2rtx|ROTTR|witcher3|DXRPathTracer|vkheaps|3DMark'
$busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match "^($names)" })
# The gate protects a running driver. A device with a problem code (r7c on 2026-10-14: CM_PROB_FAILED_DRIVER_ENTRY,
# Basic Display on screen) has no image loaded, so a client cannot hold it: name the clients and go on.
$dev = @(Get-CimInstance Win32_PnPEntity -Filter "Service='bc250kmd'" -ErrorAction SilentlyContinue)
$down = $dev.Count -gt 0 -and @($dev | Where-Object { $_.ConfigManagerErrorCode -eq 0 }).Count -eq 0
if ($busy.Count -and $down) {
    "clients $(($busy | ForEach-Object { $_.ProcessName }) -join ', ') left running: the device has problem code $($dev[0].ConfigManagerErrorCode), no KMD is loaded"
} elseif ($busy.Count) { "REFUSED: GPU client running: $(($busy | ForEach-Object { $_.ProcessName }) -join ', ')"; exit 4 }

# Before any write: on a swap, an existing keep file that is not the image in the driver store is somebody else's,
# or this arm's second swap. Either way the restore it promises is not the installed driver, so say so and stop.
# A restore is the other way round: after this arm's swap the keep file differs from the driver store by design, so
# the check does not apply to it. -Sha says which image the restore must put back.
$keepSha = ''
if (Test-Path -LiteralPath $Keep) {
    $keepSha = Sha $Keep
    "keep $keepSha at $Keep"
    if (-not $Restore -and $keepSha -ne $cur -and -not $AcceptKeep) {
        "REFUSED: the keep file does not hold the installed image"
        "  keep       $keepSha"
        "  driverstore $cur"
        "  a restore from this keep file would put the keep image on the lab. Use a keep path of this arm, or"
        "  pass -AcceptKeep when the difference is this arm's own earlier swap."
        exit 7
    }
}

if ($Restore) {
    if (-not $keepSha) { "REFUSED: no kept original at $Keep"; exit 3 }
    if ($Sha -and $keepSha -ne $Sha.ToUpperInvariant()) {
        "REFUSED: the kept original is $keepSha, not the expected $($Sha.ToUpperInvariant())"; exit 3
    }
    if ($cur -eq $keepSha) { "already this image"; "image now $cur"; exit 0 }
    $src = $Keep
} else {
    $newSha = $Sha.ToUpperInvariant()
    if ((Sha $New) -ne $newSha) { "REFUSED: new image hash $(Sha $New)"; exit 3 }
    $sig = Get-AuthenticodeSignature -LiteralPath $New
    "new signature $($sig.Status) signer $($sig.SignerCertificate.Thumbprint)"
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $Signer) { "REFUSED: signature"; exit 6 }
    foreach ($store in 'Root', 'TrustedPublisher') {
        if (-not @(Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $Signer }).Count) {
            "REFUSED: signer certificate not in LocalMachine\$store"; exit 6
        }
    }
    if ($cur -eq $newSha) { "already this image"; exit 0 }
    if (-not (Test-Path -LiteralPath $Keep)) {
        New-Item -ItemType Directory -Force (Split-Path $Keep) | Out-Null
        Copy-Item -LiteralPath $path -Destination $Keep
        "kept original $(Sha $Keep) at $Keep"
    } else { "kept original already there $(Sha $Keep)" }
    $src = $New
}
"testsigning: $((& bcdedit.exe /enum '{current}' | Select-String -Pattern 'testsigning') -join ' ')"
# A loaded image cannot be overwritten: stop the device first. restart.ps1 enables it again before its restart.
DisableForSwap
& takeown.exe /f $path /a | Out-Null
& icacls.exe $path /grant '*S-1-5-32-544:F' | Out-Null
Copy-Item -LiteralPath $src -Destination $path -Force
"image now $(Sha $path)"
# The next start must not count as a third unconfirmed one. The count that the KMD reads is in StateV2 since
# tester.31 (restart.ps1 says why); the Parameters copy is legacy and is cleared for older installs only.
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -Name UnconfirmedStarts -Value 0 -Type DWord
$sv = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\SharedState\StateV2'
$active = (Get-ItemProperty -LiteralPath $sv -Name Active -ErrorAction SilentlyContinue).Active
if ($active) {
    Set-ItemProperty -LiteralPath (Join-Path $sv "Generations\$active\Data") -Name UnconfirmedStarts -Value 0 -Type DWord
    "UnconfirmedStarts 0 (StateV2 $active and legacy)"
} else { "UnconfirmedStarts 0 (legacy; no StateV2)" }
