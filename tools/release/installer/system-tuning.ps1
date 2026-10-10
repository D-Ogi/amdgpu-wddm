# Optional Windows tuning. Only the manifest-bound package closure is executable.
# The durable journal belongs to the backend, outside the installation directory.
$script:SystemTuningFiles = @('system-tuning.ps1', 'SystemTuning.Core.psm1', 'SystemTuning.Native.psm1')

function Write-SystemTuningPlan([bool]$Selected, $Manifest) {
    if (-not $Selected) { Write-Info 'Windows tuning: not selected. Existing Windows settings stay unchanged.'; return }
    if (-not $Manifest -or -not $Manifest.system_tuning -or $null -eq $Manifest.system_tuning.recommended_items) { throw 'The package has no Windows tuning plan from the control application.' }
    Write-Info 'Windows tuning selected: apply the control application recommended background settings through its shared backend.'
    foreach ($item in @($Manifest.system_tuning.recommended_items)) { Write-Info ("  $($item.label) [$($item.id)]: $($item.note)") }
    Write-Info 'The control application manages individual choices and restoration of saved settings.'
}

function Get-SystemTuningScript([string]$PackageRoot) {
    $root = [IO.Path]::GetFullPath($PackageRoot)
    $manifest = Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw | ConvertFrom-Json
    # An installed release keeps the same manifest with package-relative file names.
    $payload = Join-Path $root 'payload\system-tuning'
    if (-not (Test-Path -LiteralPath $payload -PathType Container)) { $payload = Join-Path $root 'system-tuning' }
    foreach ($name in $script:SystemTuningFiles) {
        $relative = 'payload/system-tuning/' + $name
        $rows = @($manifest.files | Where-Object { ([string]$_.path).Replace('\', '/') -ceq $relative })
        if ($rows.Count -ne 1 -or [string]$rows[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "Windows tuning: missing or ambiguous manifest entry $relative" }
        $path = Join-Path $payload $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Windows tuning: missing $relative" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine [string]$rows[0].sha256) { throw "Windows tuning: hash mismatch for $relative" }
    }
    return (Join-Path $payload 'system-tuning.ps1')
}

function Invoke-TuningProcess([string]$ScriptPath, [ValidateSet('ApplyRecommended','RestoreAll')][string]$Action,
    [ValidateRange(1,120)][int]$TimeoutSeconds = 120) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $start.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $ScriptPath + '" -Action ' + $Action + ' -Scope Machine'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = New-Object Text.UTF8Encoding $false
    $start.StandardErrorEncoding = New-Object Text.UTF8Encoding $false
    $p = New-Object Diagnostics.Process
    $p.StartInfo = $start
    try {
        if (-not $p.Start()) { throw 'Windows tuning: could not start the backend' }
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            try { $p.Kill() } catch { }
            $stopped = $p.WaitForExit(3000)
            throw "Windows tuning exceeded $TimeoutSeconds seconds (process stopped: $stopped). Its last change is unconfirmed. Installation stopped and saved originals remain available for recovery."
        }
        if (-not $stdout.Wait(3000) -or -not $stderr.Wait(3000)) { throw 'Windows tuning output did not close. The result is unconfirmed and installation stopped.' }
        return @{ code = $p.ExitCode; text = $stdout.Result; error = $stderr.Result }
    } finally { $p.Dispose() }
}

function Save-SystemTuningRecovery([string]$PackageRoot) {
    # Keep code separately from the protected machine and per-user journals.
    $source = Split-Path (Get-SystemTuningScript $PackageRoot)
    $destination = Join-Path $env:ProgramData 'amdgpu-wddm\system-tuning-recovery'
    foreach ($path in @((Split-Path $destination), $destination)) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Windows tuning recovery directory is not a regular directory.' }
            $acl = Get-Acl -LiteralPath $path
            $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
            if ($owner -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Windows tuning recovery directory has an unexpected owner.' }
            foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
                $write = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership -bor [Security.AccessControl.FileSystemRights]::Delete
                if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544') -and ($rule.FileSystemRights -band $write)) { throw 'Windows tuning recovery directory grants write access to another account.' }
            }
        } else {
            $acl = New-Object Security.AccessControl.DirectorySecurity
            $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-544'))
            $acl.SetAccessRuleProtection($true,$false)
            foreach ($sid in 'S-1-5-18','S-1-5-32-544','S-1-5-32-545') {
                $rights = if ($sid -eq 'S-1-5-32-545') { 'ReadAndExecute' } else { 'FullControl' }
                $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier $sid),$rights,'ContainerInherit,ObjectInherit','None','Allow')))
            }
            [void](New-Object IO.DirectoryInfo $path).Create($acl)
        }
    }
    foreach ($name in $script:SystemTuningFiles) {
        $target = Join-Path $destination $name
        if (Test-Path -LiteralPath $target) {
            $item = Get-Item -LiteralPath $target -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Windows tuning recovery file is not a regular file.' }
            # Replace a directory entry, never write through an old hard link or preserve an old file ACL.
            Remove-Item -LiteralPath $target -Force
        }
        $original = Join-Path $source $name
        Copy-Item -LiteralPath $original -Destination $target -Force
        if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ine (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { throw 'Windows tuning recovery copy failed verification.' }
    }
    Write-Info ('Recovery tools kept at ' + $destination + '. In the account whose settings need restoring, run powershell -NoProfile -File "' + (Join-Path $destination 'system-tuning.ps1') + '" -Action RestoreAll -Scope User')
}

function Remove-UserDataExceptTuning([string]$Directory) {
    $root = [IO.Path]::GetFullPath($Directory)
    $item = Get-Item -LiteralPath $root -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Per-user data is not a regular directory. It was retained.' }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'system-tuning'))) { Remove-PathOrSchedule $root; return }
    # Preserve the whole user journal. Do not open it from an elevated account.
    foreach ($item in @(Get-ChildItem -LiteralPath $root -Force)) {
        if ($item.Name -ieq 'system-tuning') { continue }
        Remove-PathOrSchedule $item.FullName
    }
}

function Invoke-SelectedSystemTuning {
    param([bool]$Selected, [ValidateSet('ApplyRecommended','RestoreAll')][string]$Action, [string]$PackageRoot)
    if (-not $Selected) { return }
    $tuningAction = $Action
    # Invoke-Change never evaluates the action during Plan/DryRun, including backend loading.
    Invoke-Change "Windows tuning: $Action (saved originals remain outside the installation directory)" {
        $entry = Get-SystemTuningScript $PackageRoot
        $r = Invoke-TuningProcess -ScriptPath $entry -Action $tuningAction
        try { $result = $r.text | ConvertFrom-Json } catch { throw "Windows tuning returned invalid JSON (exit $($r.code)): $($r.error)" }
        if ($r.code -ne 0 -or $result.schema -ne 1 -or $result.ok -isnot [bool] -or -not $result.ok -or $result.action -cne $tuningAction -or $result.scope -cne 'Machine' -or $result.dryRun -isnot [bool] -or $result.dryRun) {
            throw "Windows tuning $tuningAction failed (exit $($r.code)): $($result.error). Saved originals remain available."
        }
        Write-Info "Windows tuning $tuningAction completed. The recovery journal is retained."
    } | Out-Null
}
