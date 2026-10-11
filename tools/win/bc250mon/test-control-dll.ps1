# The monitor calls bc250control.dll by name, and build.ps1 ships the -ControlDll it is given beside the exe. A DLL
# that lacks an entry point the source imports is found only at run time, as an EntryPointNotFoundException in one
# panel: the lab overlay of 2026-10-11 was built against the lab's DLL of 2026-10-06 and its fan row said
# "no Bc250Hwmon". This check reads the DLL's PE export table from the file (the DLL is never loaded) and refuses
# the build when an import of a [DllImport("bc250control.dll")] in src\*.cs is not exported, or the DLL is not x64.
param([Parameter(Mandatory)][string]$ControlDll)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Get-PeExport([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    function U16([long]$o) { if ($o -lt 0 -or $o + 2 -gt $b.Length) { throw "${Path}: offset $o is outside the file" }; [BitConverter]::ToUInt16($b, $o) }
    function U32([long]$o) { if ($o -lt 0 -or $o + 4 -gt $b.Length) { throw "${Path}: offset $o is outside the file" }; [BitConverter]::ToUInt32($b, $o) }
    if ((U16 0) -ne 0x5A4D) { throw "${Path}: not a PE file (no MZ header)" }
    $pe = U32 0x3C
    if ((U32 $pe) -ne 0x4550) { throw "${Path}: not a PE file (no PE signature)" }
    $machine = U16 ($pe + 4)
    $sections = U16 ($pe + 6)
    $opt = $pe + 24
    $sectionTable = $opt + (U16 ($pe + 20))
    switch (U16 $opt) {
        0x20B { $dirCount = U32 ($opt + 108); $exportDir = $opt + 112 }
        0x10B { $dirCount = U32 ($opt + 92); $exportDir = $opt + 96 }
        default { throw "${Path}: unknown optional header magic" }
    }
    $names = New-Object System.Collections.Generic.List[string]
    $exportRva = if ($dirCount -gt 0) { U32 $exportDir } else { 0 }
    if ($exportRva -ne 0) {
        $toFile = {
            param([long]$rva)
            for ($i = 0; $i -lt $sections; $i++) {
                $s = $sectionTable + 40 * $i
                $va = U32 ($s + 12)
                $span = [Math]::Max([long](U32 ($s + 8)), [long](U32 ($s + 16)))
                if ($rva -ge $va -and $rva -lt $va + $span) { return [long]($rva - $va + (U32 ($s + 20))) }
            }
            throw "${Path}: RVA $rva is in no section"
        }
        $dir = & $toFile $exportRva
        $count = U32 ($dir + 24)
        $nameTable = & $toFile (U32 ($dir + 32))
        for ($i = 0; $i -lt $count; $i++) {
            $p = & $toFile (U32 ($nameTable + 4 * $i))
            $q = $p
            while ($q -lt $b.Length -and $b[$q] -ne 0) { $q++ }
            $names.Add([Text.Encoding]::ASCII.GetString($b, $p, $q - $p))
        }
    }
    [pscustomobject]@{ Machine = $machine; Names = $names }
}

if (-not (Test-Path -LiteralPath $ControlDll -PathType Leaf)) { throw "Build bc250kmd_cli first and supply its bc250control.dll" }
$sha = (Get-FileHash -LiteralPath $ControlDll -Algorithm SHA256).Hash
$dll = Get-PeExport $ControlDll
if ($dll.Machine -ne 0x8664) { throw ('{0} (sha256 {1}) is not an x64 DLL (machine 0x{2:X4}); bc250mon.exe is x64' -f $ControlDll, $sha, $dll.Machine) }

# Every [DllImport("bc250control.dll", ...)] with the method it declares. An attribute that the pattern cannot pair
# with a method fails the build, so a changed declaration style cannot drop an import from the check unnoticed.
$attribute = '\[\s*DllImport\s*\(\s*"bc250control(?:\.dll)?"'
$declaration = $attribute + '(?<args>[^\]]*)\]\s*(?:\[[^\]]*\]\s*)*(?:(?:public|private|internal|protected|static|extern|unsafe)\s+)+[\w.]+(?:\[\])?\*?\s+(?<name>\w+)\s*\('
$imports = New-Object System.Collections.Generic.List[object]
foreach ($file in Get-ChildItem -Path "$here\src\*.cs") {
    $text = [IO.File]::ReadAllText($file.FullName)
    $found = [regex]::Matches($text, $declaration, 'IgnoreCase')
    $attributes = [regex]::Matches($text, $attribute, 'IgnoreCase').Count
    if ($found.Count -ne $attributes) {
        throw "$($file.Name): $attributes DllImport(""bc250control.dll"") attributes, but only $($found.Count) declarations this check can read"
    }
    foreach ($m in $found) {
        $entry = [regex]::Match($m.Groups['args'].Value, 'EntryPoint\s*=\s*"(?<ep>[^"]+)"')
        $name = if ($entry.Success) { $entry.Groups['ep'].Value } else { $m.Groups['name'].Value }
        # Without ExactSpelling the runtime also tries the A and W forms of the name.
        $exact = $m.Groups['args'].Value -match 'ExactSpelling\s*=\s*true'
        $accepted = if ($exact) { @($name) } else { @($name, "${name}A", "${name}W") }
        $imports.Add([pscustomobject]@{ Name = $name; File = $file.Name; Accepted = $accepted })
    }
}
if ($imports.Count -eq 0) { throw 'No DllImport("bc250control.dll") found in src\*.cs: the check no longer reads the source' }

$exported = [System.Collections.Generic.HashSet[string]]::new([string[]]$dll.Names)
$missing = @($imports | Where-Object { -not ($_.Accepted | Where-Object { $exported.Contains($_) }) })
if ($missing.Count) {
    throw ('{0} (sha256 {1}) does not export {2}. Build bc250kmd_cli from this tree and pass its bc250control.dll' -f
        $ControlDll, $sha, (($missing | ForEach-Object { "$($_.Name) ($($_.File))" }) -join ', '))
}
Write-Host ('  control DLL: {0} imports of bc250control.dll checked, all exported by {1} (sha256 {2}, x64, {3} exports)' -f
    $imports.Count, $ControlDll, $sha.Substring(0, 8), $dll.Names.Count)
