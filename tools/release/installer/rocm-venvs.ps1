# The TheRock Python environments (venvs) that use our HIP runtime: the uninstaller puts AMD's DLLs back into each.
#
# <install root>\tools\hip\rocm\attach-rocm-venv.ps1 (compute\hip\attach-rocm-venv.ps1, compute\hip\blaslt\README.md)
# replaces amdhip64_7.dll and libhipblaslt.dll of a venv with ours and writes one entry per venv into
# %ProgramData%\amdgpu-wddm\rocm-venvs\<key>.json: { "venv": "<folder>", "utc", "therock", "payload" }. Without our
# driver, our face cannot open the device, so a venv that keeps it after the uninstall has no GPU in torch. The
# uninstaller runs attach-rocm-venv.ps1 -Detach for each entry while the attach step is still in the install root.
#
# The entries are written by users (the folder keeps the rights that ProgramData gives: users add files and change
# only their own), and the uninstaller runs as an administrator. So an entry is only data: its venv must be a folder on
# a local drive letter (a network path would make the administrator's account log on to the server it names), and the
# attach step refuses a junction or a link on the path and holds every directory of it while it changes files.
# A venv that is gone is skipped with a log line and its entry removed. A venv that the step does not change keeps its
# entry, a warning names it, and the footprint of the uninstaller lists it.
$script:RocmVenvListDir = Join-Path $env:ProgramData 'amdgpu-wddm\rocm-venvs'
$script:RocmVenvShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$script:RocmVenvTimeoutSeconds = 120
. (Join-Path $PSScriptRoot 'rocm-files.ps1')

# The entries of the list: one row per *.json file, with the venv it names, or why it names none.
function Get-RocmVenvEntries([string]$ListDir = $script:RocmVenvListDir) {
    $rows = @()
    $guard = New-Object Bc250.RocmFiles
    try {
    $ListDir = [Bc250.RocmFiles]::LocalPath($ListDir)
    if (-not $guard.HoldDirectory($ListDir, $false, $true)) { return $rows }
    foreach ($f in @(Get-ChildItem -LiteralPath $ListDir -File -Filter '*.json' -ErrorAction Stop | Sort-Object Name)) {
        $venv = ''
        $problem = ''
        $hash = ''
        try {
            if (-not $guard.Open($f.FullName, $false, $false)) { throw 'entry disappeared' }
            $hash = $guard.Hash($f.FullName)
            $e = $guard.Text($f.FullName) | ConvertFrom-Json -ErrorAction Stop
            if ($null -ne $e -and $e.PSObject.Properties['venv']) { $venv = [string]$e.venv }
            if (-not $venv) { $problem = 'names no venv' }
        } catch { $problem = "cannot be read ($($_.Exception.Message))" }
        if ($venv) {
            try { $venv = [Bc250.RocmFiles]::LocalPath($venv) }
            catch { $problem = $_.Exception.Message; $venv = '' }
        }
        $rows += [pscustomobject]@{ file = $f.FullName; venv = $venv; problem = $problem; sha256 = $hash }
    }
    } finally { $guard.Dispose() }
    return $rows
}

# Reopen with no sharing, verify the enumerated bytes, and remove that retained identity.
# Neither an unreadable link nor an entry changed since enumeration is removed by path.
function Remove-RocmVenvEntry($Entry) {
    if (-not $Entry.sha256) { throw "unsafe or unreadable entry remains: $($Entry.file)" }
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory((Split-Path -Parent $Entry.file), $false, $false)
        if (-not $guard.Open($Entry.file, $true, $false)) { return }
        if ($guard.Hash($Entry.file) -ne $Entry.sha256) { throw "entry changed since enumeration: $($Entry.file)" }
        $guard.Delete($Entry.file); $guard.Commit()
    } finally { $guard.Dispose() }
}

function Assert-RocmVenvRemovalComplete($Rows) {
    $blocked = @($Rows | Where-Object { $_.result -notin @('detached', 'gone', 'planned') })
    if ($blocked.Count) { throw "ROCm venv removal is incomplete ($($blocked.Count) entries); the driver and recovery payload must stay installed" }
}

# Detaches every listed venv with the attach step of $Payload (<install root>\tools\hip\rocm). One row per entry:
# result detached, gone (the venv folder does not exist), unreadable, kept (the step did not change the venv), or
# planned (a dry run: the step ran with -PlanOnly and changed nothing). Each change goes through Invoke-Change.
function Invoke-RocmVenvAction {
    param([Parameter(Mandatory)][string]$Payload, [string]$ListDir = $script:RocmVenvListDir,
          [string]$Shell = $script:RocmVenvShell, [double]$TimeoutSeconds = $script:RocmVenvTimeoutSeconds,
          [ValidateSet('Detach', 'Attach')][string]$Action = 'Detach')
    $results = @()
    $entries = @()
    try { $entries = @(Get-RocmVenvEntries $ListDir) }
    catch { Write-Warn2 "the list of TheRock venvs in $ListDir cannot be read ($($_.Exception.Message)): a venv attached to our HIP runtime keeps our DLLs"; return @([pscustomobject]@{venv='';file=$ListDir;result='kept';detail=$_.Exception.Message}) }
    if (-not $entries.Count) { Write-Info "no TheRock venv is attached to our HIP runtime ($ListDir has no entry)"; return $results }
    $attachStep = Join-Path $Payload 'attach-rocm-venv.ps1'
    $hint = 'Close every Python of it, then reinstall rocm-sdk-core and rocm-sdk-libraries with pip (--force-reinstall): that puts AMD''s DLLs back.'
    foreach ($e in $entries) {
        $file = $e.file
        if ($e.problem) {
            # A corrupt record can be the only remaining reference to an attached venv.
            # Unknown is not gone: preserve the receipt and prevent runtime removal.
            Write-Warn2 "kept: the entry $file $($e.problem); its venv state is unknown"
            $results += [pscustomobject]@{ venv = ''; file = $file; result = 'kept'; detail = $e.problem }
            continue
        }
        $venv = $e.venv
        $venvGuard = New-Object Bc250.RocmFiles
        try { $exists = $venvGuard.HoldDirectory($venv, $false, $true) }
        catch {
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = $_.Exception.Message }
            continue
        } finally { $venvGuard.Dispose() }
        if (-not $exists) {
            Write-Info "skipped: the TheRock venv $venv does not exist any more (entry $file)"
            Invoke-Change "remove the entry $file of the venv $venv, which is gone" { Remove-RocmVenvEntry $e } | Out-Null
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'gone'; detail = 'the folder does not exist' }
            continue
        }
        if (-not (Test-Path -LiteralPath $attachStep)) {
            Write-Warn2 "the TheRock venv $venv keeps our HIP runtime DLLs: the attach step $attachStep is missing. $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = "$attachStep is missing" }
            continue
        }
        $argv = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $attachStep, '-Venv', $venv, '-Payload', $Payload,
            '-ListDir', $ListDir)
        if ($Action -eq 'Detach') { $argv += '-Detach' }
        if ($script:DryRunMode) {
            $plan = Invoke-NativeBounded -File $Shell -Arguments ($argv + @('-PlanOnly')) -TimeoutSeconds $TimeoutSeconds
            foreach ($line in @(([string]$plan.text) -split "`n")) { if ($line) { Write-Info "  $line" } }
            if ($plan.timed_out -or $plan.code -ne 0) {
                $detail = "plan exit $($plan.code)"
                if ($plan.timed_out) { $detail = "the $Action plan did not end in time" }
                Write-Warn2 "the TheRock venv $venv has no successful $Action plan: $detail"
                $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = $detail }
            } else {
                Invoke-Change "apply ROCm $Action in the TheRock venv $venv" { } | Out-Null
                $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'planned'; detail = "plan exit $($plan.code)" }
            }
            continue
        }
        $run = Invoke-Change "apply ROCm $Action in the TheRock venv $venv" {
            Invoke-NativeBounded -File $Shell -Arguments $argv -TimeoutSeconds $TimeoutSeconds
        }
        foreach ($line in @(([string]$run.text) -split "`n")) { if ($line) { Write-Log "   $line" } }
        $last = @(([string]$run.text) -split "`n" | Where-Object { $_ -match '^ROCM-ATTACH ' }) | Select-Object -Last 1
        if ($run.timed_out) {
            Write-Warn2 "the TheRock venv $venv may keep our HIP runtime DLLs: attach-rocm-venv.ps1 did not end within $TimeoutSeconds s (process $($run.id) still runs). $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = "the $Action did not end in time" }
        } elseif ($run.code -eq 0) {
            $result = 'attached'
            if ($Action -eq 'Detach') {
                $result = 'detached'
                Remove-RocmVenvEntry $e
            }
            Write-Info "${result}: the TheRock venv $venv"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = $result; detail = [string]$last }
        } else {
            Write-Warn2 "the TheRock venv $venv keeps our HIP runtime DLLs (attach-rocm-venv.ps1 exit $($run.code): $last). $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = "exit $($run.code): $last" }
        }
    }
    # Keep the empty list directory: a separate path-based cleanup would reopen a reparse race.
    return $results
}

function Invoke-RocmVenvDetach {
    param([Parameter(Mandatory)][string]$Payload, [string]$ListDir = $script:RocmVenvListDir,
          [string]$Shell = $script:RocmVenvShell, [double]$TimeoutSeconds = $script:RocmVenvTimeoutSeconds)
    Invoke-RocmVenvAction -Payload $Payload -ListDir $ListDir -Shell $Shell -TimeoutSeconds $TimeoutSeconds -Action Detach
}
function Invoke-RocmVenvAttach {
    param([Parameter(Mandatory)][string]$Payload, [string]$ListDir = $script:RocmVenvListDir,
          [string]$Shell = $script:RocmVenvShell, [double]$TimeoutSeconds = $script:RocmVenvTimeoutSeconds)
    Invoke-RocmVenvAction -Payload $Payload -ListDir $ListDir -Shell $Shell -TimeoutSeconds $TimeoutSeconds -Action Attach
}

# The footprint row (common.ps1, Get-ReleaseFootprint): the venvs that are still listed, so still hold our DLLs.
function Get-RocmVenvFootprint([string]$ListDir = $script:RocmVenvListDir) {
    return (Read-ReleaseFootprintItem -Item 'TheRock venvs' -Read {
        $rows = @(Get-RocmVenvEntries $ListDir)
        if (-not $rows.Count) { return @{ present = $false; detail = "no venv is listed in $ListDir" } }
        $names = @($rows | ForEach-Object { if ($_.venv) { $_.venv } else { $_.file } })
        return @{ present = $true; detail = "$($rows.Count) listed in ${ListDir}: $($names -join ', ')" }
    })
}
