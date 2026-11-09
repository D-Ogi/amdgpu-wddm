#Requires -Version 7.0
# No PowerShell function has the name of a built-in alias.
#
# PowerShell resolves an alias before a function of the same name, without a warning. The FINAL-VERIFY row of rc3
# (2026-11-09, batch B4) defined 'function Cli' to call bc250kmd_cli.exe; the built-in alias cli is Clear-Item, so
# 'Cli state set ...' ran Clear-Item, never set the DPM cap, and the row printed PASS. Case does not matter: Cli, cli
# and CLI are one name.
#
# The alias names are the fixed list powershell-builtin-aliases.txt beside this file: the built-in aliases of Windows
# PowerShell 5.1 and PowerShell 7 together (both run the scripts of this repository), not the aliases of the machine
# that runs the gate, which a profile or a module can change. That file says how it was made.
#
# ERROR (exit 1):
#   parse   the file does not parse.
#   alias   a function or a filter has the name of a built-in alias.
# Exit 2: no alias list, or no file to examine.
#
# The files are the tracked .ps1 and .psm1 files under tools/ and driver/ of the repository, or the files given on
# the command line. The 'function-alias' gate of quick.ps1 runs it over the repository; its planted controls are in
# test_check_function_alias.py beside it.
#
#   pwsh -NoProfile -File check-function-alias.ps1 <file> [<file> ...]
#   pwsh -NoProfile -File check-function-alias.ps1 -Repo <repo>
param([Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
      [string]$Repo = '',
      [string]$Aliases = '')
$ErrorActionPreference = 'Stop'
$roots = @('tools/', 'driver/')

if (-not $Aliases) { $Aliases = Join-Path $PSScriptRoot 'powershell-builtin-aliases.txt' }
if (-not [IO.File]::Exists($Aliases)) { Write-Host "check-function-alias: no alias list $Aliases"; exit 2 }
$builtin = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in [IO.File]::ReadAllLines($Aliases)) {
    if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
    $fields = $line -split "`t"
    if ($fields.Count -lt 2) { Write-Host "check-function-alias: a line of $Aliases has no tab: '$line'"; exit 2 }
    $builtin[$fields[0]] = $fields[1]
}
if (-not $builtin.Count) { Write-Host "check-function-alias: $Aliases names no alias"; exit 2 }

if (-not $Files -or -not $Files.Count) {
    if (-not $Repo) { $Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path }
    $listed = @(& git -C $Repo ls-files -z -- @($roots | ForEach-Object { $_.TrimEnd('/') }))
    if ($LASTEXITCODE -ne 0) { Write-Host "check-function-alias: git ls-files failed in $Repo"; exit 2 }
    $Files = @(($listed -join '') -split "`0" | Where-Object { $_ -match '\.psm?1$' } | ForEach-Object { Join-Path $Repo $_ })
    if (-not $Files.Count) { Write-Host "check-function-alias: no PowerShell file found under $Repo"; exit 2 }
}

$errorCount = 0
foreach ($file in $Files) {
    $path = [IO.Path]::GetFullPath($file)
    if (-not [IO.File]::Exists($path)) { Write-Host "check-function-alias: no file $file"; exit 2 }
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    foreach ($e in @($errors)) {
        Write-Host ('{0}:{1}: ERROR parse {2}' -f $path, $e.Extent.StartLineNumber, $e.Message)
        $errorCount++
    }
    if (@($errors).Count) { continue }
    # A class method is called as $object.Name(): no alias takes its place, so it is not examined.
    foreach ($f in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $n.Parent -isnot [Management.Automation.Language.FunctionMemberAst] }, $true)) {
        # A scope modifier (function script:Name, global:Name) does not change the name that a call looks up.
        $name = $f.Name -replace '^(?i:script|global|local|private):', ''
        if ($builtin.ContainsKey($name)) {
            Write-Host ('{0}:{1}: ERROR alias {2} {3}: the built-in alias {4} ({5}) runs in its place; rename it' -f $path,
                $f.Extent.StartLineNumber, $(if ($f.IsFilter) { 'filter' } else { 'function' }), $f.Name, $name.ToLowerInvariant(), $builtin[$name])
            $errorCount++
        }
    }
}
Write-Host ('check-function-alias: {0} file(s), {1} alias name(s), {2} error(s)' -f $Files.Count, $builtin.Count, $errorCount)
exit ($errorCount -gt 0 ? 1 : 0)
