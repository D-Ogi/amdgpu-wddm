# G-EVT and G-STAGE (GUI plan, section 9): the engine's events and terminal result as the setup window reads them, on a
# computer WITHOUT a BC-250. Every run is a plan or a dry run of install.ps1 under Windows PowerShell 5.1, headless
# (headless.ps1: no window, stdin closed, a time bound), with -Gui, -InvocationId, -EventsFile and -ResultFile; the
# state of an earlier install comes from a test folder (AMDGPU_WDDM_TEST_STATE_DIR). Nothing on this computer changes:
# the footprint is compared before and after. Never run the real install here.
#   pwsh -File tools\release\test-engine-events.ps1 -Package <unpacked package folder> [-WorkRoot <dir>] [-FirmwareDir <dir>]
# -FirmwareDir: a folder with the release's firmware files (test-dryrun.ps1 passes the one it downloaded); without it
# the -FirmwareDir case is skipped.
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
        task         = [bool](Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue)
        bc250        = Test-Path -LiteralPath 'C:\BC250'
        programfiles = Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'amdgpu-wddm')
        bootopts     = [string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions
    } | ConvertTo-Json -Compress
}
$m = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
$pkgVersion = [string]$m.version
$tsActive = [string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions -match 'TESTSIGNING'
$bootId = $null
$v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters' -Name BootId -ErrorAction SilentlyContinue).BootId
if ($null -ne $v) { $bootId = [int64][BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$v), 0) }
$work = Join-Path $WorkRoot ('events-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
[void][IO.Directory]::CreateDirectory($work)
$n = 0

# One engine run: a fresh folder for its events and result, its own invocation id, optional test state and cancel.
function Invoke-Engine {
    param([string]$Name, [string[]]$Arguments = @(), $State, [switch]$Cancel, [string]$PackageDir = $Package, [string]$Script = 'install.ps1', [switch]$NoInvocation, [switch]$StaleResult, [hashtable]$Env = @{})
    $script:n++
    $dir = Join-Path $work ('{0:D2}-{1}' -f $script:n, ($Name -replace '[^a-z0-9]+', '-'))
    [void][IO.Directory]::CreateDirectory($dir)
    $events = Join-Path $dir 'events.jsonl'
    $result = Join-Path $dir 'result.json'
    $inv = [guid]::NewGuid().ToString()
    if ($Cancel) { [IO.File]::WriteAllText("$events.cancel", '') }
    if ($StaleResult) { [IO.File]::WriteAllText($result, (@{ schema = 'amdgpu-wddm.engine-result/1'; invocation = 'stale'; outcome = 'verified'; exit_code = 0 } | ConvertTo-Json)) }
    $a = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PackageDir "installer\$Script"), '-Gui', '-EventsFile', $events, '-ResultFile', $result)
    if (-not $NoInvocation) { $a += @('-InvocationId', $inv) } else { $inv = $null }
    $a += $Arguments
    $stateDir = $null
    if ($State) {
        $stateDir = Join-Path $dir 'state'
        [void][IO.Directory]::CreateDirectory($stateDir)
        [IO.File]::WriteAllText((Join-Path $stateDir 'state.json'), ($State | ConvertTo-Json -Depth 4))
        $env:AMDGPU_WDDM_TEST_STATE_DIR = $stateDir
    }
    foreach ($k in $Env.Keys) { Set-Item -Path "Env:\$k" -Value $Env[$k] }
    try { $r = Invoke-Headless -File $ps51 -Arguments $a -TimeoutSeconds 240 } finally {
        if ($State) { Remove-Item Env:\AMDGPU_WDDM_TEST_STATE_DIR }
        foreach ($k in $Env.Keys) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
    }
    $ev = @()
    if (Test-Path -LiteralPath $events) { $ev = @(Get-Content -LiteralPath $events | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json }) }
    $res = $null
    if (Test-Path -LiteralPath $result) { $res = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json }
    "  [$Name] exit $($r.code), $($ev.Count) events, outcome $(if ($res) { $res.outcome } else { 'none' }) (pid $($r.pid), no window)" | Write-Host
    return [pscustomobject]@{ code = $r.code; text = $r.text; events = $ev; result = $res; invocation = $inv; dir = $dir; state_dir = $stateDir }
}
# The stream rules every run must keep: one schema, one invocation, consecutive seq from 1, the result event last and
# equal to the result file, which belongs to this invocation and names the process exit code.
function Test-Stream($R, [string]$Name) {
    $ok = $R.events.Count -gt 0 -and $R.result
    $inv = $(if ($R.invocation) { $R.invocation } elseif ($R.result) { $R.result.invocation } else { $null })
    for ($i = 0; $ok -and $i -lt $R.events.Count; $i++) {
        $e = $R.events[$i]
        if ($e.schema -ne 'amdgpu-wddm.engine-event/1' -or $e.invocation -ne $inv -or [int]$e.seq -ne $i + 1 -or -not $e.type -or -not $e.utc) { $ok = $false }
    }
    $last = $(if ($R.events.Count) { $R.events[-1] } else { $null })
    $ok = $ok -and $last.type -eq 'result' -and $last.outcome -eq $R.result.outcome -and [int]$last.exit_code -eq $R.code
    $ok = $ok -and $R.result.schema -eq 'amdgpu-wddm.engine-result/1' -and $R.result.invocation -eq $inv -and [int]$R.result.exit_code -eq $R.code -and $R.result.engine.contract -eq 'amdgpu-wddm.engine/1'
    $ok = $ok -and $R.events[0].type -eq 'start' -and @($R.events | Where-Object { $_.type -eq 'result' }).Count -eq 1
    Check $ok "${Name}: stream (schema, invocation $inv, seq 1..$($R.events.Count), start first, one result last = result file, exit code $($R.code))"
    if (-not $ok) { $R.text }
}
function Get-Events($R, [string]$Type) { return @($R.events | Where-Object { $_.type -eq $Type }) }
function Get-Seq($R, [string]$Type) { $e = @(Get-Events $R $Type); if ($e.Count) { return [int]$e[0].seq } else { return [int]::MaxValue } }
$before = Get-Footprint
$fresh = $null

'[G-EVT] plan on this PC: refused by the preflight, ids not text, nothing changed'
$r = Invoke-Engine 'plan refused' @('-Plan')
Test-Stream $r 'plan refused'
Check (($r.code -eq 2) -and ($r.result.outcome -eq 'refused') -and ($r.result.message_id -eq 'result.preflight-refused') -and (@($r.result.failed_checks) -contains 'gpu.missing')) "refused with result.preflight-refused, failed check ids: $(@($r.result.failed_checks) -join ', ')"
$checks = @(Get-Events $r 'check')
Check (($checks.Count -ge 8) -and -not @($checks | Where-Object { -not $_.id -or $_.id -notmatch '^[a-z0-9-]+\.[a-z0-9-]+$' -or $_.result -notin 'ok', 'warn', 'fail' }).Count) "$($checks.Count) check events, each with a stable id and ok/warn/fail"
Check ($r.result.mutated -eq $false -and $r.result.nothing_changed -eq $true -and $r.result.mode -eq 'plan' -and $r.result.dry_run) 'plan: mode plan, nothing changed'
Check (-not (Get-Events $r 'step').Count -and -not (Get-Events $r 'decision').Count) 'refused before the decision: no decision, no step'

'[G-EVT] plan of a fresh install: decision, consents, restarts, settings-impact plan'
$r = Invoke-Engine 'plan fresh' @('-Plan', '-DryRunIgnoreBoard')
Test-Stream $r 'plan fresh'
$d = @(Get-Events $r 'decision')
Check (($r.code -eq 0) -and ($r.result.outcome -eq 'planned') -and ($d.Count -eq 1) -and ($d[0].action -eq 'install')) 'planned: one decision, action install'
Check ((@($d[0].consents) -contains 'test-signing') -eq (-not $tsActive)) "consents from the engine: $(@($d[0].consents) -join ', ') (test signing active on this PC: $tsActive)"
Check ([int]$d[0].restarts -eq $(if ($tsActive) { 1 } else { 2 })) "restarts: $($d[0].restarts)"
Check (($d[0].firmware_source -eq 'download') -and (@($d[0].notes) -contains 'RELEASE-NOTES.md') -and $d[0].compatibility.ok) 'firmware from the download, the release notes named, the package''s compatibility record verifies'
$sp = @(Get-Events $r 'settings-plan')
Check (($sp.Count -eq 1) -and (@($sp[0].rows).Count -ge 20) -and -not @($sp[0].rows | Where-Object { $_.decision -in 'installer', 'restored' -or -not $_.group -or -not $_.name }).Count -and ($null -ne $sp[0].summary.added)) "settings-plan: $(@($sp[0].rows).Count) rows (group, name, decision), summary $($sp[0].summary | ConvertTo-Json -Compress)"
Check ((Get-Seq $r 'decision') -lt (Get-Seq $r 'settings-plan')) 'the decision comes before the settings plan'
Check (-not (Get-Events $r 'step').Count -and -not (Get-Events $r 'install-action').Count) 'a plan has no step and records no install action'
$fresh = $r

'[G-EVT] setup-window run without the consent: stops before any change'
if (-not $tsActive) {
    $r = Invoke-Engine 'needs consent' @('-DryRun', '-DryRunIgnoreBoard')
    Test-Stream $r 'needs consent'
    Check (($r.code -eq 4) -and ($r.result.outcome -eq 'needs-consent') -and ((@($r.result.consents_needed) -join ',') -eq 'test-signing')) "needs-consent, consents_needed: $(@($r.result.consents_needed) -join ', ')"
    Check (-not (Get-Events $r 'step').Count -and ($r.text -notmatch 'would ask')) 'no step, no question asked'
} else { '  (skipped: test signing is active on this PC, the install needs no test-signing consent)' }

'[G-EVT] full dry run with the consent: stages in order, settings plan before the first step, cancel window, restart handed over'
$r = Invoke-Engine 'dry run' @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning')
Test-Stream $r 'dry run'
Check (($r.code -eq 0) -and ($r.result.outcome -eq 'completed') -and ($r.result.message_id -eq 'result.dry-run-complete') -and $r.result.dry_run -and -not $r.result.mutated) 'completed (dry run), nothing changed'
$stages = @(Get-Events $r 'stage' | ForEach-Object { $_.id })
$want = @('preflight') + $(if ($tsActive) { @() } else { @('test-signing') }) + @('install', 'firmware', 'files', 'driver', 'settings', 'finish')
Check ((($stages -join ',') -eq ($want -join ','))) "stages: $($stages -join ', ')"
Check ((Get-Seq $r 'settings-plan') -lt (Get-Seq $r 'step')) 'the settings-impact plan comes before the first step'
$cancel = @(Get-Events $r 'cancel')
$filesSeq = [int](@(Get-Events $r 'stage' | Where-Object { $_.id -eq 'files' })[0].seq)
# Phase 1 withdraws Cancel before test signing; the dry run then shows phase 2 as the new run after the restart, which
# offers it again until the driver install (interfaces-setup.md section 10).
$cancelWant = $(if ($tsActive) { 'before-changes=True,driver-install=False' } else { 'before-changes=True,test-signing=False,phase-2=True,driver-install=False' })
$cancelHave = @($cancel | ForEach-Object { "$($_.where)=$([bool]$_.available)" }) -join ','
Check (($cancelHave -eq $cancelWant) -and ([int]$cancel[-1].seq -lt $filesSeq)) "cancel offered before the changes, withdrawn before test signing and before the files and the driver install: $cancelHave"
$steps = @(Get-Events $r 'step')
Check (($steps.Count -ge 15) -and -not @($steps | Where-Object { -not $_.dry_run }).Count) "$($steps.Count) step events, each marked dry run"
$rr = @(Get-Events $r 'restart-required')
Check (($rr.Count -eq $(if ($tsActive) { 1 } else { 2 })) -and ($rr[-1].reason_id -eq 'restart.complete') -and ($r.result.restart.required) -and ($r.result.restart.reason_id -eq 'restart.complete')) "restart handed to the window: $(@($rr | ForEach-Object { $_.reason_id }) -join ', ')"
Check (($r.result.restart.continuation.kind -eq 'verify') -and ($r.result.restart.continuation.command -match 'powershell\.exe.* -File .*installer\\install\.ps1"? -HoldWindow -Verify$')) "continuation after the last restart: $($r.result.restart.continuation.command)"
if (-not $tsActive) {
    $first = $rr[0]
    Check (($first.reason_id -eq 'restart.test-signing') -and ($first.continuation.kind -eq 'continue') -and ($first.continuation.command -match [regex]::Escape("amdgpu-wddm\installer\packages\$pkgVersion\installer\install.ps1"))) "continuation after the test-signing restart runs from the closure: $($first.continuation.command)"
}
Check ($r.text -notmatch 'Restart now\?|would ask: Restart') 'the engine never asks for the restart'
Check (-not (Get-Events $r 'install-action').Count) 'a dry run records no install action'

'[G-EVT] invocation binding: a stale result is removed, a generated id is used throughout'
$r = Invoke-Engine 'stale result' @('-Plan', '-DryRunIgnoreBoard') -StaleResult
Check ($r.result.invocation -eq $r.invocation -and $r.result.outcome -eq 'planned') 'the result file is this invocation''s, not the stale one'
$r = Invoke-Engine 'generated id' @('-Plan', '-DryRunIgnoreBoard') -NoInvocation
Test-Stream $r 'generated id'
Check ($r.result.invocation -match '^[0-9a-f]{8}-[0-9a-f]{4}-') "an id is generated when the window gives none: $($r.result.invocation)"

'[G-EVT] cancel at the safe point'
$r = Invoke-Engine 'cancel' @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning') -Cancel
Test-Stream $r 'cancel'
Check (($r.code -eq 8) -and ($r.result.outcome -eq 'cancelled') -and ($r.result.message_id -eq 'result.cancelled') -and -not $r.result.mutated -and ($r.result.stop.by -eq 'cancel')) "cancelled before any change: exit 8, result.cancelled, stop by cancel at $($r.result.stop.where)"
Check (-not (Get-Events $r 'step').Count) 'no step after the cancel'

'[G-EVT] repair and upgrade decisions over an installed release'
$st = [ordered]@{ schema = 1; phase = 'verified'; package_version = $pkgVersion; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2026-10-03T00:00:00Z' }
$r = Invoke-Engine 'repair plan' @('-Plan', '-DryRunIgnoreBoard', '-Repair') -State $st
$d = @(Get-Events $r 'decision')
Check (($r.result.outcome -eq 'planned') -and ($d[0].action -eq 'repair') -and -not @($d[0].consents).Count -and ([int]$d[0].restarts -eq 1) -and (Get-Events $r 'settings-plan').Count) 'repair: no consent, one restart, a settings plan'
$r = Invoke-Engine 'already plan' @('-Plan', '-DryRunIgnoreBoard') -State $st
$d = @(Get-Events $r 'decision')
Check (($d[0].action -eq 'already') -and ([int]$d[0].restarts -eq 0) -and -not (Get-Events $r 'settings-plan').Count) 'the same verified version: already, nothing to plan'
$st2 = [ordered]@{ schema = 1; phase = 'verified'; package_version = '0.7.198.100-tester.10'; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2026-10-03T00:00:00Z' }
$r = Invoke-Engine 'upgrade dry run' @('-DryRun', '-DryRunIgnoreBoard') -State $st2
Test-Stream $r 'upgrade dry run'
Check (($r.result.outcome -eq 'completed') -and (@(Get-Events $r 'decision')[0].action -eq 'upgrade') -and ($r.result.phase_before -eq 'verified') -and ($r.text -notmatch 'would: bcdedit')) 'upgrade: no consent needed, phase 2 only'
if ($FirmwareDir) {
    $r = Invoke-Engine 'firmware folder plan' @('-Plan', '-DryRunIgnoreBoard', '-FirmwareDir', $FirmwareDir) -State $st2
    Check ((@(Get-Events $r 'decision')[0].firmware_source -eq 'folder') -and (@(Get-Events $r 'check' | Where-Object { $_.id -eq 'firmware.folder-ok' }).Count -eq 1)) 'a firmware folder: checked (firmware.folder-ok), source folder'
} else { '  (skipped: no -FirmwareDir given)' }

'[G-EVT] a failing step still ends with a terminal result'
$broken = Join-Path $work 'pkg-broken'
[void][IO.Directory]::CreateDirectory($broken)
foreach ($f in @($m.files)) {
    $dst = Join-Path $broken ($f.path -replace '/', '\')
    [void][IO.Directory]::CreateDirectory((Split-Path $dst))
    [void](New-Item -ItemType HardLink -Path $dst -Target (Join-Path $Package ($f.path -replace '/', '\')))
}
$tbl = Join-Path $broken 'installer\registry-defaults.json'
Remove-Item -LiteralPath $tbl
[IO.File]::WriteAllText($tbl, '{ "schema": 1, "defaults": ')
$mb = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
foreach ($f in $mb.files) { if ($f.path -eq 'installer/registry-defaults.json') { $f.sha256 = (Get-FileHash -LiteralPath $tbl -Algorithm SHA256).Hash; $f.size = (Get-Item -LiteralPath $tbl).Length } }
[IO.File]::WriteAllText((Join-Path $broken 'manifest.json'), ($mb | ConvertTo-Json -Depth 8))
$r = Invoke-Engine 'failed step' @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning') -PackageDir $broken
Test-Stream $r 'failed step'
Check (($r.code -eq 6) -and ($r.result.outcome -eq 'failed') -and ($r.result.message_id -eq 'result.step-failed') -and $r.result.detail) "failed: exit 6, result.step-failed, detail for the log only ($($r.result.detail))"

'[G-STAGE] restart boundaries advance only with positive evidence of a new boot (R1): resume and explicit Verify'
if ($null -ne $bootId) {
    $resume = @('-DryRun', '-DryRunIgnoreBoard'); $verify = @('-DryRun', '-Verify')
    $unreadable = @{ AMDGPU_WDDM_TEST_BOOT_ID = 'unreadable' }
    foreach ($c in @(
            @{ name = 'resume, same boot'; phase = 'driver-pending-restart'; saved = $bootId; args = $resume; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'resume, no saved boot'; phase = 'driver-pending-restart'; saved = $null; args = $resume; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'resume, current BootId unreadable'; phase = 'driver-pending-restart'; saved = $bootId + 1; args = $resume; env = $unreadable; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'resume, new boot (positive control)'; phase = 'driver-pending-restart'; saved = $bootId + 1; args = $resume; code = 0; msg = 'result.dry-run-complete' }
            @{ name = 'resume of testsigning-pending, same boot'; phase = 'testsigning-pending'; saved = $bootId; args = $resume; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'resume of testsigning-pending, no saved boot'; phase = 'testsigning-pending'; saved = $null; args = $resume; code = 7; msg = 'result.restart-still-pending' }
            # Past the boundary, this PC (test signing not active) stops for the test-signing restart itself.
            @{ name = 'resume of testsigning-pending, new boot (positive control)'; phase = 'testsigning-pending'; saved = $bootId + 1; args = $resume; code = 5; msg = 'result.testsigning-not-active' }
            @{ name = 'Verify of testsigning-pending, same boot'; phase = 'testsigning-pending'; saved = $bootId; args = $verify; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'Verify of testsigning-pending, no saved boot'; phase = 'testsigning-pending'; saved = $null; args = $verify; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'Verify of testsigning-pending, new boot: unfinished'; phase = 'testsigning-pending'; saved = $bootId + 1; args = $verify; code = 2; msg = 'result.install-unfinished' }
            @{ name = 'Verify of driver-pending-restart, same boot'; phase = 'driver-pending-restart'; saved = $bootId; args = $verify; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'Verify of driver-pending-restart, current BootId unreadable'; phase = 'driver-pending-restart'; saved = $bootId + 1; args = $verify; env = $unreadable; code = 7; msg = 'result.restart-still-pending' }
            @{ name = 'Verify of driver-pending-restart, new boot: unfinished'; phase = 'driver-pending-restart'; saved = $bootId + 1; args = $verify; code = 2; msg = 'result.install-unfinished' }
            @{ name = 'Verify of installed, same boot'; phase = 'installed'; saved = $bootId; args = $verify; code = 7; msg = 'result.verify-before-restart' }
            @{ name = 'Verify of installed, no saved boot'; phase = 'installed'; saved = $null; args = $verify; code = 7; msg = 'result.verify-before-restart' }
            @{ name = 'Verify of installed, current BootId unreadable'; phase = 'installed'; saved = $bootId + 1; args = $verify; env = $unreadable; code = 7; msg = 'result.verify-before-restart' }
            @{ name = 'Verify of installed, new boot (positive control: the checks run)'; phase = 'installed'; saved = $bootId + 1; args = $verify; code = 3; msg = 'result.verify-failed' })) {
        $st = [ordered]@{ schema = 1; phase = $c.phase; package_version = $pkgVersion; install_root = (Join-Path $env:ProgramFiles 'amdgpu-wddm'); updated_utc = '2000-01-01T00:00:00.0000000Z' }
        if ($null -ne $c.saved) { $st.restart_boot_id = $c.saved }
        $env1 = $(if ($c.env) { $c.env } else { @{} })
        $r = Invoke-Engine $c.name $c.args -State $st -Env $env1
        Test-Stream $r $c.name
        $ran = @(Get-Events $r 'stage' | Where-Object { $_.id -in 'verify', 'install', 'files' }).Count
        $kept = [string](Get-Content -LiteralPath (Join-Path $r.state_dir 'state.json') -Raw | ConvertFrom-Json).phase
        $ok = ($r.code -eq $c.code) -and ($r.result.message_id -eq $c.msg)
        if ($c.code -eq 7) { $ok = $ok -and $r.result.restart.still_pending -and -not $ran -and -not (Get-Events $r 'step').Count -and ($kept -eq $c.phase) }
        if ($c.code -eq 2) { $ok = $ok -and -not $ran -and -not (Get-Events $r 'step').Count -and ($kept -eq $c.phase) }
        if ($c.code -in 0, 3) { $ok = $ok -and $ran }
        Check $ok "$($c.name): exit $($r.code), $($r.result.message_id), phase kept $kept ($($r.result.detail))"
    }
} else { '  (skipped: BootId not readable on this PC)' }

'[G-EVT] verification and its report use one manifest, the installed one (R5)'
if ($null -ne $bootId) {
    # An installed release B (another version and ABI) under a test install root; package A (this package) runs Verify.
    $instB = Join-Path $work 'installed-b'
    [void][IO.Directory]::CreateDirectory($instB)
    $mB = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
    $mB.version = '0.0.1.100-other.1'; $mB.name = 'amdgpu-wddm-tester-0.0.1.100-other.1'; $mB.kmd_abi = '0x000700AA'
    [IO.File]::WriteAllText((Join-Path $instB 'manifest.json'), ($mB | ConvertTo-Json -Depth 8))
    $shaB = (Get-FileHash -LiteralPath (Join-Path $instB 'manifest.json') -Algorithm SHA256).Hash
    $st = [ordered]@{ schema = 1; phase = 'installed'; package_version = $mB.version; install_root = $instB; updated_utc = '2000-01-01T00:00:00.0000000Z'; restart_boot_id = $bootId + 1 }
    $r = Invoke-Engine 'verify of another installed release' @('-DryRun', '-Verify') -State $st
    $rep = @(Get-ChildItem -LiteralPath (Join-Path $r.state_dir 'verify') -Filter 'verify-*.json' -ErrorAction SilentlyContinue)
    $vr = $(if ($rep.Count -eq 1) { Get-Content -LiteralPath $rep[0].FullName -Raw | ConvertFrom-Json })
    Check (($r.code -eq 3) -and $vr -and ($vr.package_version -eq $mB.version) -and ($vr.release -eq $mB.name) -and ($vr.manifest_sha256 -eq $shaB) -and ($vr.manifest_source -eq 'install-root') -and ($vr.kmd_abi -eq '0x000700AA') -and ($r.text -match [regex]::Escape("verifies against $instB\manifest.json (install-root): $($mB.version)"))) "package $pkgVersion verifies installed $($mB.version): checks and report both name the installed manifest ($(if ($vr) { "$($vr.package_version), $($vr.manifest_source)" } else { 'no report' }))"
} else { '  (skipped: BootId not readable on this PC)' }

'[G-STAGE] deadline: the run stops at the first stop point after -DeadlineUtc (interfaces-setup.md section 10)'
$past = [DateTime]::UtcNow.AddMinutes(-1).ToString('o')
$r = Invoke-Engine 'deadline passed' @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning', '-DeadlineUtc', $past)
Test-Stream $r 'deadline passed'
Check (($r.code -eq 8) -and ($r.result.outcome -eq 'cancelled') -and ($r.result.message_id -eq 'result.deadline') -and -not $r.result.mutated -and ($r.result.stop.by -eq 'deadline') -and ($r.result.stop.where -eq 'stage:preflight') -and -not (Get-Events $r 'step').Count) "deadline passed: exit 8, result.deadline at $($r.result.stop.where), nothing changed, no step"
Check ((@(Get-Events $r 'start')[0].job -eq 'kill-on-close') -and ($r.result.children.job -eq 'kill-on-close') -and ($r.result.children.left_at_exit -eq 0) -and ($r.result.children.closure -eq 'complete')) "the run's own job object: $($r.result.children.job), $($r.result.children.left_at_exit) process(es) left at its end"
$r = Invoke-Engine 'deadline unreadable' @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning', '-DeadlineUtc', 'soon')
Check (($r.code -eq 8) -and ($r.result.message_id -eq 'result.deadline') -and ($r.result.deadline_utc -match '^unreadable')) 'an unreadable deadline counts as passed: stopped before any change'
$future = [DateTime]::UtcNow.AddMinutes(10).ToString('o')
$r = Invoke-Engine 'deadline later' @('-Plan', '-DryRunIgnoreBoard', '-DeadlineUtc', $future)
Test-Stream $r 'deadline later'
Check (($r.code -eq 0) -and ($r.result.outcome -eq 'planned') -and ($null -eq $r.result.stop) -and (@(Get-Events $r 'start')[0].deadline_utc) -and ($r.result.deadline_utc)) 'a deadline that has not passed: the plan completes, start and result name the deadline'
$prepDest = Join-Path $work 'prepared-deadline'
$r = Invoke-Engine 'prepare deadline passed' @('-Destination', $prepDest, '-DeadlineUtc', $past) -Script 'prepare-offline.ps1'
Test-Stream $r 'prepare deadline passed'
Check (($r.code -eq 8) -and ($r.result.message_id -eq 'result.deadline') -and ($r.result.stop.where -eq 'stage:prepare-check') -and -not (Test-Path -LiteralPath $prepDest) -and -not (Test-Path -LiteralPath "$prepDest.partial")) 'prepare-offline after its deadline: stopped at the first stage, no folder written'

'[G-STAGE] cancel on both sides of every stop point (R8): before it stops there; after it the run goes on to the next one'
# AMDGPU_WDDM_TEST_CANCEL_AT creates the cancel file just before or just after a stop point looks (dry run only). A
# fresh dry run walks phase 1 (before-changes, after-staging) and phase 2 (before-driver-install) in one process;
# after the last stop point of a phase, Cancel is withdrawn ('cancel' event available false) and nothing stops.
function Get-CancelOffers($R) { return (@(Get-Events $R 'cancel') | ForEach-Object { "$($_.where)=$($_.available)" }) -join ',' }
if (-not $tsActive) {
    foreach ($c in @(
            @{ at = 'before-changes:before'; code = 8; where = 'before-changes' }
            @{ at = 'before-changes:after'; code = 8; where = 'stage:test-signing' }
            @{ at = 'after-staging:before'; code = 8; where = 'after-staging' }
            @{ at = 'after-staging:after'; code = 8; where = 'stage:install'; withdrawn = 'test-signing' }
            @{ at = 'before-driver-install:before'; code = 8; where = 'before-driver-install' }
            @{ at = 'before-driver-install:after'; code = 0; where = $null; withdrawn = 'driver-install' })) {
        $r = Invoke-Engine "cancel $($c.at)" @('-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning') -Env @{ AMDGPU_WDDM_TEST_CANCEL_AT = $c.at }
        Test-Stream $r "cancel $($c.at)"
        $offers = Get-CancelOffers $r
        $ok = ($r.code -eq $c.code) -and ([string]$r.result.stop.where -eq [string]$c.where)
        if ($c.code -eq 8) { $ok = $ok -and ($r.result.stop.by -eq 'cancel') }
        if ($c.withdrawn) { $ok = $ok -and ($offers -match "(^|,)$($c.withdrawn)=False") }
        Check $ok "cancel at $($c.at): exit $($r.code), stopped at $(if ($r.result.stop) { $r.result.stop.where } else { 'no stop point (the run finished)' }); offers $offers"
    }
} else { '  (skipped: test signing is active on this PC, phase 1 has no after-staging point)' }
if ($FirmwareDir) {
    foreach ($c in @(
            @{ at = 'before-copy:before'; code = 8; where = 'before-copy'; folder = $false }
            @{ at = 'before-copy:after'; code = 8; where = 'stage:copy'; folder = $false }
            @{ at = 'after-copy:before'; code = 8; where = 'after-copy'; folder = $false }
            @{ at = 'after-copy:after'; code = 0; where = $null; folder = $true })) {
        $dest = Join-Path $work ('prepared-cancel-' + ($c.at -replace '[^a-z]+', '-'))
        $r = Invoke-Engine "prepare cancel $($c.at)" @('-Destination', $dest, '-FirmwareDir', $FirmwareDir) -Script 'prepare-offline.ps1' -Env @{ AMDGPU_WDDM_TEST_CANCEL_AT = $c.at }
        Test-Stream $r "prepare cancel $($c.at)"
        $ok = ($r.code -eq $c.code) -and ([string]$r.result.stop.where -eq [string]$c.where) -and ((Test-Path -LiteralPath $dest) -eq $c.folder) -and -not (Test-Path -LiteralPath "$dest.partial")
        Check $ok "prepare-offline, cancel at $($c.at): exit $($r.code), stopped at $(if ($r.result.stop) { $r.result.stop.where } else { 'no stop point (prepared)' }), folder $(Test-Path -LiteralPath $dest), no .partial left"
    }
} else { '  (prepare-offline cases skipped: no -FirmwareDir)' }

'[G-STAGE] a changing setup-window run without its job object refuses before any change'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Not elevated here: had the refusal failed, the run would stop at needs-admin, never reach a change.
    $r = Invoke-Engine 'no job, real run' @('-AcceptTestSigning') -Env @{ AMDGPU_WDDM_TEST_NO_JOB = '1' }
    Test-Stream $r 'no job, real run'
    Check (($r.code -eq 2) -and ($r.result.message_id -eq 'result.preflight-error') -and ($r.result.detail -match 'cannot contain its child processes') -and -not $r.result.mutated -and -not (Get-Events $r 'step').Count -and (@(Get-Events $r 'start')[0].job -match '^none')) "no job: refused before any change ($($r.result.detail))"
    $r = Invoke-Engine 'no job, plan' @('-Plan', '-DryRunIgnoreBoard') -Env @{ AMDGPU_WDDM_TEST_NO_JOB = '1' }
    Check (($r.code -eq 0) -and ($r.result.outcome -eq 'planned') -and ($r.result.children.closure -eq 'unknown')) 'no job, a plan (changes nothing): runs, closure recorded as unknown'
} else { '  (skipped: this shell is elevated; the refusal test runs only where a failed refusal cannot reach a change)' }

'[G-STAGE] child closure: every process the engine started ends with it'
$env:AMDGPU_WDDM_TEST_CHILD_SECONDS = '120'
try { $r = Invoke-Engine 'children at the end' @('-Plan', '-DryRunIgnoreBoard') } finally { Remove-Item Env:\AMDGPU_WDDM_TEST_CHILD_SECONDS }
$tc = @(Get-Events $r 'test-child')
Check (($r.code -eq 0) -and $tc.Count -and ($r.result.children.left_at_exit -ge 2) -and ($r.result.children.ended -eq $r.result.children.left_at_exit) -and ($r.result.children.closure -eq 'complete') -and ($r.result.children.remaining -eq 0) -and -not (Get-Process -Id ([int]$tc[0].pid) -ErrorAction SilentlyContinue) -and @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$([int]$tc[0].pid)").Count -eq 0) "normal end: the engine ended its $($r.result.children.left_at_exit) left-over child process(es) (cmd.exe and its ping) and recorded them"
# The caller terminates the engine while its child and grandchild run: Windows ends both with the job.
$kdir = Join-Path $work 'kill'
[void][IO.Directory]::CreateDirectory($kdir)
$kev = Join-Path $kdir 'events.jsonl'
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $ps51
foreach ($x in @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Package 'installer\install.ps1'), '-Gui', '-InvocationId', ([guid]::NewGuid().ToString()), '-EventsFile', $kev, '-ResultFile', (Join-Path $kdir 'result.json'), '-DryRun', '-DryRunIgnoreBoard', '-AcceptTestSigning')) { [void]$psi.ArgumentList.Add($x) }
$psi.Environment['PSModulePath'] = (@([Environment]::GetEnvironmentVariable('PSModulePath', 'Machine'), [Environment]::GetEnvironmentVariable('PSModulePath', 'User')) | Where-Object { $_ }) -join ';'
$psi.Environment['AMDGPU_WDDM_TEST_CHILD_SECONDS'] = '120'
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$engine = [Diagnostics.Process]::Start($psi)
$childPid = $null; $grandPid = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 60 -and -not $engine.HasExited -and -not $grandPid) {
    Start-Sleep -Milliseconds 200
    if (-not $childPid -and (Test-Path -LiteralPath $kev)) {
        $e = @(Get-Content -LiteralPath $kev | Where-Object { $_ -match '"type":"test-child"' } | ForEach-Object { $_ | ConvertFrom-Json })
        if ($e.Count) { $childPid = [int]$e[0].pid }
    }
    if ($childPid) { $g = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$childPid" | Where-Object { $_.Name -ieq 'PING.EXE' }); if ($g.Count) { $grandPid = [int]$g[0].ProcessId } }
}
$wasRunning = -not $engine.HasExited
if ($wasRunning) { $engine.Kill() }
[void]$engine.WaitForExit(30000)
$gone = $false
for ($i = 0; $i -lt 50 -and -not $gone -and $childPid -and $grandPid; $i++) {
    $gone = -not (Get-Process -Id $childPid -ErrorAction SilentlyContinue) -and -not (Get-Process -Id $grandPid -ErrorAction SilentlyContinue)
    if (-not $gone) { Start-Sleep -Milliseconds 100 }
}
Check ($wasRunning -and $childPid -and $grandPid -and $gone -and -not (Test-Path -LiteralPath (Join-Path $kdir 'result.json'))) "engine terminated by its caller after $([int]$clock.Elapsed.TotalSeconds) s: its cmd.exe and ping.exe ended with it (no result: changes unknown to the caller)"
if (-not $gone) { foreach ($p in $childPid, $grandPid) { if ($p) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue } } }

$after = Get-Footprint
Check ($before -eq $after) "system footprint unchanged: $after"
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
