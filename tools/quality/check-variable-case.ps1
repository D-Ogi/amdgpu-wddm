#Requires -Version 7.0
# One variable has one spelling in each scope that writes it.
#
# PowerShell variable names ignore case: $Report and $report are one variable. This is the check that would have
# caught the release build set losing its report table. tools\release\build-release-set.ps1 got the parameter
# [string]$Report, and the line $report = [ordered]@{...} of the same scope then wrote the table into that typed
# parameter: the table became a string, and 4 of the 32 release-build-set tests failed while a package was cut.
# The same defect in a lab kit script: foreach ($l in ...) overwrote $L of the same scope (2026-10-24).
#
# A scope is the top level of a file, or the body of a function, a filter, a class method or a script block. The
# parameters of a function or a script block belong to its body. Five kinds of script block do not make a scope,
# because PowerShell runs them in the scope of their caller: a script block that is dot-sourced (. { }), and a
# script block given to ForEach-Object, Where-Object (and their aliases), Measure-Command, or the .ForEach() and
# .Where() methods. A write with $script: goes to the top level of its file, a write with $global: goes to one
# global scope of its file, a write with $local: or $private: stays in its own scope. $env:, $using: and the other
# drive names are not PowerShell variables of this file and are not examined.
#
# A dot-sourced file (. "$PSScriptRoot\lib.ps1", . (Join-Path $PSScriptRoot 'lib.ps1')) runs in the scope that
# dot-sources it, so the names that the top level of that file writes are writes of that scope at the line of the
# dot-source. A dot-source with any other argument is not followed.
#
# A write is: a parameter, the target of an assignment (also [int]$x = ..., $a, $b = ..., $x += ..., $x++), the
# variable of a foreach statement, a [ref]$x argument, and the name given to -OutVariable, -ErrorVariable,
# -WarningVariable, -InformationVariable, -PipelineVariable, Tee-Object -Variable and Set-Variable/New-Variable.
# Every other use is a read. $x.Name = ... and $x[0] = ... change the object, not the variable: they are reads.
#
# ERROR (exit 1):
#   parse     the file does not parse.
#   case      one scope writes one variable under two spellings that differ only in case. When one of the writes
#             is a typed parameter or a typed assignment ([string]$Report), the message says so: then the other
#             write converts its value to that type.
#   hardlink  New-Item with -Target (a hard link, a junction or a symbolic link): Windows PowerShell 5.1 and
#             PowerShell 7.4 read [ ] in -Target as a wildcard and do not find a path that has them. Use
#             CreateHardLinkW for a hard link and mklink /J for a junction: both take the path as it is.
# Not reported: a read under another spelling than the writes of the variable. PowerShell gives it the same value,
# so it cannot change what the script does; -Reads lists these as WARNING lines and does not change the exit code.
#
# The files are the tracked .ps1 and .psm1 files of the repository, except the records (see $records below), or the
# files given on the command line. The 'variable-case' gate of quick.ps1 runs it over the repository; its planted
# controls are in test_check_variable_case.py beside it.
#
#   pwsh -NoProfile -File check-variable-case.ps1 <file> [<file> ...] [-Reads]
#   pwsh -NoProfile -File check-variable-case.ps1 -Repo <repo> [-Reads]
param([Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
      [string]$Repo = '',
      [switch]$Reads)
$ErrorActionPreference = 'Stop'
$clock = [Diagnostics.Stopwatch]::StartNew()

# Records are history: the scripts under evidence\ are what a measurement ran (evidence\ is immutable, CLAUDE.md
# rule 6), and the scripts under experiments\ are what an experiment ran. A fix of them would make the record lie
# about what ran. Each record directory is named here by its full path from the repository root; a new tool
# directory is examined until somebody adds it to this list.
$records = @('evidence/', 'experiments/')

if (-not $Files -or -not $Files.Count) {
    if (-not $Repo) { $Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path }
    $listed = @(& git -C $Repo ls-files -z)
    if ($LASTEXITCODE -ne 0) { Write-Host "check-variable-case: git ls-files failed in $Repo"; exit 2 }
    $Files = @(($listed -join '') -split "`0" | Where-Object {
            $relative = $_
            $relative -match '\.psm?1$' -and -not @($records | Where-Object { $relative.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count
        } | ForEach-Object { Join-Path $Repo $_ })
    if (-not $Files.Count) { Write-Host "check-variable-case: no PowerShell file found under $Repo"; exit 2 }
}

$sameScopeCommands = [Collections.Generic.HashSet[string]]::new([string[]]@(
        'ForEach-Object', '%', 'foreach', 'Where-Object', '?', 'where', 'Measure-Command'), [StringComparer]::OrdinalIgnoreCase)
$outParameters = '^(ErrorVariable|ev|OutVariable|ov|WarningVariable|wv|InformationVariable|iv|PipelineVariable|pv|Variable|Name)$'
$parsed = @{}

function Read-Ast([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($parsed.ContainsKey($full)) { return $parsed[$full] }
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($full, [ref]$tokens, [ref]$errors)
    $parsed[$full] = @{ ast = $ast; errors = @($errors) }
    return $parsed[$full]
}

# The script block whose scope a node is in. The body of a function is its scope, also for its parameters, which
# are children of the function and not of its body.
function Get-Scope($Node) {
    for ($n = $Node.Parent; $n; $n = $n.Parent) {
        if ($n -is [Management.Automation.Language.FunctionDefinitionAst]) { return $n.Body }
        if ($n -is [Management.Automation.Language.ScriptBlockAst] -and (-not $n.Parent -or -not (Test-SameScope $n))) { return $n }
    }
    return $null
}

# True when PowerShell runs this script block in the scope of its caller.
function Test-SameScope($Block) {
    $expression = $Block.Parent
    if ($expression -isnot [Management.Automation.Language.ScriptBlockExpressionAst]) { return $false }
    $holder = $expression.Parent
    if ($holder -is [Management.Automation.Language.CommandParameterAst]) { $holder = $holder.Parent }
    if ($holder -is [Management.Automation.Language.CommandAst]) {
        if ($holder.InvocationOperator -eq [Management.Automation.Language.TokenKind]::Dot -and
            $holder.CommandElements[0] -eq $expression) { return $true }
        $name = $holder.GetCommandName()
        return [bool]($name -and $sameScopeCommands.Contains($name))
    }
    if ($holder -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $holder.Member -is [Management.Automation.Language.StringConstantExpressionAst]) {
        return $holder.Member.Value -in 'ForEach', 'Where'
    }
    return $false
}

function Get-ScopeName($Scope, $Root) {
    if ($Scope -eq $Root) { return 'the top level of the file' }
    $owner = $Scope.Parent
    if ($owner -is [Management.Automation.Language.FunctionDefinitionAst]) {
        return ('{0} {1}' -f $(if ($owner.IsFilter) { 'filter' } else { 'function' }), $owner.Name)
    }
    if ($owner -is [Management.Automation.Language.FunctionMemberAst]) { return "method $($owner.Name)" }
    return "the script block at line $($Scope.Extent.StartLineNumber)"
}

# The file a dot-source names, when its argument is a literal path or a path under $PSScriptRoot.
function Resolve-DotSource($Command, [string]$Directory) {
    if ($Command.CommandElements.Count -lt 1) { return $null }
    $text = $Command.CommandElements[0].Extent.Text.Trim()
    if ($text -match '^\(\s*Join-Path\s+\$PSScriptRoot\s+[''"]([^''"$]+)[''"]\s*\)$') { $relative = $Matches[1] }
    elseif ($text -match '^[''"]?\$PSScriptRoot[\\/]([^''"$]+)[''"]?$') { $relative = $Matches[1] }
    elseif ($text -match '^[''"]?([^''"$(){}]+\.ps1)[''"]?$') { $relative = $Matches[1] }
    else { return $null }
    $path = [IO.Path]::GetFullPath((Join-Path $Directory $relative))
    if ([IO.File]::Exists($path)) { return $path }
    return $null
}

# The names that the top level of a file writes, for the scopes that dot-source it. A file that is being read
# gives no names, which stops a cycle of dot-sources.
$topWrites = @{}
$reading = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Get-TopWrites([string]$Path) {
    if ($topWrites.ContainsKey($Path)) { return $topWrites[$Path] }
    if (-not $reading.Add($Path)) { return @() }
    $entry = Read-Ast $Path
    $names = @()
    if (-not $entry.errors.Count) {
        $root = $entry.ast
        foreach ($w in @(Get-Writes $root $Path)) {
            if ($w.scope -eq $root -and $w.kind -ne 'read') { $names += $w.name }
        }
    }
    [void]$reading.Remove($Path)
    $topWrites[$Path] = $names
    return $names
}

$typeConstraint = [Management.Automation.Language.TypeConstraintAst]

# Every write of a file: name as spelled, scope, line, and the type when the write is a typed parameter or a typed
# assignment.
function Get-Writes($Root, [string]$Path) {
    $directory = Split-Path -Parent $Path
    # The key of the global scope of this file: its extent, which no script block uses as a key.
    $globalScope = $Root.Extent
    $result = [Collections.Generic.List[object]]::new()
    $nodes = $Root.FindAll({ param($n) $n -is [Management.Automation.Language.VariableExpressionAst] -or
            $n -is [Management.Automation.Language.CommandAst] }, $true)
    foreach ($node in $nodes) {
        if ($node -is [Management.Automation.Language.CommandAst]) {
            $elements = $node.CommandElements
            for ($i = 0; $i -lt $elements.Count - 1; $i++) {
                $e = $elements[$i]
                if ($e -is [Management.Automation.Language.CommandParameterAst] -and $e.ParameterName -match $outParameters -and
                    $elements[$i + 1] -is [Management.Automation.Language.StringConstantExpressionAst]) {
                    $commandName = $node.GetCommandName()
                    # -Name is a variable only for the variable commands; -Variable only for Tee-Object.
                    if ($e.ParameterName -eq 'Name' -and $commandName -notmatch '^(Set-Variable|New-Variable|sv|nv)$') { continue }
                    if ($e.ParameterName -eq 'Variable' -and $commandName -ne 'Tee-Object') { continue }
                    $result.Add(@{ name = $elements[$i + 1].Value.TrimStart('+'); scope = (Get-Scope $node); line = $e.Extent.StartLineNumber; type = $null; kind = 'write' })
                }
            }
            $commandName = $node.GetCommandName()
            if ($commandName -match '^(Set-Variable|New-Variable|sv|nv)$' -and $elements.Count -gt 1 -and
                $elements[1] -is [Management.Automation.Language.StringConstantExpressionAst]) {
                $result.Add(@{ name = $elements[1].Value; scope = (Get-Scope $node); line = $node.Extent.StartLineNumber; type = $null; kind = 'write' })
            }
            if ($node.InvocationOperator -eq [Management.Automation.Language.TokenKind]::Dot) {
                $target = Resolve-DotSource $node $directory
                if ($target) {
                    $scope = Get-Scope $node
                    foreach ($n in @(Get-TopWrites $target)) {
                        $result.Add(@{ name = $n; scope = $scope; line = $node.Extent.StartLineNumber; type = $null; kind = 'write'; via = (Split-Path -Leaf $target) })
                    }
                }
            }
            continue
        }
        $vp = $node.VariablePath
        if ($vp.IsDriveQualified -or $node.Parent -is [Management.Automation.Language.UsingExpressionAst]) { continue }
        $name = $vp.UserPath
        if ($name -match ':') { $name = ($name -split ':', 2)[1] }
        if (-not $name -or $name -eq 'null') { continue }
        $parent = $node.Parent
        $kind = 'read'
        $type = $null
        if ($parent -is [Management.Automation.Language.ParameterAst]) {
            $kind = 'parameter'
            $constraint = @($parent.Attributes | Where-Object { $_ -is $typeConstraint })
            if ($constraint.Count) { $type = $constraint[0].TypeName.FullName }
        }
        elseif ($parent -is [Management.Automation.Language.AssignmentStatementAst] -and $parent.Left -eq $node) { $kind = 'write' }
        elseif ($parent -is [Management.Automation.Language.ForEachStatementAst] -and $parent.Variable -eq $node) { $kind = 'write' }
        elseif ($parent -is [Management.Automation.Language.UnaryExpressionAst] -and
            $parent.TokenKind -in 'PlusPlus', 'MinusMinus', 'PostfixPlusPlus', 'PostfixMinusMinus') { $kind = 'write' }
        elseif ($parent -is [Management.Automation.Language.ConvertExpressionAst]) {
            $outer = $parent.Parent
            if ($parent.Type.TypeName.Name -eq 'ref') { $kind = 'write' }
            elseif ($outer -is [Management.Automation.Language.AssignmentStatementAst] -and $outer.Left -eq $parent) {
                $kind = 'write'
                $type = $parent.Type.TypeName.FullName
            }
        }
        elseif ($parent -is [Management.Automation.Language.ArrayLiteralAst] -and
            $parent.Parent -is [Management.Automation.Language.AssignmentStatementAst] -and $parent.Parent.Left -eq $parent) { $kind = 'write' }
        if ($vp.IsScript) { $scope = $Root }
        elseif ($vp.IsGlobal) { $scope = $globalScope }
        else { $scope = Get-Scope $node }
        $result.Add(@{ name = $name; scope = $scope; line = $node.Extent.StartLineNumber; type = $type; kind = $kind })
    }
    return $result
}

$errorCount = 0
$warningCount = 0
foreach ($file in $Files) {
    $path = [IO.Path]::GetFullPath($file)
    if (-not [IO.File]::Exists($path)) { Write-Host "check-variable-case: no file $file"; exit 2 }
    $entry = Read-Ast $path
    foreach ($e in $entry.errors) {
        Write-Host ('{0}:{1}: ERROR parse {2}' -f $path, $e.Extent.StartLineNumber, $e.Message)
        $errorCount++
    }
    if ($entry.errors.Count) { continue }
    $root = $entry.ast
    [void]$reading.Add($path)
    $uses = Get-Writes $root $path
    [void]$reading.Remove($path)

    # Per scope and lower-case name: each spelling with its first write and its first read.
    $byScope = [Collections.Generic.Dictionary[object, hashtable]]::new()
    foreach ($u in $uses) {
        if (-not $byScope.ContainsKey($u.scope)) { $byScope[$u.scope] = @{} }
        $names = $byScope[$u.scope]
        $key = $u.name.ToLowerInvariant()
        # The inner table compares keys by ordinal: a PowerShell @{} ignores case and would merge the spellings.
        if (-not $names.ContainsKey($key)) { $names[$key] = [Collections.Hashtable]::new([StringComparer]::Ordinal) }
        if (-not $names[$key].ContainsKey($u.name)) { $names[$key][$u.name] = @{ write = $null; read = $null } }
        $slot = $names[$key][$u.name]
        if ($u.kind -eq 'read') { if (-not $slot.read) { $slot.read = $u } }
        elseif (-not $slot.write -or ($u.type -and -not $slot.write.type)) { $slot.write = $u }
    }
    foreach ($scope in $byScope.Keys) {
        $where = if ($scope -is [Management.Automation.Language.IScriptExtent]) { 'the global scope' } else { Get-ScopeName $scope $root }
        # get_Keys() and get_Count(), not .Keys and .Count: on a hashtable a key wins over a property, and these
        # tables have a key for every variable name, $keys and $count included.
        foreach ($key in @($byScope[$scope].get_Keys())) {
            $spellings = $byScope[$scope][$key]
            if ($spellings.get_Count() -lt 2) { continue }
            $written = @($spellings.GetEnumerator() | Where-Object { $_.Value.write } | Sort-Object { $_.Value.write.line })
            if ($written.Count -ge 2) {
                $list = ($written | ForEach-Object {
                        $w = $_.Value.write
                        $what = switch ($w.kind) { 'parameter' { 'parameter' } default { 'write' } }
                        if ($w.type) { $what = "[$($w.type)] $what" }
                        if ($w.via) { $what = "$what by $($w.via)" }
                        '${0} ({1}, line {2})' -f $_.Key, $what, $w.line
                    }) -join ', '
                $typed = @($written | Where-Object { $_.Value.write.type })
                $note = if ($typed.Count) { "; the other spelling writes into the typed variable `$$($typed[0].Key)" } else { '' }
                Write-Host ('{0}:{1}: ERROR case one variable written as {2} in {3}{4}' -f $path, $written[0].Value.write.line, $list, $where, $note)
                $errorCount++
            }
            elseif ($Reads) {
                $list = ($spellings.GetEnumerator() | ForEach-Object {
                        $s = $_.Value
                        $first = if ($s.write) { $s.write } else { $s.read }
                        '${0} (line {1})' -f $_.Key, $first.line
                    }) -join ', '
                Write-Host ('{0}: WARNING case one variable read as {1} in {2}' -f $path, $list, $where)
                $warningCount++
            }
        }
    }

    foreach ($c in @($root.FindAll({ param($n) $n -is [Management.Automation.Language.CommandAst] }, $true))) {
        if ($c.GetCommandName() -ne 'New-Item') { continue }
        foreach ($el in $c.CommandElements) {
            if ($el -is [Management.Automation.Language.CommandParameterAst] -and $el.ParameterName.Length -ge 2 -and
                'target'.StartsWith($el.ParameterName.ToLowerInvariant())) {
                Write-Host ('{0}:{1}: ERROR hardlink New-Item -{2}: PowerShell reads [ ] in it as a wildcard (use CreateHardLinkW or mklink /J)' -f $path, $el.Extent.StartLineNumber, $el.ParameterName)
                $errorCount++
            }
        }
    }
}
$summary = 'check-variable-case: {0} file(s), {1} error(s)' -f $Files.Count, $errorCount
if ($Reads) { $summary += ", $warningCount warning(s)" }
Write-Host ('{0}, {1:N1} s' -f $summary, $clock.Elapsed.TotalSeconds)
exit ($errorCount -gt 0 ? 1 : 0)
