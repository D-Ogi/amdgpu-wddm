# G-OFF (GUI plan, section 9; WU-051): offline dependency resolution with networking unavailable and the original
# download removed, on a computer WITHOUT a BC-250. prepare-offline.ps1 builds a prepared folder; install.ps1 dry runs
# from that folder and from a kept repair set with every download and host probe failing as if the PC were offline
# (AMDGPU_WDDM_TEST_NO_NETWORK=1, common.ps1); a missing or changed firmware file is refused before any change.
# Every child runs under Windows PowerShell 5.1, headless. Only -WorkRoot is written; the footprint is compared.
#   pwsh -File tools\release\test-offline.ps1 -Package <unpacked package folder> [-WorkRoot <dir>] [-FirmwareDir <dir>]
# -FirmwareDir: the release's firmware files (test-dryrun.ps1 passes the folder it downloaded). Without it the first
# prepared folder is made by a real download (network needed for that one step only).
param([Parameter(Mandatory)][string]$Package, [string]$WorkRoot = (Join-Path (Split-Path $Package) 'test-tmp'), [string]$FirmwareDir)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Get-Footprint {
    [ordered]@{
        programdata  = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramData 'amdgpu-wddm') -Recurse -Force -ErrorAction SilentlyContinue).Count
        software     = Test-Path -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm'
        runonce      = [bool](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'amdgpu-wddm-installer' -ErrorAction SilentlyContinue)
        bc250        = Test-Path -LiteralPath 'C:\BC250'
        programfiles = Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'amdgpu-wddm')
    } | ConvertTo-Json -Compress
}
function Invoke-Ps51([string[]]$ScriptArgs, [switch]$Offline, [string]$StateDir) {
    if ($Offline) { $env:AMDGPU_WDDM_TEST_NO_NETWORK = '1' }
    if ($StateDir) { $env:AMDGPU_WDDM_TEST_STATE_DIR = $StateDir }
    try { $r = Invoke-Headless -File $ps51 -Arguments (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File') + $ScriptArgs) -TimeoutSeconds 600 }
    finally { Remove-Item Env:\AMDGPU_WDDM_TEST_NO_NETWORK, Env:\AMDGPU_WDDM_TEST_STATE_DIR -ErrorAction SilentlyContinue }
    "  (pid $($r.pid), exit $($r.code), no window$(if ($Offline) { ', network unavailable' }))" | Write-Host
    return $r
}
# A copy of a folder: hard links (no second copy of 180 MB), except the subfolders named in -RealCopy.
function Copy-Linked([string]$From, [string]$To, [string[]]$RealCopy = @()) {
    foreach ($f in Get-ChildItem -LiteralPath $From -Recurse -File) {
        $rel = $f.FullName.Substring($From.TrimEnd('\').Length + 1)
        $dst = Join-Path $To $rel
        [void][IO.Directory]::CreateDirectory((Split-Path $dst))
        if (@($RealCopy | Where-Object { $rel -like "$_\*" }).Count) { Copy-Item -LiteralPath $f.FullName -Destination $dst }
        else { [void](New-Item -ItemType HardLink -Path $dst -Target $f.FullName) }
    }
}
$m = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
$ver = [string]$m.version
$work = Join-Path $WorkRoot ('offline-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
[void][IO.Directory]::CreateDirectory($work)
$before = Get-Footprint

'[G-OFF] prepare without network and without a firmware folder: refused, nothing left behind'
$dst0 = Join-Path $work 'prepared-none'
$res0 = Join-Path $work 'prepare-none.json'
$r = Invoke-Ps51 @((Join-Path $Package 'installer\prepare-offline.ps1'), '-Destination', $dst0, '-ResultFile', $res0) -Offline
$j = Get-Content -LiteralPath $res0 -Raw | ConvertFrom-Json
Check (($r.code -eq 2) -and ($j.outcome -eq 'refused') -and (@($j.failed_checks) -contains 'firmware.unreachable') -and ($r.text -match 'network unavailable \(test\)')) "refused: $($j.message_id), $(@($j.failed_checks) -join ', ')"
Check (-not (Test-Path -LiteralPath $dst0) -and -not (Test-Path -LiteralPath "$dst0.partial")) 'no destination and no partial folder'

'[G-OFF] prepare a folder on a PC with the package (the original download), then remove the download'
$download = Join-Path $work 'download\amdgpu-wddm-tester'
Copy-Linked $Package $download
$prepared = Join-Path $work 'prepared'
$res1 = Join-Path $work 'prepare.json'
$ev1 = Join-Path $work 'prepare.jsonl'
if ($FirmwareDir) { $r = Invoke-Ps51 @((Join-Path $download 'installer\prepare-offline.ps1'), '-Destination', $prepared, '-FirmwareDir', $FirmwareDir, '-Gui', '-EventsFile', $ev1, '-ResultFile', $res1, '-InvocationId', 'prepare-1') -Offline }
else { $r = Invoke-Ps51 @((Join-Path $download 'installer\prepare-offline.ps1'), '-Destination', $prepared, '-Gui', '-EventsFile', $ev1, '-ResultFile', $res1, '-InvocationId', 'prepare-1') }
$j = Get-Content -LiteralPath $res1 -Raw | ConvertFrom-Json
$ev = @(Get-Content -LiteralPath $ev1 | ForEach-Object { $_ | ConvertFrom-Json })
Check (($r.code -eq 0) -and ($j.outcome -eq 'prepared') -and ($j.invocation -eq 'prepare-1') -and ($j.mode -eq 'prepare-offline') -and -not $j.mutated) "prepared ($($j.message_id)); not an install action"
Check ((($ev | Where-Object { $_.type -eq 'stage' } | ForEach-Object { $_.id }) -join ',') -eq 'prepare-check,firmware,copy,finish') 'stages: prepare-check, firmware, copy, finish'
$set = Get-Content -LiteralPath (Join-Path $prepared 'offline-set.json') -Raw | ConvertFrom-Json
Check (($set.schema -eq 'amdgpu-wddm.offline-set/1') -and ($set.version -eq $ver) -and (@($set.firmware.files).Count -eq @($m.firmware.files).Count)) "offline-set.json: $($set.schema), $($set.version), $(@($set.firmware.files).Count) firmware files"
$okFiles = @($m.files | Where-Object { (Get-FileHash -LiteralPath (Join-Path $prepared ($_.path -replace '/', '\')) -Algorithm SHA256).Hash -eq $_.sha256 }).Count
$okFw = @($m.firmware.files | Where-Object { (Test-Path -LiteralPath (Join-Path $prepared "firmware\$($_.name)")) -and (Get-FileHash -LiteralPath (Join-Path $prepared "firmware\$($_.name)") -Algorithm SHA256).Hash -eq ([string]$_.sha256).ToUpperInvariant() }).Count
Check (($okFiles -eq @($m.files).Count) -and ($okFw -eq @($m.firmware.files).Count) -and -not (Test-Path -LiteralPath "$prepared.partial") -and -not (Test-Path -LiteralPath (Join-Path $prepared 'firmware-download'))) "every package file ($okFiles) and firmware file ($okFw) checked in the folder; no partial or download folder left"
Remove-Item -LiteralPath (Split-Path $download) -Recurse -Force
Check (-not (Test-Path -LiteralPath $download)) 'the original download is removed'

'[G-OFF] install from the prepared folder, network unavailable'
$r = Invoke-Ps51 @((Join-Path $prepared 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') -Offline
Check ($r.code -eq 0) "dry run from the prepared folder: exit $($r.code)"
Check (($r.text -match [regex]::Escape("firmware: the folder of this package, $prepared\firmware (no download)")) -and ($r.text -match '\[ok\s*\]\s+GPU firmware\s+9 files in .+ match the pinned SHA256')) 'the firmware comes from the folder''s own firmware\, every SHA256 checked in the preflight'
Check (($r.text -match [regex]::Escape("would: stage the continuation closure: every file of this package and the firmware folder $prepared\firmware")) -and ($r.text -notmatch 'https://') -and ($r.text -notmatch 'network unavailable')) 'the folder and its firmware are staged into the closure; no address is tried'
Check ($r.text -match 'Dry run complete') 'the install runs to the end'
if ($r.code -ne 0) { $r.text }
$evg = Join-Path $work 'gui-offline.jsonl'
$r = Invoke-Ps51 @((Join-Path $prepared 'installer\install.ps1'), '-Plan', '-DryRunIgnoreBoard', '-Gui', '-EventsFile', $evg, '-ResultFile', (Join-Path $work 'gui-offline.json')) -Offline
$d = @(Get-Content -LiteralPath $evg | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.type -eq 'decision' })
Check (($r.code -eq 0) -and ($d[0].firmware_source -eq 'package-folder')) "setup window plan from the prepared folder: firmware source $($d[0].firmware_source)"

'[G-OFF] a prepared folder with a missing or a changed firmware file is refused before any change'
foreach ($c in @(
        @{ name = 'missing'; edit = { param($dir) Remove-Item -LiteralPath (Join-Path $dir 'firmware\cyan_skillfish2_me.bin') }; want = 'cyan_skillfish2_me\.bin missing' }
        @{ name = 'changed'; edit = { param($dir) [IO.File]::AppendAllText((Join-Path $dir 'firmware\LICENSE.amdgpu'), 'x') }; want = 'LICENSE\.amdgpu has another SHA256' })) {
    $v = Join-Path $work "prepared-$($c.name)"
    Copy-Linked $prepared $v -RealCopy @('firmware')
    & $c.edit $v
    $r = Invoke-Ps51 @((Join-Path $v 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') -Offline
    Check (($r.code -eq 2) -and ($r.text -match "\[fail\]\s+GPU firmware\s+.*$($c.want)") -and ($r.text -match 'Nothing was changed') -and ($r.text -notmatch 'would: ')) "$($c.name) file: refused in the preflight, no change shown"
    if ($r.code -ne 2) { $r.text }
}

'[G-OFF] repair from the kept repair set, network unavailable, no package folder anywhere'
$state = Join-Path $work 'state'
$kept = Join-Path $state "packages\$ver"
Copy-Linked $prepared $kept
Remove-Item -LiteralPath (Join-Path $kept 'offline-set.json')
[IO.File]::WriteAllText((Join-Path $state 'packages\index.json'), ([ordered]@{ schema = 'amdgpu-wddm.repair-sets/1'; active = [ordered]@{ version = $ver; dir = $kept; firmware_complete = $true } } | ConvertTo-Json))
[IO.File]::WriteAllText((Join-Path $state 'state.json'), ([ordered]@{ schema = 1; phase = 'verified'; package_version = $ver; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2026-10-03T00:00:00Z' } | ConvertTo-Json))
Remove-Item -LiteralPath $prepared -Recurse -Force
$r = Invoke-Ps51 @((Join-Path $kept 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard', '-Repair') -Offline -StateDir $state
Check ($r.code -eq 0) "repair dry run from the kept set: exit $($r.code)"
Check (($r.text -match "repairing $([regex]::Escape($ver)) \(-Repair, phase verified\)") -and ($r.text -match 'this run starts from its continuation closure') -and ($r.text -match [regex]::Escape("firmware: the folder of this package, $kept\firmware (no download)"))) 'repair: the kept set is its own closure and carries the firmware'
Check (($r.text -notmatch 'https://') -and ($r.text -match 'would: pnputil /add-driver') -and ($r.text -notmatch 'would: bcdedit')) 'repair: phase 2 again, no download, no test-signing change'
if ($r.code -ne 0) { $r.text }

'[G-OFF] no network and no firmware: the ordinary package is refused before any change'
$r = Invoke-Ps51 @((Join-Path $kept 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') -Offline -StateDir (Join-Path $work 'state-empty')
Check ($r.code -eq 0) "(control) the kept set installs offline from its own firmware: exit $($r.code)"
$plain = Join-Path $work 'plain'
Copy-Linked $kept $plain
Remove-Item -LiteralPath (Join-Path $plain 'firmware') -Recurse -Force
$r = Invoke-Ps51 @((Join-Path $plain 'installer\install.ps1'), '-DryRun', '-DryRunIgnoreBoard') -Offline
Check (($r.code -eq 2) -and ($r.text -match '\[fail\]\s+GPU firmware\s+no download host answers') -and ($r.text -match 'Nothing was changed')) 'a package without firmware and without network: refused (firmware.unreachable)'

'[G-OFF] destination rules of prepare-offline'
$foreign = Join-Path $work 'foreign'
[void][IO.Directory]::CreateDirectory($foreign)
[IO.File]::WriteAllText((Join-Path $foreign 'owner.txt'), 'not ours')
$resF = Join-Path $work 'prepare-foreign.json'
$r = Invoke-Ps51 @((Join-Path $kept 'installer\prepare-offline.ps1'), '-Destination', $foreign, '-FirmwareDir', (Join-Path $kept 'firmware'), '-ResultFile', $resF) -Offline
$j = Get-Content -LiteralPath $resF -Raw | ConvertFrom-Json
Check (($r.code -eq 2) -and (@($j.failed_checks) -contains 'prepare.destination-not-empty') -and ((Get-ChildItem -LiteralPath $foreign).Count -eq 1)) 'a non-empty folder that is not a prepared folder is refused and left as it was'
$again = Join-Path $work 'again'
$r1 = Invoke-Ps51 @((Join-Path $kept 'installer\prepare-offline.ps1'), '-Destination', $again, '-FirmwareDir', (Join-Path $kept 'firmware')) -Offline
$t1 = (Get-Content -LiteralPath (Join-Path $again 'offline-set.json') -Raw | ConvertFrom-Json).prepared_utc
$r2 = Invoke-Ps51 @((Join-Path $kept 'installer\prepare-offline.ps1'), '-Destination', $again, '-FirmwareDir', (Join-Path $kept 'firmware')) -Offline
$t2 = (Get-Content -LiteralPath (Join-Path $again 'offline-set.json') -Raw | ConvertFrom-Json).prepared_utc
Check (($r1.code -eq 0) -and ($r2.code -eq 0) -and ($t2 -ne $t1) -and -not @(Get-ChildItem -LiteralPath $work -Directory -Filter 'again.old-*').Count) 'an existing prepared folder is replaced as a whole'
$r = Invoke-Ps51 @((Join-Path $kept 'installer\prepare-offline.ps1'), '-Destination', (Join-Path $kept 'sub')) -Offline
Check ($r.code -eq 2) 'a destination inside the package is refused'

$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
