# Every variable a script reads must be written somewhere in that script.
#
# This is the check that would have caught the lab twin compiling nothing: the BD-167 branch hoisted the
# control application's source list into $appSources and used it in two compiles, our side kept its own inline
# list in the first compile, and the resolution took the branch's second compile while the assignment was lost.
# csc with no source file is CS5001, and no gate of the release suite runs that build.
#
# PowerShell's own parser answers it exactly: parameters, assignments, foreach and for variables, data and
# scriptblock parameters, and -ErrorVariable/-OutVariable/-PipelineVariable/-Variable targets and the name
# argument of Set-Variable and New-Variable are writes; everything else a VariableExpressionAst names is a
# read. Automatic variables are listed once below.
#
# With -Recipes it reads every build recipe of the repository, which is the set a merge of a build branch
# touches, and the set the 'script-variables' gate of quick.ps1 runs it over. Its planted controls are in
# test_check_unassigned.py beside it.
#
#   pwsh -NoProfile -File check-unassigned.ps1 <file> [<file> ...]
#   pwsh -NoProfile -File check-unassigned.ps1 -Recipes -Repo <repo>
param([Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
      [switch]$Recipes,
      [string]$Repo = '')

if ($Recipes) {
    if (-not $Repo) { $Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path }
    # The recipes: tools\build, every build.ps1 of a component, the installer and the release driver.
    $found = @()
    $found += Get-ChildItem -LiteralPath (Join-Path $Repo 'tools\build') -Filter *.ps1 -File -ErrorAction SilentlyContinue
    $found += Get-ChildItem -LiteralPath $Repo -Recurse -Filter 'build.ps1' -File -ErrorAction SilentlyContinue
    $found += Get-ChildItem -LiteralPath (Join-Path $Repo 'tools\release') -Filter '*build*.ps1' -File -ErrorAction SilentlyContinue
    $found += Get-ChildItem -LiteralPath (Join-Path $Repo 'compute\hip') -Filter 'build-*.ps1' -File -ErrorAction SilentlyContinue
    $Files = @($found | Select-Object -ExpandProperty FullName -Unique | Sort-Object)
    if (-not $Files.Count) { Write-Host "check-unassigned: no recipe found under $Repo"; exit 2 }
}
if (-not $Files -or -not $Files.Count) { Write-Host 'check-unassigned: no file given'; exit 2 }

$automatic = @(
    '_', 'args', 'error', 'false', 'true', 'null', 'host', 'home', 'pwd', 'pid', 'psscriptroot',
    'pscommandpath', 'psitem', 'matches', 'lastexitcode', 'psversiontable', 'myinvocation', 'input',
    'psboundparameters', 'profile', 'executioncontext', 'stacktrace', 'switch', 'this', 'foreach',
    'erroractionpreference', 'progresspreference', 'warningpreference', 'verbosepreference',
    'informationpreference', 'confirmpreference', 'debugpreference', 'psdefaultparametervalues',
    'ofs', 'iscoreclr', 'iswindows', 'islinux', 'ismacos', 'psedition', 'nestedpromptlevel',
    'outputencoding', 'formatenumerationlimit', 'psculture', 'psuiculture', 'shellid', 'pscmdlet',
    'psdebugcontext', 'report', 'ismacosx'
)

$bad = 0
foreach ($file in $Files) {
    $errors = $null
    $tokens = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $file).Path,
        [ref]$tokens, [ref]$errors)
    if ($errors.Count) {
        Write-Host ("{0}: does not parse: {1}" -f $file, $errors[0].Message)
        $bad++
        continue
    }

    $written = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in $automatic) { [void]$written.Add($n) }
    # scope and provider prefixes are a write of the underlying name elsewhere, or not ours at all
    foreach ($n in 'env', 'global', 'script', 'local', 'using', 'private', 'variable') { [void]$written.Add($n) }

    foreach ($p in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.ParameterAst] }, $true)) {
        [void]$written.Add($p.Name.VariablePath.UserPath)
    }
    foreach ($a in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)) {
        foreach ($v in $a.Left.FindAll({ $args[0] -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
            [void]$written.Add($v.VariablePath.UserPath)
        }
    }
    foreach ($f in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.ForEachStatementAst] }, $true)) {
        [void]$written.Add($f.Variable.VariablePath.UserPath)
    }
    foreach ($c in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.CommandAst] }, $true)) {
        $elements = $c.CommandElements
        for ($i = 0; $i -lt $elements.Count - 1; $i++) {
            $e = $elements[$i]
            # -ErrorVariable and its kin, Tee-Object -Variable, Set-Variable/New-Variable -Name: each names a
            # variable the command writes, and the name is a bare string, not a $variable.
            if ($e -is [System.Management.Automation.Language.CommandParameterAst] -and
                $e.ParameterName -match '^(ErrorVariable|OutVariable|WarningVariable|InformationVariable|PipelineVariable|OutBuffer|Variable|Name)$') {
                $next = $elements[$i + 1]
                if ($next -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                    [void]$written.Add($next.Value.TrimStart('+'))
                }
            }
        }
        # Set-Variable x 1 / New-Variable x: the name is the first positional argument.
        $name = $c.GetCommandName()
        if ($name -and $name -match '^(Set-Variable|New-Variable|sv|nv)$' -and $elements.Count -gt 1) {
            $first = $elements[1]
            if ($first -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                [void]$written.Add($first.Value)
            }
        }
    }
    # A data section or a trap gives its own variable; a catch gives $_. Both are covered above or automatic.

    $seen = @{}
    foreach ($v in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
        $name = $v.VariablePath.UserPath
        if ($name -match ':') { $name = $name.Split(':')[0] }
        if ($written.Contains($name)) { continue }
        if (-not $seen.ContainsKey($name)) { $seen[$name] = $v.Extent.StartLineNumber }
    }
    if ($seen.Count) {
        $bad++
        foreach ($name in ($seen.Keys | Sort-Object { $seen[$_] })) {
            Write-Host ("{0}:{1}: `${2} is read and never written" -f $file, $seen[$name], $name)
        }
    }
}
Write-Host ("checked {0} file(s), {1} with a variable that is never written" -f $Files.Count, $bad)
exit ($bad -gt 0 ? 1 : 0)
