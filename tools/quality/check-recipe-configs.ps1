#Requires -Version 7.0
# Every recipe config resolves in both flavours, and every read of a config's key finds the key or looks it up first.
#
# This is the check that would have caught the release flavour failing before it compiled. common.ps1 sets
# Set-StrictMode -Version 3.0, under which reading a key that a parsed JSON object does not have is an error, not
# $null. The three recipe scripts read $recipeConfig.release_options in the release flavour, and no vkd3d config,
# the dxvk per-app config and five of the seven Mesa configs had that key, so their release builds stopped with
# "The property 'release_options' cannot be found on this object" before meson ran. No gate ran that code path; a
# release build of the vkd3d engine found it. The version stamp's read of the ships map had the same trap after the
# compile, for the three configs without one, in either flavour.
#
# Two parts, both under Set-StrictMode -Version 3.0:
#
#   resolve  runs each recipe script's own option resolution, the script's text from its first line through the
#            statement that calls Add-ReproducibleMesonOptions, for every config of its -Config set and of its
#            configs JSON, in the release flavour and with -Lab, and with each value of the script's -Compiler set
#            when it has one (build-mesa.ps1: msvc and clang-cl). Only $PSScriptRoot is replaced, by the script's
#            directory. The source tree is a directory whose meson_options.txt declares amdgpu-wddm-lab-flavour,
#            so the release flavour takes its release path; the run stops before the script checks the source
#            tree, so nothing is configured or compiled.
#   reads    evaluates every $recipeConfig.<key> read of the whole script against every config, the reads after
#            the compile included (the version stamp's ships map), which the resolve part cannot reach. A read is
#            skipped for a config only when a condition before it that reads nothing but the config is false for
#            that config: the if clause around it, or the left side of an -and it is on the right of. Any other
#            condition is taken to be true.
#
# A recipe script is a .ps1 of tools\build that names a *-configs.json. Its planted controls are in
# test_check_recipe_configs.py beside it; the 'recipe-configs' gate of quick.ps1 runs it over the repository.
#
#   pwsh -NoProfile -File check-recipe-configs.ps1 -Out <dir> [-Repo <repo>]
param([Parameter(Mandatory)][string]$Out,
      [string]$Repo = '')

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path }
$buildDir = Join-Path $Repo 'tools\build'
if (-not (Test-Path -LiteralPath $buildDir)) { Write-Host "check-recipe-configs: no $buildDir"; exit 2 }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
# Add-ReproducibleMesonOptions refuses a source or build directory with white space in it, and both are under -Out.
if ($Out -match '\s') { Write-Host "check-recipe-configs: -Out $Out contains white space"; exit 2 }

$source = Join-Path $Out 'source'
New-Item -ItemType Directory -Force $source | Out-Null
Set-Content -LiteralPath (Join-Path $source 'meson_options.txt') -Encoding utf8 `
    -Value "option('amdgpu-wddm-lab-flavour', type : 'boolean', value : false)"
$build = Join-Path $Out 'build'

# One line per problem: the three reads of one key on one line of a script fail alike for a config without the key.
$problems = [System.Collections.Generic.List[string]]::new()
function Add-Problem([string]$Text) {
    if ($script:problems.Contains($Text)) { return }
    $script:problems.Add($Text)
    Write-Host $Text
}

# The value of one expression of a recipe script, with the script's config variable set to one config. The names of
# this function's own variables cannot be the config variable's: PowerShell names are not case sensitive, and the
# recipe scripts' -Config parameter is the reason theirs is $recipeConfig and not $config.
function Get-ConfigValue([string]$__expression, [string]$__variable, $__value) {
    Set-StrictMode -Version 3.0
    Set-Variable -Name $__variable -Value $__value
    return (& ([scriptblock]::Create($__expression)))
}

# The parts of a condition that must all be true: the operands of its -and chain.
function Get-Conjuncts($node) {
    while ($true) {
        if ($node -is [System.Management.Automation.Language.PipelineAst] -and $node.PipelineElements.Count -eq 1 -and
            $node.PipelineElements[0] -is [System.Management.Automation.Language.CommandExpressionAst]) {
            $node = $node.PipelineElements[0].Expression
        } elseif ($node -is [System.Management.Automation.Language.ParenExpressionAst]) {
            $node = $node.Pipeline
        } else { break }
    }
    if ($node -is [System.Management.Automation.Language.BinaryExpressionAst] -and
        $node.Operator -eq [System.Management.Automation.Language.TokenKind]::And) {
        return @(Get-Conjuncts $node.Left) + @(Get-Conjuncts $node.Right)
    }
    return , $node
}

# The conditions that are true wherever $read runs: the condition of each if clause whose body holds it, the left side
# of each -and whose right side holds it, the condition of a ternary whose true branch holds it. A function body
# ends the walk, because a function runs wherever it is called from.
function Get-Guards($read) {
    $guards = @()
    $node = $read
    while ($node.Parent) {
        $parent = $node.Parent
        if ($parent -is [System.Management.Automation.Language.FunctionDefinitionAst]) { break }
        if ($parent -is [System.Management.Automation.Language.BinaryExpressionAst] -and
            $parent.Operator -eq [System.Management.Automation.Language.TokenKind]::And -and $parent.Right -eq $node) {
            $guards += @(Get-Conjuncts $parent.Left)
        } elseif ($parent -is [System.Management.Automation.Language.IfStatementAst]) {
            foreach ($clause in $parent.Clauses) {
                if ($clause.Item2 -eq $node) { $guards += @(Get-Conjuncts $clause.Item1) }
            }
        } elseif ($parent -is [System.Management.Automation.Language.TernaryExpressionAst] -and $parent.IfTrue -eq $node) {
            $guards += @(Get-Conjuncts $parent.Condition)
        }
        $node = $parent
    }
    return $guards
}

# A guard the gate can evaluate reads nothing but the config variable and runs no command.
function Test-ConfigOnly($guard, [string]$variable) {
    if ($guard.Find({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -or
                $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)) { return $false }
    foreach ($v in $guard.FindAll({ param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
        if ($v.VariablePath.UserPath -notin @($variable, 'true', 'false', 'null')) { return $false }
    }
    return $true
}

$scripts = 0
$configsSeen = 0
$resolutions = 0
$readsChecked = 0
$claimed = @{}
$resolved = [ordered]@{}
foreach ($file in @(Get-ChildItem -LiteralPath $buildDir -Filter '*.ps1' -File | Sort-Object Name)) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    $jsonNames = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
                $n.Value -match '^[\w.-]+-configs\.json$' }, $true) | ForEach-Object { $_.Value } | Sort-Object -Unique)
    if (-not $jsonNames.Count) { continue }
    $name = $file.Name
    $scripts++
    if ($errors.Count) { Add-Problem "${name}: does not parse: $($errors[0].Message)"; continue }
    if ($jsonNames.Count -ne 1) { Add-Problem "${name}: names $($jsonNames.Count) configs files ($($jsonNames -join ', ')), not one"; continue }
    $jsonPath = Join-Path $buildDir $jsonNames[0]
    $claimed[$jsonNames[0]] = $true
    if (-not (Test-Path -LiteralPath $jsonPath)) { Add-Problem "${name}: $($jsonNames[0]) does not exist"; continue }
    $configs = (Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json).configs
    $configNames = @($configs.PSObject.Properties | ForEach-Object { $_.Name })

    # The config variable: the one assigned from <configs file>.configs.$Config.
    $assignment = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
            $n.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
            $n.Right.Extent.Text -match '\.configs\.\$Config\b' }, $true)
    if (-not $assignment) { Add-Problem "${name}: reads $($jsonNames[0]) but assigns no variable from its .configs.`$Config"; continue }
    $variable = $assignment.Left.VariablePath.UserPath

    # The -Config set of the script, which may differ from the configs of the file.
    $validateSet = @()
    if ($ast.ParamBlock) {
        foreach ($p in $ast.ParamBlock.Parameters) {
            if ($p.Name.VariablePath.UserPath -ne 'Config') { continue }
            foreach ($a in $p.Attributes) {
                if ($a -is [System.Management.Automation.Language.AttributeAst] -and $a.TypeName.Name -eq 'ValidateSet') {
                    $validateSet += @($a.PositionalArguments | ForEach-Object { $_.Value })
                }
            }
        }
    }
    $all = @(@($configNames) + @($validateSet) | Sort-Object -Unique)
    $configsSeen += $configNames.Count
    # The -Compiler set of a script that has one: every config resolves with each compiler. The record of the default
    # compiler keeps its name ('<script> <config> <flavour>'); the record of another one adds the compiler's name.
    $compilers = @('')
    if ($ast.ParamBlock) {
        foreach ($p in $ast.ParamBlock.Parameters) {
            if ($p.Name.VariablePath.UserPath -ne 'Compiler') { continue }
            $default = if ($p.DefaultValue) { $p.DefaultValue.Extent.Text.Trim("'", '"') } else { '' }
            foreach ($a in $p.Attributes) {
                if ($a -is [System.Management.Automation.Language.AttributeAst] -and $a.TypeName.Name -eq 'ValidateSet') {
                    $compilers = @('') + @($a.PositionalArguments | ForEach-Object { $_.Value } | Where-Object { $_ -ne $default })
                }
            }
        }
    }

    # resolve: the script's own text up to the end of its option resolution.
    $anchor = $null
    foreach ($s in $(if ($ast.EndBlock) { $ast.EndBlock.Statements } else { @() })) {
        if ($s -is [System.Management.Automation.Language.AssignmentStatementAst] -and
            $s.Right.Find({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and
                    $n.GetCommandName() -eq 'Add-ReproducibleMesonOptions' }, $true)) { $anchor = $s }
    }
    if (-not $anchor) {
        Add-Problem "${name}: no statement of its body assigns the result of Add-ReproducibleMesonOptions, so the gate does not know where its option resolution ends"
    } else {
        $optionsVariable = $anchor.Left.VariablePath.UserPath
        $flavourAssignment = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                $n.Right.Find({ param($m) $m -is [System.Management.Automation.Language.CommandAst] -and
                        $m.GetCommandName() -eq 'Get-AmdgpuWddmFlavour' }, $true) }, $true)
        $flavourVariable = if ($flavourAssignment) { $flavourAssignment.Left.VariablePath.UserPath } else { '' }
        $text = $ast.Extent.StartScriptPosition.GetFullScript().Substring(0, $anchor.Extent.EndOffset)
        $quotedDir = "'" + $buildDir.Replace("'", "''") + "'"
        foreach ($t in @($tokens | Where-Object { $_.Kind -eq 'Variable' -and $_.Name -eq 'PSScriptRoot' -and
                    $_.Extent.EndOffset -le $anchor.Extent.EndOffset } | Sort-Object { $_.Extent.StartOffset } -Descending)) {
            $text = $text.Substring(0, $t.Extent.StartOffset) + $quotedDir + $text.Substring($t.Extent.EndOffset)
        }
        $emit = "[pscustomobject]@{ options = @(`$$optionsVariable); flavour = " +
            $(if ($flavourVariable) { "`$$flavourVariable" } else { '$null' }) + ' }'
        $prefix = Join-Path $Out ($file.BaseName + '.resolve.ps1')
        Set-Content -LiteralPath $prefix -Encoding utf8 -Value ($text + [Environment]::NewLine + $emit)
        foreach ($config in $all) {
            foreach ($compiler in $compilers) {
                foreach ($flavour in 'release', 'lab') {
                    $resolutions++
                    $arguments = @{ Config = $config; Source = $source; Build = $build }
                    if ($flavour -eq 'lab') { $arguments.Lab = $true }
                    if ($compiler) { $arguments.Compiler = $compiler }
                    $label = if ($compiler) { "$flavour, $compiler" } else { $flavour }
                    try {
                        $result = @(& $prefix @arguments)
                        $last = if ($result.Count) { $result[-1] } else { $null }
                        if (-not $last -or -not ($last.PSObject.Properties.Name -contains 'options')) {
                            Add-Problem "${name}: -Config $config ($label) resolved to nothing"
                            continue
                        }
                        if ($flavourVariable -and $last.flavour -ne $flavour) {
                            Add-Problem "${name}: -Config $config ($label) resolved the $($last.flavour) flavour instead"
                            continue
                        }
                        $resolved[("$name $config $flavour" + $(if ($compiler) { " $compiler" } else { '' }))] = @($last.options)
                    } catch {
                        # A line of the script or of common.ps1 is named; a parameter the script refuses is the script's.
                        # The record the catch receives is positioned at the call above; the exception's own record
                        # is positioned where the script failed.
                        $info = $_.InvocationInfo
                        if ($_.Exception -is [System.Management.Automation.RuntimeException] -and $_.Exception.ErrorRecord -and
                            $_.Exception.ErrorRecord.InvocationInfo) { $info = $_.Exception.ErrorRecord.InvocationInfo }
                        $where = if ($info) { $info.ScriptName } else { '' }
                        $line = if ($info) { $info.ScriptLineNumber } else { 0 }
                        $problem = "-Config $config ($label) does not resolve"
                        if ($where -eq $prefix -and $line -gt 0) { $problem = "${name}:${line}: $problem" }
                        elseif ($where -and $where -ne $prefix -and $where -ne $PSCommandPath) {
                            $problem = "${name}: $problem in $(Split-Path -Leaf $where):$line"
                        } else { $problem = "${name}: $problem" }
                        Add-Problem "${problem}: $($_.Exception.Message)"
                    }
                }
            }
        }
    }

    # reads: every $<variable>.<key> of the whole script, against every config of the file.
    $reads = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.MemberExpressionAst] -and
                $n -isnot [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
                $n.Expression -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $n.Expression.VariablePath.UserPath -eq $variable -and
                $n.Member -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
                $n.Member.Value -ne 'PSObject' }, $true))
    foreach ($read in $reads) {
        $guards = @(Get-Guards $read | Where-Object { Test-ConfigOnly $_ $variable })
        foreach ($config in $configNames) {
            $readsChecked++
            $skipped = $false
            foreach ($g in $guards) {
                try { if (-not (Get-ConfigValue $g.Extent.Text $variable $configs.$config)) { $skipped = $true; break } } catch { }
            }
            if ($skipped) { continue }
            try { $null = Get-ConfigValue $read.Extent.Text $variable $configs.$config } catch {
                Add-Problem ("{0}:{1}: {2} with -Config {3}: {4}" -f $name, $read.Extent.StartLineNumber, $read.Extent.Text,
                    $config, $_.Exception.Message)
            }
        }
    }
}

foreach ($json in @(Get-ChildItem -LiteralPath $buildDir -Filter '*-configs.json' -File | Sort-Object Name)) {
    if (-not $claimed.ContainsKey($json.Name)) { Add-Problem "$($json.Name): no recipe script of tools\build reads it" }
}
if (-not $scripts) { Write-Host "check-recipe-configs: no recipe script under $buildDir"; exit 2 }
$resolved | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'resolved.json') -Encoding utf8
Write-Host ("checked {0} recipe script(s), {1} config(s), {2} resolution(s), {3} config read(s); {4} problem(s)" -f
    $scripts, $configsSeen, $resolutions, $readsChecked, $problems.Count)
exit ($problems.Count -gt 0 ? 1 : 0)
