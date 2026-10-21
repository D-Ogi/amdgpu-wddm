# BD-211: temporary files that earlier releases of the driver left behind.
#
# Mesa's u_memstream on Windows (src/util/memstream.c before the BD-211 fix) made one file with
# GetTempFileName(<temp>, "MEMSTREAM", 0, ...) for every stream, and its close called remove() before fclose(), so the
# remove failed on the open file and the file stayed. spirv_to_nir opens one stream per shader with specialization
# constants, so every program that compiled such shaders through our user-mode drivers (games, DWM on the zink route,
# Steam's browser) left one file per shader in its temp folder. GetTempFileName has 65535 names per prefix: once they
# are used, every new name costs 65535 failed file creations, which is seconds of kernel time per shader compile.
#
# The names are the ones GetTempFileName gives for that prefix: "MEM", one to four hex digits, ".TMP". The installer
# removes such files when they are older than a day and not open. Another program that uses the same three letters
# loses only temporary files older than a day, which Windows Storage Sense also deletes.

$script:MesaTempNamePattern = '^MEM[0-9A-F]{1,4}\.TMP$'

# The temp folder of every profile Windows knows (ProfileList includes the service profiles) and the Windows temp
# folder, which processes without a profile of their own use. Only folders that exist.
function Get-MesaTempRoots {
    $roots = New-Object Collections.Generic.List[string]
    $list = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
    foreach ($k in @(Get-ChildItem -LiteralPath $list -ErrorAction SilentlyContinue)) {
        $p = [string](Get-ItemProperty -LiteralPath $k.PSPath -Name ProfileImagePath -ErrorAction SilentlyContinue).ProfileImagePath
        if ($p) { $roots.Add((Join-Path ([Environment]::ExpandEnvironmentVariables($p)) 'AppData\Local\Temp')) }
    }
    if ($env:windir) { $roots.Add((Join-Path $env:windir 'Temp')) }
    return @($roots | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Sort-Object -Unique)
}

# Read only: the files under $Roots (top level of each folder) whose name matches and whose last write is older than
# $MinAge. A folder that cannot be read is counted in 'unreadable' and skipped.
function Find-MesaTempLeftovers {
    param([string[]]$Roots, [TimeSpan]$MinAge = (New-TimeSpan -Hours 24), [DateTime]$NowUtc = [DateTime]::UtcNow)
    $files = New-Object Collections.Generic.List[IO.FileInfo]
    $young = 0; $unreadable = 0
    $cut = $NowUtc - $MinAge
    foreach ($root in @($Roots)) {
        try {
            foreach ($f in ([IO.DirectoryInfo]$root).EnumerateFiles('MEM*.TMP')) {
                if ($f.Name -notmatch $script:MesaTempNamePattern) { continue }
                if ($f.LastWriteTimeUtc -gt $cut) { $young++; continue }
                $files.Add($f)
            }
        } catch { $unreadable++ }
    }
    $bytes = [long]0; foreach ($f in $files) { $bytes += $f.Length }
    return [pscustomobject]@{ files = $files.ToArray(); count = $files.Count; bytes = $bytes; young = $young; unreadable = $unreadable }
}

# Removes the files Find-MesaTempLeftovers found. A file that is open without delete sharing (the program that made
# it still runs) or that this account may not delete stays; neither stops the installation.
function Remove-MesaTempLeftovers {
    param([IO.FileInfo[]]$Files)
    $removed = 0; $bytes = [long]0; $inUse = 0; $denied = 0
    foreach ($f in @($Files)) {
        $f.Refresh()
        if (-not $f.Exists) { continue }
        $len = $f.Length
        try { [IO.File]::Delete($f.FullName); $removed++; $bytes += $len }
        catch [UnauthorizedAccessException] { $denied++ }
        catch { $inUse++ }
    }
    return [pscustomobject]@{ removed = $removed; bytes = $bytes; in_use = $inUse; denied = $denied }
}
