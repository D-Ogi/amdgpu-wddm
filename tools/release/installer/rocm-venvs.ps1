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

# The entries of the list: one row per *.json file, with the venv it names, or why it names none.
function Get-RocmVenvEntries([string]$ListDir = $script:RocmVenvListDir) {
    $rows = @()
    if (-not (Test-Path -LiteralPath $ListDir -PathType Container)) { return $rows }
    foreach ($f in @(Get-ChildItem -LiteralPath $ListDir -File -Filter '*.json' -ErrorAction Stop | Sort-Object Name)) {
        $venv = ''
        $problem = ''
        try {
            $e = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
            if ($null -ne $e -and $e.PSObject.Properties['venv']) { $venv = [string]$e.venv }
            if (-not $venv) { $problem = 'names no venv' }
        } catch { $problem = "cannot be read ($($_.Exception.Message))" }
        if ($venv -and $venv -notmatch '^[A-Za-z]:\\[^\\]') { $problem = "names $venv, which is not a folder on a local drive letter"; $venv = '' }
        $rows += [pscustomobject]@{ file = $f.FullName; venv = $venv; problem = $problem }
    }
    return $rows
}

# Detaches every listed venv with the attach step of $Payload (<install root>\tools\hip\rocm). One row per entry:
# result detached, gone (the venv folder does not exist), unreadable, kept (the step did not change the venv), or
# planned (a dry run: the step ran with -PlanOnly and changed nothing). Each change goes through Invoke-Change.
function Invoke-RocmVenvDetach {
    param([Parameter(Mandatory)][string]$Payload, [string]$ListDir = $script:RocmVenvListDir,
          [string]$Shell = $script:RocmVenvShell, [double]$TimeoutSeconds = $script:RocmVenvTimeoutSeconds)
    $results = @()
    $entries = @()
    try { $entries = @(Get-RocmVenvEntries $ListDir) }
    catch { Write-Warn2 "the list of TheRock venvs in $ListDir cannot be read ($($_.Exception.Message)): a venv attached to our HIP runtime keeps our DLLs"; return $results }
    if (-not $entries.Count) { Write-Info "no TheRock venv is attached to our HIP runtime ($ListDir has no entry)"; return $results }
    $attachStep = Join-Path $Payload 'attach-rocm-venv.ps1'
    $hint = 'Close every Python of it, then reinstall rocm-sdk-core and rocm-sdk-libraries with pip (--force-reinstall): that puts AMD''s DLLs back.'
    foreach ($e in $entries) {
        $file = $e.file
        if ($e.problem) {
            Write-Info "skipped: the entry $file $($e.problem)"
            Invoke-Change "remove the entry $file, which names no venv to detach" { Remove-Item -LiteralPath $file -Force } | Out-Null
            $results += [pscustomobject]@{ venv = ''; file = $file; result = 'unreadable'; detail = $e.problem }
            continue
        }
        $venv = $e.venv
        if (-not (Test-Path -LiteralPath $venv -PathType Container)) {
            Write-Info "skipped: the TheRock venv $venv does not exist any more (entry $file)"
            Invoke-Change "remove the entry $file of the venv $venv, which is gone" { Remove-Item -LiteralPath $file -Force } | Out-Null
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'gone'; detail = 'the folder does not exist' }
            continue
        }
        if (-not (Test-Path -LiteralPath $attachStep)) {
            Write-Warn2 "the TheRock venv $venv keeps our HIP runtime DLLs: the attach step $attachStep is missing. $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = "$attachStep is missing" }
            continue
        }
        $argv = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $attachStep, '-Venv', $venv, '-Payload', $Payload,
            '-ListDir', $ListDir, '-Detach')
        if ($script:DryRunMode) {
            $plan = Invoke-NativeBounded -File $Shell -Arguments ($argv + @('-PlanOnly')) -TimeoutSeconds $TimeoutSeconds
            foreach ($line in @(([string]$plan.text) -split "`n")) { if ($line) { Write-Info "  $line" } }
            Invoke-Change "put AMD's DLLs back into the TheRock venv $venv (attach-rocm-venv.ps1 -Detach)" { } | Out-Null
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'planned'; detail = "plan exit $($plan.code)" }
            continue
        }
        $run = Invoke-Change "put AMD's DLLs back into the TheRock venv $venv (attach-rocm-venv.ps1 -Detach)" {
            Invoke-NativeBounded -File $Shell -Arguments $argv -TimeoutSeconds $TimeoutSeconds
        }
        foreach ($line in @(([string]$run.text) -split "`n")) { if ($line) { Write-Log "   $line" } }
        $last = @(([string]$run.text) -split "`n" | Where-Object { $_ -match '^ROCM-ATTACH ' }) | Select-Object -Last 1
        if ($run.timed_out) {
            Write-Warn2 "the TheRock venv $venv may keep our HIP runtime DLLs: attach-rocm-venv.ps1 did not end within $TimeoutSeconds s (process $($run.id) still runs). $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = 'the detach did not end in time' }
        } elseif ($run.code -eq 0) {
            Write-Info "detached: the TheRock venv $venv has AMD's DLLs again"
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'detached'; detail = [string]$last }
        } else {
            Write-Warn2 "the TheRock venv $venv keeps our HIP runtime DLLs (attach-rocm-venv.ps1 exit $($run.code): $last). $hint"
            $results += [pscustomobject]@{ venv = $venv; file = $file; result = 'kept'; detail = "exit $($run.code): $last" }
        }
    }
    if ((Test-Path -LiteralPath $ListDir) -and -not @(Get-ChildItem -LiteralPath $ListDir -Force -ErrorAction SilentlyContinue).Count) {
        Invoke-Change "remove the empty list $ListDir" { Remove-Item -LiteralPath $ListDir -Force } | Out-Null
    }
    return $results
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
