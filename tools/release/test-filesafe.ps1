# Host test of the installer's file replacement and re-run logic (installer\common.ps1), under Windows PowerShell 5.1
# as the installer runs. Works only inside -WorkRoot; changes nothing else on the computer: no administrator rights,
# no MoveFileEx (ScheduleOldCopies off), no state file, no registry.
#   powershell -NoProfile -NonInteractive -File tools\release\test-filesafe.ps1 -Installer <package>\installer -WorkRoot <empty test folder>
#   -Case trap: a step that throws under the installer's trap; the caller checks the message and exit code 6.
# A file that DWM or an application has loaded is simulated by a handle opened for read with FileShare Read|Delete:
# like an image section, it refuses a writer and allows a rename. The test shows that a plain Copy-Item fails on it
# (the lab failure at install.ps1:345) before it shows that Copy-FileSafe replaces it.
param([Parameter(Mandatory)][string]$Installer, [Parameter(Mandatory)][string]$WorkRoot, [ValidateSet('files', 'trap')][string]$Case = 'files')
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$script:DryRunMode = $false
$script:LogPath = $null
$script:ScheduleOldCopies = $false

if ($Case -eq 'trap') {
    & {
        trap { Write-StepFailure $_; exit 6 }
        Invoke-Change 'first test step' { 'first step done' } | Out-Null
        Invoke-Change 'second test step (throws)' { throw 'simulated failure' } | Out-Null
        Write-Host 'not reached'
    }
    exit 0
}

$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { Write-Host "  PASS $Text" } else { Write-Host "  FAIL $Text"; $script:fail++ } }
function New-TestFile([string]$Path, [string]$Text) { [void][IO.Directory]::CreateDirectory((Split-Path $Path)); [IO.File]::WriteAllText($Path, $Text) }
function Open-Held([string]$Path) { New-Object IO.FileStream -ArgumentList $Path, ([IO.FileMode]::Open), ([IO.FileAccess]::Read), ([IO.FileShare]([IO.FileShare]::Read -bor [IO.FileShare]::Delete)) }

if (Test-Path -LiteralPath $WorkRoot) { throw "WorkRoot $WorkRoot exists: give an empty, new folder" }
$src = Join-Path $WorkRoot 'src'
$dst = Join-Path $WorkRoot 'dst'

Write-Host 'single files'
New-TestFile "$src\same.dll" 'release build'
New-TestFile "$dst\same.dll" 'release build'
$t0 = (Get-Item -LiteralPath "$dst\same.dll").LastWriteTimeUtc
Start-Sleep -Milliseconds 50
$r = Copy-FileSafe -Source "$src\same.dll" -Destination "$dst\same.dll"
Check ($r -eq 'current') "equal SHA256: 'current' ($r)"
Check ((Get-Item -LiteralPath "$dst\same.dll").LastWriteTimeUtc -eq $t0) 'equal SHA256: target not rewritten'

New-TestFile "$src\free.dll" 'new build'
New-TestFile "$dst\free.dll" 'old build'
$r = Copy-FileSafe -Source "$src\free.dll" -Destination "$dst\free.dll"
Check ($r -eq 'copied') "different, not in use: 'copied' ($r)"
Check ((Get-Sha256 "$dst\free.dll") -eq (Get-Sha256 "$src\free.dll")) 'different, not in use: target equals the source'

New-TestFile "$src\held.dll" 'new stub'
New-TestFile "$dst\held.dll" 'old stub, loaded'
$h = Open-Held "$dst\held.dll"
try {
    $plain = $null
    try { Copy-Item -LiteralPath "$src\held.dll" -Destination "$dst\held.dll" -Force -ErrorAction Stop; $plain = 'overwrote' } catch { $plain = $_.Exception.Message }
    Check ($plain -ne 'overwrote') "positive control: plain Copy-Item over the held file fails ($plain)"
    $r = Copy-FileSafe -Source "$src\held.dll" -Destination "$dst\held.dll"
    Check ($r -eq 'replaced-in-use') "different, in use: 'replaced-in-use' ($r)"
    Check ((Get-Sha256 "$dst\held.dll") -eq (Get-Sha256 "$src\held.dll")) 'different, in use: target equals the source'
    $old = @(Get-ChildItem -LiteralPath $dst -File -Filter 'held.dll.old-*')
    Check ($old.Count -eq 1 -and $old[0].Name -match '^held\.dll\.old-\d{8}T\d{9}Z$') "old copy renamed to <name>.old-<utc> ($($old.Name -join ', '))"
    Check ([IO.File]::ReadAllText($old[0].FullName) -eq 'old stub, loaded') 'old copy keeps the old content'
    $buf = New-Object byte[] 3; [void]$h.Read($buf, 0, 3)
    Check ([Text.Encoding]::ASCII.GetString($buf) -eq 'old') 'the holder still reads its (old) file'
    # No check of Remove-OldCopies while held: this handle allows delete, a loaded image does not.
} finally { $h.Dispose() }
New-TestFile "$dst\keep.old-notadate" 'not ours'
Check ((Remove-OldCopies -Directory $dst) -eq 1) 'Remove-OldCopies removes the released old copy'
Check (Test-Path -LiteralPath "$dst\keep.old-notadate") 'Remove-OldCopies keeps a file that only looks similar'

Write-Host 'tree over a partial install, then a re-run'
$ts = Join-Path $WorkRoot 'tree-src'
$td = Join-Path $WorkRoot 'tree-dst'
New-TestFile "$ts\a.dll" 'a v2'
New-TestFile "$ts\b.dll" 'b v2'
New-TestFile "$ts\c.dll" 'c v2'
New-TestFile "$ts\sub\d.json" 'd v2'
New-TestFile "$td\a.dll" 'a v2'
New-TestFile "$td\b.dll" 'b v1 loaded'
New-TestFile "$td\c.dll" 'c v1'
$h = Open-Held "$td\b.dll"
try {
    $n = Copy-TreeSafe -Source $ts -Destination $td
    Check ($n.current -eq 1 -and $n.copied -eq 2 -and $n['replaced-in-use'] -eq 1) "first run: 1 current, 2 copied, 1 replaced in use ($($n.current)/$($n.copied)/$($n['replaced-in-use']))"
    $n = Copy-TreeSafe -Source $ts -Destination $td
    Check ($n.current -eq 4 -and $n.copied -eq 0 -and $n['replaced-in-use'] -eq 0) "re-run with the same package: all 4 already current ($($n.current)/$($n.copied)/$($n['replaced-in-use']))"
} finally { $h.Dispose() }
$same = $true
foreach ($f in Get-ChildItem -LiteralPath $ts -Recurse -File) {
    $rel = $f.FullName.Substring($ts.Length + 1)
    if ((Get-Sha256 $f.FullName) -ne (Get-Sha256 (Join-Path $td $rel))) { $same = $false }
}
Check $same 'tree: every target file equals its source'

Write-Host 'state facts recorded once'
$st = [pscustomobject]@{ schema = 1 }
$v1 = Set-StateValueOnce $st 'stub_existed' $false
$v2 = Set-StateValueOnce $st 'stub_existed' $true
Check (($v1 -eq $false) -and ($v2 -eq $false) -and ($st.stub_existed -eq $false)) 'a re-run keeps the first run''s stub_existed'

Write-Host 'registry keys (a scratch key under HKCU\Software, removed at the end)'
$rk = 'HKCU:\Software\amdgpu-wddm-installer-selftest-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
try {
    New-Item -Path $rk -Force | Out-Null
    New-ItemProperty -LiteralPath $rk -Name Keep -Value 1 -PropertyType DWord | Out-Null
    New-Item -Path "$rk\Sub" | Out-Null
    New-Item -Path $rk -Force | Out-Null
    $wiped = @((Get-Item -LiteralPath $rk).GetValueNames()).Count -eq 0
    Check $wiped 'positive control: New-Item -Force on an existing key deletes its values'
    New-ItemProperty -LiteralPath $rk -Name Keep -Value 1 -PropertyType DWord -Force | Out-Null
    New-Item -Path "$rk\Sub" -Force | Out-Null
    Initialize-RegistryKey $rk
    Initialize-RegistryKey "$rk\New\Deep"
    $item = Get-Item -LiteralPath $rk
    Check ((@($item.GetValueNames()) -contains 'Keep') -and (@($item.GetSubKeyNames()) -contains 'Sub')) 'Initialize-RegistryKey keeps the values and subkeys of an existing key'
    Check (Test-Path -LiteralPath "$rk\New\Deep") 'Initialize-RegistryKey creates a missing key with its parents'
} finally { Remove-Item -LiteralPath $rk -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $rk)) 'scratch registry key removed'

if ($fail) { Write-Host "$fail check(s) failed"; exit 1 }
Write-Host 'file checks passed'
exit 0
