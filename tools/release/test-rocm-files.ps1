# Synthetic file-identity and transaction tests. No installed venv or ProgramData access.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
# New-Item -ItemType HardLink or Junction reads [ ] in -Target as a wildcard (gate variable-case, "hardlink").
# CreateHardLinkW and mklink /J take the path as it is.
function New-HardLink([string]$Path, [string]$Existing) {
    if (-not ('Bc250Test.HardLink' -as [type])) {
        Add-Type -Namespace Bc250Test -Name HardLink -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CreateHardLinkW(string lpFileName, string lpExistingFileName, IntPtr lpSecurityAttributes);
'@
    }
    if (-not [Bc250Test.HardLink]::CreateHardLinkW($Path, $Existing, [IntPtr]::Zero)) {
        throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error(), "no hard link $Path to $Existing")
    }
}
function New-Junction([string]$Path, [string]$Target) {
    $text = & cmd.exe /d /c "mklink /J `"$Path`" `"$Target`""
    if ($LASTEXITCODE -ne 0) { throw "mklink /J could not link $Path to $Target (exit $LASTEXITCODE): $text" }
}
. (Join-Path $PSScriptRoot 'installer\rocm-files.ps1')
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('rocm-files-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($base)
$checks = 0
function Assert($Ok, [string]$Message) { if (-not $Ok) { throw "FAIL: $Message" }; $script:checks++; Write-Host "PASS $Message" }
function Refuses([scriptblock]$Action) { try { & $Action; return $false } catch { return $true } }
function Bytes([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text) }
$file = Join-Path $base 'live.dll'
$kept = Join-Path $base 'kept.dll'
$metadata = Join-Path $base 'record.json'
$alias = Join-Path $base 'alias.dll'
$moved = Join-Path $base 'moved.dll'
try {
    Bytes $file 'known AMD bytes'
    # Positive controls for the mutation methods used by the adversarial attempts below.
    Move-Item -LiteralPath $file -Destination $moved
    Move-Item -LiteralPath $moved -Destination $file
    New-HardLink $alias $file
    Assert ([IO.File]::ReadAllText($alias) -eq 'known AMD bytes') 'hardlink positive control'
    Remove-Item -LiteralPath $alias
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory($base, $false, $false)
        [void]$guard.Open($file, $true, $false)
        $hash = $guard.Hash($file)
        Assert (Refuses { Move-Item -LiteralPath $file -Destination $moved }) 'retained DLL cannot be renamed'
        Assert (Refuses { Bytes $file 'attacker bytes' }) 'retained DLL cannot be overwritten'
        # NTFS permits adding an alias even with no sharing. It cannot retarget the open handle.
        New-HardLink $alias $file
        Assert (Refuses { $guard.WriteText($file, 'replacement bytes') }) 'a newly added hardlink is detected before mutation'
        Remove-Item -LiteralPath $alias
        Assert ($guard.Hash($file) -eq $hash) 'hash remains on the retained identity'
        [void]$guard.Open($kept, $true, $true)
        $guard.Copy($file, $kept)
        $guard.WriteText($file, 'replacement bytes')
        Assert ($guard.Text($file) -eq 'replacement bytes') 'positive write through retained handle'
        # Simulate failure after DLL replacement, when the metadata output is being reserved.
        Bytes $metadata 'planted metadata'
        Assert (Refuses { [void]$guard.Open($metadata, $true, $true) }) 'CREATE_NEW refuses a planted record'
    } finally { $guard.Dispose() }
    Assert ([IO.File]::ReadAllText($file) -eq 'known AMD bytes' -and -not (Test-Path $kept)) 'failure rolls back live bytes and removes created kept file'
    Assert ([IO.File]::ReadAllText($metadata) -eq 'planted metadata') 'rollback does not change the planted file'
    Remove-Item -LiteralPath $metadata
    # Unknown hardlinks are refused before a write, regardless of equal bytes.
    New-HardLink $alias $file
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory($base, $false, $false)
        Assert (Refuses { [void]$guard.Open($file, $true, $false) }) 'existing hardlink refused'
    } finally { $guard.Dispose() }
    Remove-Item -LiteralPath $alias
    $foreign = Join-Path $base 'foreign-target.bin'
    Bytes $foreign 'foreign bytes must remain unchanged'
    Remove-Item -LiteralPath $file
    New-HardLink $file $foreign
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory($base, $false, $false)
        Assert (Refuses { [void]$guard.Open($file, $true, $false) }) 'a preexisting hardlink to a foreign identity is refused'
    } finally { $guard.Dispose() }
    Assert ([IO.File]::ReadAllText($foreign) -ceq 'foreign bytes must remain unchanged') 'foreign target stays byte-for-byte unchanged'
    Remove-Item -LiteralPath $file
    Bytes $file 'known AMD bytes'
    # Deletion is deferred until every staged write succeeds; cancellation restores the old bytes.
    Bytes $metadata 'original registration'
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory($base, $false, $false)
        [void]$guard.Open($file, $true, $false); [void]$guard.Open($metadata, $true, $false)
        $guard.Delete($file); $guard.WriteText($metadata, 'new registration')
    } finally { $guard.Dispose() }
    Assert ([IO.File]::ReadAllText($metadata) -eq 'original registration' -and (Test-Path $file)) 'uncommitted deletes and record writes roll back'
    $guard = New-Object Bc250.RocmFiles
    try {
        [void]$guard.HoldDirectory($base, $false, $false)
        [void]$guard.Open($file, $true, $false)
        $guard.Delete($file); $guard.Commit()
    } finally { $guard.Dispose() }
    Assert (-not (Test-Path $file)) 'committed deletion removes the retained identity'
    foreach ($bad in @('\\no-contact.invalid\share\venv', '\\?\P:\venv', 'P:\venv:stream', 'P:\venv.\child', 'P:\venv\..\')) {
        Assert (Refuses { [void][Bc250.RocmFiles]::LocalPath($bad) }) "unsafe path refused: $bad"
    }
    Write-Host "test-rocm-files: $checks checks passed"
} finally {
    if (-not $base.StartsWith(([IO.Path]::GetFullPath($Out).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'cleanup outside test output' }
    Remove-Item -LiteralPath $base -Recurse -Force
}
