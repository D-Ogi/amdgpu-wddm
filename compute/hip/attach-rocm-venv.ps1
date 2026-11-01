# attach-rocm-venv.ps1 - puts our two ROCm-facing DLLs into a TheRock Python environment, and takes them out.
#
# A TheRock venv (rocm_sdk_core, rocm_sdk_libraries, torch) loads AMD's HIP runtime and AMD's hipBLASLt.
# On this device both need ours in their place:
#   - amdhip64_7.dll, the AMD ABI face of our HIP runtime (build-amd-abi.ps1), in _rocm_sdk_core\bin. AMD's
#     DLL is kept beside it as amdhip64_7.dll.orig and is never loaded (D015: no second HIP runtime).
#   - libhipblaslt.dll, our hipBLASLt front (build-blaslt-front.ps1), in _rocm_sdk_libraries\bin. AMD's DLL
#     becomes libhipblaslt_amd.dll in the same directory; the front forwards to it by that name, and AMD's
#     kpack device code still resolves from that directory.
# The payload directory (-Payload, by default the directory of this script; the installer puts the two DLLs,
# payload.json and this script under <install root>\tools\hip\rocm) holds the two DLLs and payload.json:
#   { "therock": "<version>", "face": { "file", "sha256", "amd_sha256" },
#     "blaslt_front": { "file", "sha256", "amd_sha256", "amd_exports" } }
# amd_sha256 names the AMD DLL each one was built and tested for. A venv whose AMD DLL is another one (another
# TheRock nightly, or a pip upgrade) is refused: the front's export table is AMD's table of that build.
#
# Modes:
#   (none)    attach: replace both AMD DLLs; a step already in place is skipped. Writes
#             <venv>\bc250-rocm-attach.json with the four hashes.
#   -Detach   put AMD's DLLs back and remove ours and the record.
#   -Check    say the state and change nothing.
#   -PlanOnly say what attach or -Detach would do and change nothing.
# Exit codes: 0 attached (or detached with -Detach, or all attached or all detached with -Check); 2 a payload
# file or a venv directory is missing; 3 refused (a DLL that is neither ours nor the AMD DLL that payload.json
# names, or a partly attached venv that -Check finds); 4 a DLL to change is in use (close every Python process
# of the venv). A refusal changes no file.
# pip and this script: pip removes and installs by the names of the wheel's RECORD, which does not know the
# kept names. A reinstall of the same wheel leaves state 'reinstalled' (attach again); a wheel of another
# TheRock build leaves state 'other' (install the build of payload.json, then attach).
#
# Windows PowerShell 5.1 and PowerShell 7: no ternary, null-coalescing or pipeline chain operators.
param(
    [Parameter(Mandatory = $true)][string]$Venv,
    [string]$Payload = '',
    [switch]$Detach,
    [switch]$Check,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $Payload) { $Payload = Split-Path -Parent $MyInvocation.MyCommand.Path }

function Sha([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Say([string]$Text) { Write-Host "ROCM-ATTACH $Text" }
function Stop-With([int]$Code, [string]$Text) { Say $Text; exit $Code }

$Venv = [IO.Path]::GetFullPath($Venv)
$site = Join-Path $Venv 'Lib\site-packages'
$coreBin = Join-Path $site '_rocm_sdk_core\bin'
$libBin = Join-Path $site '_rocm_sdk_libraries\bin'
foreach ($d in @($coreBin, $libBin)) {
    if (-not (Test-Path -LiteralPath $d)) { Stop-With 2 "MISSING: $d (is $Venv a TheRock venv?)" }
}
$manifestPath = Join-Path $Payload 'payload.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { Stop-With 2 "MISSING: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

# The two replacements: where ours goes, where AMD's is kept, and the hashes.
$items = @(
    [pscustomobject]@{
        what = 'HIP runtime face'; live = (Join-Path $coreBin 'amdhip64_7.dll'); kept = (Join-Path $coreBin 'amdhip64_7.dll.orig')
        ours = (Join-Path $Payload $manifest.face.file); oursSha = $manifest.face.sha256; amdSha = $manifest.face.amd_sha256
    },
    [pscustomobject]@{
        what = 'hipBLASLt front'; live = (Join-Path $libBin 'libhipblaslt.dll'); kept = (Join-Path $libBin 'libhipblaslt_amd.dll')
        ours = (Join-Path $Payload $manifest.blaslt_front.file); oursSha = $manifest.blaslt_front.sha256
        amdSha = $manifest.blaslt_front.amd_sha256
    }
)
foreach ($i in $items) {
    if (-not $Check -and -not $Detach) {
        if (-not (Test-Path -LiteralPath $i.ours)) { Stop-With 2 "MISSING: $($i.ours)" }
        if ((Sha $i.ours) -ne $i.oursSha) { Stop-With 3 "REFUSED: $($i.ours) is not the file payload.json names ($($i.oursSha))" }
    }
}

# The state of one replacement:
#   attached     ours live, AMD's kept;
#   detached     AMD's live, nothing kept;
#   reinstalled  AMD's live and AMD's kept: pip installed the same wheel again over an attached venv (pip does not
#                know the kept name, so it leaves that file). Attach puts ours back; -Detach removes the kept copy.
#   other        anything else, for example another TheRock build: changed by no mode.
function Get-State($i) {
    $live = Sha $i.live
    $kept = Sha $i.kept
    if ($live -eq $i.oursSha -and $kept -eq $i.amdSha) { return 'attached' }
    if ($live -eq $i.amdSha -and $kept -eq '') { return 'detached' }
    if ($live -eq $i.amdSha -and $kept -eq $i.amdSha) { return 'reinstalled' }
    return "other (live '$live', kept '$kept')"
}

$states = @($items | ForEach-Object { Get-State $_ })
for ($n = 0; $n -lt $items.Count; $n++) { Say "$($items[$n].what): $($states[$n])" }
$record = Join-Path $Venv 'bc250-rocm-attach.json'

# A DLL that a process has loaded can be renamed but not removed or overwritten: a change of the venv that
# stops halfway leaves it half attached. Every file that a mode changes is opened without sharing first.
function Assert-NotInUse([string[]]$Paths) {
    foreach ($p in $Paths) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close() }
        catch { Stop-With 4 "IN USE: $p (close every Python process of $Venv, then run again)" }
    }
}
$changing = @()
for ($n = 0; $n -lt $items.Count; $n++) {
    $want = 'attached'
    if ($Detach) { $want = 'detached' }
    if ($states[$n] -ne $want) { $changing += @($items[$n].live, $items[$n].kept) }
}

$otherHint = "This payload is for TheRock $($manifest.therock): install exactly that rocm_sdk_core and rocm_sdk_libraries (pip install --force-reinstall), then attach again."
if ($Check) {
    if (@($states | Where-Object { $_ -ne 'attached' }).Count -eq 0) { Stop-With 0 'CHECK attached' }
    if (@($states | Where-Object { $_ -ne 'detached' }).Count -eq 0) { Stop-With 0 'CHECK detached' }
    if (@($states | Where-Object { $_ -like 'other*' }).Count -eq 0) {
        Stop-With 3 'CHECK REFUSED: the venv is partly attached (a pip reinstall puts AMD''s DLL back over ours). Attach again, or -Detach.'
    }
    Stop-With 3 "CHECK REFUSED: an AMD DLL of this venv is not the one this payload names. $otherHint"
}

if ($Detach) {
    for ($n = 0; $n -lt $items.Count; $n++) {
        $i = $items[$n]
        if ($states[$n] -like 'other*') { Stop-With 3 "REFUSED: $($i.what) is $($states[$n]); nothing is changed. $otherHint" }
    }
    if ($PlanOnly) { Stop-With 0 'PLAN detach: the AMD DLLs go back in place, ours and the record are removed' }
    Assert-NotInUse $changing
    for ($n = 0; $n -lt $items.Count; $n++) {
        $i = $items[$n]
        if ($states[$n] -eq 'detached') { Say "$($i.what): already detached"; continue }
        if ($states[$n] -eq 'reinstalled') {
            Remove-Item -LiteralPath $i.kept -Force
        } else {
            Remove-Item -LiteralPath $i.live -Force
            Move-Item -LiteralPath $i.kept -Destination $i.live
        }
        if ((Sha $i.live) -ne $i.amdSha -or (Test-Path -LiteralPath $i.kept)) {
            Stop-With 3 "REFUSED: after the detach $($i.live) is not AMD's DLL alone"
        }
        Say "$($i.what): detached, $($i.live) is AMD's $($i.amdSha)"
    }
    if (Test-Path -LiteralPath $record) { Remove-Item -LiteralPath $record -Force }
    Stop-With 0 'DETACHED'
}

for ($n = 0; $n -lt $items.Count; $n++) {
    $i = $items[$n]
    if ($states[$n] -like 'other*') {
        Stop-With 3 "REFUSED: $($i.what) is $($states[$n]); nothing is changed. $($i.live) must be AMD's $($i.amdSha) or ours. $otherHint"
    }
}
if ($PlanOnly) { Stop-With 0 'PLAN attach: each detached AMD DLL is kept under its other name and ours takes its place' }
Assert-NotInUse $changing
for ($n = 0; $n -lt $items.Count; $n++) {
    $i = $items[$n]
    if ($states[$n] -eq 'attached') { Say "$($i.what): already attached"; continue }
    if ($states[$n] -eq 'reinstalled') { Remove-Item -LiteralPath $i.live -Force }
    else { Move-Item -LiteralPath $i.live -Destination $i.kept }
    Copy-Item -LiteralPath $i.ours -Destination $i.live
    if ((Sha $i.live) -ne $i.oursSha -or (Sha $i.kept) -ne $i.amdSha) {
        Stop-With 3 "REFUSED: after the attach $($i.what) does not have the expected hashes"
    }
    Say "$($i.what): attached, $($i.live) is ours $($i.oursSha), AMD's kept as $($i.kept)"
}
$rec = [ordered]@{
    utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'); therock = $manifest.therock; payload = $Payload
    face = [ordered]@{ live = $items[0].live; sha256 = $items[0].oursSha; kept = $items[0].kept; amd_sha256 = $items[0].amdSha }
    blaslt_front = [ordered]@{ live = $items[1].live; sha256 = $items[1].oursSha; kept = $items[1].kept; amd_sha256 = $items[1].amdSha }
}
[IO.File]::WriteAllText($record, ($rec | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
Stop-With 0 "ATTACHED (record $record)"
