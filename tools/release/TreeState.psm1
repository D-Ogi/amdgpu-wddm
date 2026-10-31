# The state of the recipe tree that the release gates read (BD-263).
#
# build-release.ps1 runs the quality suite (quick.cmd) and its other gates on its own repository as that tree is on
# disk. A package names one commit, so the gates must read that commit: an uncommitted edit in the tree is not in the
# package, and a foreign edit made during a run (tester.32 round 6) failed a clean commit. These functions record the
# commit and the uncommitted content of a tree, refuse a tree that is not clean, and refuse a tree that changed
# between two points of a run. A lab candidate (-NoProvenance) is built from a dirty tree on purpose; it records
# the dirty files, and its tree must still not change during the run.
Set-StrictMode -Version 3.0

function Invoke-TreeGit([string]$Repo, [string[]]$Arguments) {
    $text = @(& git -C $Repo @Arguments 2>&1)
    if ($LASTEXITCODE) { throw "git $($Arguments -join ' ') in $Repo failed (exit $LASTEXITCODE): $($text -join ' ')" }
    return $text
}

# The commit, the uncommitted entries and one SHA-256 over the uncommitted content of a tree. $Scratch is a
# directory for one temporary file (git diff writes it there); the file is removed again.
function Get-TreeState([string]$Repo, [string]$Scratch) {
    $Repo = [IO.Path]::GetFullPath($Repo)
    $head = @(Invoke-TreeGit $Repo @('rev-parse', '--verify', 'HEAD'))[0].ToString().Trim()
    if ($head -notmatch '^[0-9a-f]{40}$') { throw "$Repo has no commit at HEAD ($head)" }
    # Untracked files count: a new source file changes what a gate reads. Ignored files do not: build products.
    $status = @(Invoke-TreeGit $Repo @('-c', 'core.quotepath=false', 'status', '--porcelain=v1', '--untracked-files=all',
                                       '--ignore-submodules=none') | ForEach-Object { $_.ToString() } | Where-Object { $_ })
    $digest = ''
    if ($status.Count) {
        [void][IO.Directory]::CreateDirectory($Scratch)
        $diff = Join-Path $Scratch ('tree-diff-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.bin')
        try {
            $null = Invoke-TreeGit $Repo @('diff', 'HEAD', '--binary', '--no-ext-diff', '--no-color', '--ignore-submodules=none', "--output=$diff")
            $parts = [Collections.Generic.List[string]]::new()
            $parts.Add((Get-FileHash -LiteralPath $diff -Algorithm SHA256).Hash)
            # git diff does not show the content of an untracked file: its blob id does.
            $untracked = @(Invoke-TreeGit $Repo @('-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard') |
                ForEach-Object { $_.ToString() } | Where-Object { $_ } | Sort-Object)
            foreach ($u in $untracked) {
                $blob = @(Invoke-TreeGit $Repo @('hash-object', '--', $u))[0].ToString().Trim()
                $parts.Add("$u $blob")
            }
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $digest = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($parts -join "`n"))) }
            finally { $sha.Dispose() }
        } finally { Remove-Item -LiteralPath $diff -ErrorAction SilentlyContinue }
    }
    return [pscustomobject]@{ repo = $Repo; head = $head; clean = (-not $status.Count); entries = $status; digest = $digest
                             utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }
}

# Refuses a tree with uncommitted entries, unless the build says why it reads a work tree ($DirtyReason).
function Assert-TreeClean($State, [string]$DirtyReason) {
    if ($State.clean) { return }
    $list = @($State.entries | Select-Object -First 10) -join '; '
    if (-not $DirtyReason) {
        throw ("the recipe tree $($State.repo) is not clean ($($State.entries.Count) uncommitted entries, for example: $list). " +
               "The release gates read this tree, and the package names commit $($State.head.Substring(0, 8)): commit or " +
               'remove the edits, or build the package from a clean worktree of that commit (BD-263)')
    }
}

# Refuses a tree that is not the same as at the start of the run: another commit, or other uncommitted content.
function Assert-TreeUnchanged($Before, [string]$Scratch, [string]$What) {
    $now = Get-TreeState $Before.repo $Scratch
    $why = @()
    if ($now.head -ne $Before.head) { $why += "HEAD moved from $($Before.head.Substring(0, 8)) to $($now.head.Substring(0, 8))" }
    if ($now.digest -ne $Before.digest -or (($now.entries -join "`n") -cne ($Before.entries -join "`n"))) {
        $old = @($Before.entries); $new = @($now.entries)
        $added = @($new | Where-Object { $old -cnotcontains $_ }); $gone = @($old | Where-Object { $new -cnotcontains $_ })
        $text = "the uncommitted content changed ($($old.Count) entries at the start, $($new.Count) now"
        if ($added.Count) { $text += "; new: $(@($added | Select-Object -First 5) -join ', ')" }
        if ($gone.Count) { $text += "; gone: $(@($gone | Select-Object -First 5) -join ', ')" }
        if (-not $added.Count -and -not $gone.Count) { $text += '; the same entries with other content' }
        $why += $text + ')'
    }
    if ($why.Count) {
        throw ("the recipe tree $($Before.repo) changed during the run ($What): $($why -join '; '). The gates of this run " +
               'did not read the tree that the package names; do not edit the release tree while a package builds (BD-263)')
    }
    return $now
}

Export-ModuleMember -Function Get-TreeState, Assert-TreeClean, Assert-TreeUnchanged
