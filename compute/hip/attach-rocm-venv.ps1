# attach-rocm-venv.ps1 - puts our two ROCm-facing DLLs into a TheRock Python environment, and takes them out.
#
# A TheRock venv (rocm_sdk_core, rocm_sdk_libraries, torch) loads AMD's HIP runtime and AMD's hipBLASLt.
# On this device both need ours in their place:
#   - amdhip64_7.dll, the AMD ABI face of our HIP runtime (build-amd-abi.ps1), in _rocm_sdk_core\bin. AMD's
#     DLL is kept beside it as amdhip64_7.dll.orig and is never loaded (D015: no second HIP runtime).
#   - libhipblaslt.dll, our hipBLASLt front (build-blaslt-front.ps1), in _rocm_sdk_libraries\bin. AMD's DLL
#     becomes libhipblaslt_amd.dll in the same directory; the front forwards to it by that name, and AMD's
#     kpack device code still resolves from that directory.
# The payload directory (-Payload, by default the directory of this script; the installer puts it in
# <install root>\tools\hip\rocm) holds the two DLLs, this script, rocm-files.ps1 and payload.json:
#   { "schema": 2, "therock": "<version>", "face": { "file", "amd_sha256", "build_sha256" },
#     "blaslt_front": { "file", "amd_sha256", "build_sha256", "amd_exports", "served_names" } }
# amd_sha256 names the AMD DLL each one was built and tested for. A venv whose AMD DLL is another one (another
# TheRock nightly, or a pip upgrade) is refused: the front's export table is AMD's table of that build. Our two
# DLLs are the files of the payload directory as they are now: the release stamps a version resource on each and
# signs it, so their bytes are not the build_sha256 of the build. The package manifest checks them. A payload.json
# of schema 1 (the kits up to kit-int8-1) gives "sha256" for each, and then the file must have it.
#
# Modes:
#   (none)    attach: replace both AMD DLLs; a step already in place is skipped. Writes
#             <venv>\bc250-rocm-attach.json with the four hashes, and an entry for the uninstaller in -ListDir.
#   -Detach   put AMD's DLLs back and remove ours, the record and the entry.
#   -Check    say the state and change nothing.
#   -PlanOnly say what attach or -Detach would do and change nothing.
# -ListDir is the list of attached venvs that the uninstaller reads, by default
# %ProgramData%\amdgpu-wddm\rocm-venvs: one file per venv, so that each user writes only their own entries. The
# uninstaller (tools\release\installer\rocm-venvs.ps1) runs -Detach for each entry, as an administrator.
# Exit codes: 0 attached (or detached with -Detach, or all attached or all detached with -Check); 2 a payload
# file or a venv directory is missing; 3 refused (a DLL that is neither ours nor the AMD DLL that payload.json
# names, a partly attached venv that -Check finds, a path that is not on a local drive letter, a junction or a
# link on the path); 4 a DLL to change is in use; 5 rollback failed and recovery is required.
# Handles remain open through validation, writes and rollback. A caught failure restores the original bytes.
# This is not a crash-atomic filesystem transaction: power loss or forced process termination needs recovery.
# pip and this script: pip removes and installs by the names of the wheel's RECORD, which does not know the
# kept names. A reinstall of the same wheel leaves state 'reinstalled' (attach again); a wheel of another
# TheRock build leaves state 'other' (install the build of payload.json, then attach). A venv that an earlier
# payload of ours attached is state 'earlier' only if its kept AMD bytes are authorized by the trusted payload.
# The venv record identifies the earlier face, but cannot authorize arbitrary AMD restore bytes.
#
# Windows PowerShell 5.1 and PowerShell 7: no ternary, null-coalescing or pipeline chain operators.
param(
    [Parameter(Mandatory = $true)][string]$Venv,
    [string]$Payload = '', [string]$ListDir = '',
    [switch]$Detach, [switch]$Check, [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $Payload) { $Payload = $PSScriptRoot }
if (-not $ListDir) { $ListDir = Join-Path $env:ProgramData 'amdgpu-wddm\rocm-venvs' }
function Say([string]$Text) { Write-Host "ROCM-ATTACH $Text" }
function Stop-With([int]$Code, [string]$Text) { Say $Text; exit $Code }
function Field($Object, [string]$Name) {
    if ($null -eq $Object -or -not $Object.PSObject.Properties[$Name]) { return '' }
    return [string]$Object.$Name
}
function Local-Path([string]$Path) {
    # Reject device paths, UNC, ADS and ambiguous Win32 names before any file access.
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path (Get-Location).ProviderPath $Path }
    if ($Path -notmatch '^[A-Za-z]:\\[^\\]' -or $Path.Substring(2).Contains(':')) { throw "not a folder on a local drive letter: $Path" }
    foreach ($part in $Path.Substring(3).Split('\')) {
        if ($part -ne '.' -and $part -ne '..' -and $part -match '[. ]$') { throw "ambiguous path: $Path" }
    }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}

# The helper ships beside this script; the fallback is only the source-tree layout.
$guard = Join-Path $PSScriptRoot 'rocm-files.ps1'
if (-not (Test-Path -LiteralPath $guard)) { $guard = Join-Path $PSScriptRoot '..\..\tools\release\installer\rocm-files.ps1' }
. $guard
$tx = New-Object Bc250.RocmFiles
$exitCode = 3
try {
    $Venv = Local-Path $Venv; $Payload = Local-Path $Payload; $ListDir = Local-Path $ListDir
    $mutate = -not ($Check -or $PlanOnly)
    $core = Join-Path $Venv 'Lib\site-packages\_rocm_sdk_core\bin'
    $lib = Join-Path $Venv 'Lib\site-packages\_rocm_sdk_libraries\bin'
    [void]$tx.HoldDirectory($core, $false, $false)
    [void]$tx.HoldDirectory($lib, $false, $false)
    [void]$tx.HoldDirectory($Payload, $false, $false)
    $manifestPath = Join-Path $Payload 'payload.json'
    if (-not $tx.Open($manifestPath, $false, $false)) { Stop-With 2 "MISSING: $manifestPath" }
    $manifest = $tx.Text($manifestPath) | ConvertFrom-Json
    if ((Field $manifest 'schema') -notin @('1', '2')) { throw 'unsupported payload schema' }
    $record = Join-Path $Venv 'bc250-rocm-attach.json'
    [void]$tx.Open($record, $mutate, $false)
    $rec = $null
    if ($tx.Exists($record)) { try { $rec = $tx.Text($record) | ConvertFrom-Json } catch { throw 'invalid venv record' } }
    $items = @()
    foreach ($row in @(@('face', $core, 'amdhip64_7.dll', 'amdhip64_7.dll.orig'), @('blaslt_front', $lib, 'libhipblaslt.dll', 'libhipblaslt_amd.dll'))) {
        $m = $manifest.($row[0])
        $name = [string]$m.file
        if ($name -notmatch '^[^\\/:]+\.dll$' -or $name.Contains('..')) { throw "invalid payload file name: $name" }
        $ours = Join-Path $Payload $name
        [void]$tx.Open($ours, $false, $false)
        $live = Join-Path $row[1] $row[2]; $kept = Join-Path $row[1] $row[3]
        [void]$tx.Open($live, $mutate, $false); [void]$tx.Open($kept, $mutate, $false)
        $old = $null
        if ($null -ne $rec -and $rec.PSObject.Properties[$row[0]]) { $old = $rec.($row[0]) }
        $amd = ([string]$m.amd_sha256).ToLowerInvariant()
        if ($amd -notmatch '^[a-f0-9]{64}$') { throw 'invalid AMD hash' }
        $restore = @($amd)
        if ($m.PSObject.Properties['restore_amd_sha256']) {
            foreach ($prior in @($m.restore_amd_sha256)) {
                if ([string]$prior -notmatch '^[a-fA-F0-9]{64}$') { throw 'invalid trusted AMD restore hash' }
                $restore += ([string]$prior).ToLowerInvariant()
            }
        }
        $ourHash = $tx.Hash($ours); $fixed = Field $m 'sha256'
        if ($fixed -and $ourHash -ne $fixed) { throw "$ours is not the file payload.json names ($fixed)" }
        if ($ourHash -eq $amd) { throw "$ours has the bytes of AMD's DLL, so it is not ours" }
        if (-not $Detach -and -not $Check -and -not $ourHash) { Stop-With 2 "MISSING: $ours" }
        $liveHash = $tx.Hash($live); $keptHash = $tx.Hash($kept)
        $state = 'other'
        if ($liveHash -eq $amd -and -not $keptHash) { $state = 'detached' }
        elseif ($liveHash -eq $amd -and $keptHash -eq $amd) { $state = 'reinstalled' }
        elseif ($ourHash -and $liveHash -eq $ourHash -and $keptHash -eq $amd) { $state = 'attached' }
        # An untrusted record only identifies the earlier face. It cannot authorize an AMD hash.
        elseif ((Field $old 'sha256') -eq $liveHash -and $liveHash -and $keptHash -eq $amd) { $state = 'earlier' }
        # History comes from the package manifest, never from the writable venv/list record.
        if ($Detach -and $state -eq 'other') {
            if ($restore -contains $liveHash -and -not $keptHash) { $state = 'detached'; $amd = $liveHash }
            elseif ($restore -contains $liveHash -and $restore -contains $keptHash) { $state = 'reinstalled'; $amd = $liveHash }
            elseif ($restore -contains $keptHash -and $liveHash -and (Field $old 'sha256') -eq $liveHash) { $state = 'earlier'; $amd = $keptHash }
        }
        $items += [pscustomobject]@{ name = $row[0]; live = $live; kept = $kept; ours = $ours; sha = $ourHash; amd = $amd; state = $state }
        Say "$($row[0]): $state"
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $key = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Venv.ToLowerInvariant()))[0..15] | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
    $entry = Join-Path $ListDir "$key.json"
    # Validate existing list ancestors and files for every mode, before touching DLL bytes.
    $listExists = $tx.HoldDirectory($ListDir, $false, $true)
    if ($listExists) { [void]$tx.Open($entry, $mutate, $false) }
    if (@($items | Where-Object { $_.state -eq 'other' }).Count) {
        throw "an AMD DLL is from another TheRock build or the venv has unknown DLLs. This payload is for TheRock $($manifest.therock); pip install --force-reinstall exactly that build, then attach again"
    }
    if ($Check) {
        if ($tx.Exists($entry)) { Say "recorded for the uninstaller: $entry" } else { Say "not recorded for the uninstaller ($entry)" }
        if (@($items | Where-Object { $_.state -ne 'detached' }).Count -eq 0) { Stop-With 0 'CHECK detached' }
        if (@($items | Where-Object { $_.state -ne 'attached' }).Count -eq 0 -and $tx.Exists($entry)) { Stop-With 0 'CHECK attached' }
        if (@($items | Where-Object { $_.state -eq 'earlier' }).Count) { throw 'CHECK: an earlier payload is attached; attach again or -Detach' }
        throw 'CHECK: partly attached or not registered; attach again or -Detach'
    }
    if ($PlanOnly) { Stop-With 0 "PLAN $(if ($Detach) { 'detach' } else { 'attach' }): validated retained files; no bytes changed" }
    if ($Detach) {
        foreach ($i in $items) {
            if ($i.state -eq 'detached') { Say "$($i.name): already detached"; continue }
            if ($i.state -ne 'reinstalled') { $tx.Copy($i.kept, $i.live) }
            if ($tx.Hash($i.live) -ne $i.amd) { throw 'AMD restore verification failed' }
            $tx.Delete($i.kept)
        }
        $tx.Delete($record); $tx.Delete($entry)
        $tx.Commit(); Say 'DETACHED'; $exitCode = 0
    } else {
        # Reserve every output first. CREATE_NEW never follows or overwrites a file planted after validation.
        # A registration failure is an attach failure. Disposal restores bytes and removes reserved outputs.
        if (-not $listExists) { [void]$tx.HoldDirectory($ListDir, $true, $false) }
        foreach ($path in @($record, $entry) + @($items | ForEach-Object { $_.kept })) {
            if (-not $tx.Exists($path)) { [void]$tx.Open($path, $true, $true) }
        }
        $utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        $newRecord = [ordered]@{ utc = $utc; therock = $manifest.therock; payload = $Payload }
        foreach ($i in $items) {
            if ($i.state -eq 'attached') { Say "$($i.name): already attached" }
            else {
                if ($i.state -eq 'detached') { $tx.Copy($i.live, $i.kept) }
                $tx.Copy($i.ours, $i.live)
                Say "$($i.name): attached"
            }
            if ($tx.Hash($i.live) -ne $i.sha -or $tx.Hash($i.kept) -ne $i.amd) { throw 'attach verification failed' }
            $newRecord[$i.name] = [ordered]@{ live = $i.live; kept = $i.kept; sha256 = $i.sha; amd_sha256 = $i.amd }
        }
        $tx.WriteText($record, ($newRecord | ConvertTo-Json -Depth 4))
        $line = [ordered]@{ venv = $Venv; utc = $utc; therock = $manifest.therock; payload = $Payload }
        $tx.WriteText($entry, ($line | ConvertTo-Json))
        $tx.Commit(); Say "recorded for the uninstaller: $entry"; Say "ATTACHED (record $record)"; $exitCode = 0
    }
} catch {
    $why = $_.Exception
    while ($null -ne $why.InnerException) { $why = $why.InnerException }
    $exitCode = 3
    if ($why.Message -match 'IN USE:') { $exitCode = 4 }
    elseif ($why -is [IO.DirectoryNotFoundException]) { $exitCode = 2 }
    Say "REFUSED: $($why.Message)"
} finally {
    try { $tx.Dispose() } catch { Say "RECOVERY REQUIRED: $($_.Exception.Message)"; $exitCode = 5 }
}
exit $exitCode
