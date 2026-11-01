# File transaction functions. Import has no side effects; callers supply a verified path.
# Caller must load x3-env.ps1 first and prove boot health and stick identity.
function Write-X3Durable([string]$Path, [byte[]]$Bytes) {
    $f = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $f.Write($Bytes, 0, $Bytes.Length); $f.Flush($true) } finally { $f.Dispose() }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) -cne [Convert]::ToBase64String($Bytes)) { throw 'candidate readback mismatch' }
}

function Update-X3Counter([string]$EnvPath) {
    $old = [IO.File]::ReadAllBytes($EnvPath)
    $new = Clear-X3Env $old
    if ([Convert]::ToBase64String($old) -ceq [Convert]::ToBase64String($new)) { return 'already clear' }
    $tag = [Guid]::NewGuid().ToString('N')
    $candidate = "$EnvPath.x3-new-$tag"
    $backup = "$EnvPath.x3-backup-$tag"
    Write-X3Durable $candidate $new
    Write-X3Durable $backup $old
    # FAT32 has no universal File.Replace contract. Backup and rename preserve
    # old bytes. An interrupted missing env makes corrected GRUB select Linux.
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($EnvPath)) -cne [Convert]::ToBase64String($old)) { throw 'grubenv changed during acknowledgement' }
    [IO.File]::Delete($EnvPath)
    [IO.File]::Move($candidate, $EnvPath)
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($EnvPath)) -cne [Convert]::ToBase64String($new)) { throw 'grubenv final readback mismatch' }
    return ('cleared; backup=' + [IO.Path]::GetFileName($backup))
}
