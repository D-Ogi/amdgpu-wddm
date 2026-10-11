# Check that every import of every binary in a staging tree resolves.
#
# A zip that misses one DLL fails on the tester's machine with a dialog and no log line, so this
# runs before the zip is made. A name resolves when a file of that name sits next to the binary,
# in the tree's lib/ollama directory, or in the list of libraries Windows itself provides.
#
# Exit code 0: every import resolves. 1: at least one does not, and each one is named.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$ReadObj
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $ReadObj) { $ReadObj = 'P:/BC-250/toolchain/llvm-23.1.2-win64/bin/llvm-readobj.exe' }
if (-not (Test-Path $ReadObj)) { throw "llvm-readobj is missing: $ReadObj" }
$Root = (Resolve-Path $Root).Path

# Libraries Windows provides. Anything else must travel in the zip.
$system = @(
    'kernel32.dll', 'kernelbase.dll', 'advapi32.dll', 'user32.dll', 'gdi32.dll', 'shell32.dll',
    'shlwapi.dll', 'ole32.dll', 'oleaut32.dll', 'combase.dll', 'ws2_32.dll', 'crypt32.dll',
    'bcrypt.dll', 'ncrypt.dll', 'secur32.dll', 'psapi.dll', 'version.dll', 'winmm.dll',
    'setupapi.dll', 'cfgmgr32.dll', 'powrprof.dll', 'dxgi.dll', 'd3d12.dll', 'dxcore.dll',
    'vulkan-1.dll', 'opengl32.dll', 'rpcrt4.dll', 'userenv.dll', 'netapi32.dll', 'iphlpapi.dll',
    'dbghelp.dll', 'imagehlp.dll', 'winhttp.dll', 'wininet.dll', 'mswsock.dll', 'ntdll.dll',
    'api-ms-win-core-synch-l1-2-0.dll', 'msvcrt.dll',
    # The Universal C Runtime and the Visual C++ redistributable, both part of Windows 11.
    'ucrtbase.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'concrt140.dll'
) | ForEach-Object { $_.ToLower() }

$binaries = Get-ChildItem $Root -Recurse -Include '*.exe', '*.dll' -File
if (-not $binaries) { throw "no binaries found under $Root" }

$available = @{}
foreach ($file in $binaries) { $available[$file.Name.ToLower()] = $file.FullName }

$missing = @()
foreach ($binary in $binaries) {
    $text = & $ReadObj --coff-imports $binary.FullName 2>&1 | Out-String
    foreach ($match in [regex]::Matches($text, '(?m)^\s*Name:\s*(\S+\.dll)\s*$')) {
        $name = $match.Groups[1].Value.ToLower()
        if ($system -contains $name) { continue }
        if ($name -like 'api-ms-win-*') { continue }
        if ($name -like 'ext-ms-win-*') { continue }
        if ($available.ContainsKey($name)) { continue }
        $missing += [pscustomobject]@{
            binary = $binary.FullName.Substring($Root.Length).TrimStart('\', '/')
            import = $name
        }
    }
}

"checked $($binaries.Count) binaries under $Root"
if ($missing.Count -gt 0) {
    $missing | Sort-Object binary, import | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "$($missing.Count) unresolved imports"
    exit 1
}
'every import resolves inside the tree or in Windows itself'
exit 0
