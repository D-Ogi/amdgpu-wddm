# Source-only BD-296 gate. Execute the actual policy-removal command with memory-only removal functions, and
# its actual packaged dry-run assertion. Never run the uninstaller, open a registry key or require a package.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'))
$ErrorActionPreference = 'Stop'
$script:failed = 0
$script:checks = 0
function Assert-Test([bool]$Ok, [string]$Name) {
    $script:checks++
    if ($Ok) { "PASS $Name" } else { "FAIL $Name"; $script:failed++ }
}
function Parse-Source([string]$Path) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Cannot parse $Path : $($errors[0].Message)" }
    return $ast
}
function One($Nodes, [string]$Name) {
    $items = @($Nodes)
    if ($items.Count -ne 1) { throw "Expected one $Name, found $($items.Count)" }
    return $items[0]
}
$uninstall = Parse-Source (Join-Path $Installer 'uninstall.ps1')
$common = Parse-Source (Join-Path $Installer 'common.ps1')
$dryrun = Parse-Source (Join-Path $PSScriptRoot 'test-dryrun.ps1')
$change = One ($common.FindAll({ param($n)
    $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-Change'
}, $true)) 'Invoke-Change function'
$policy = One ($uninstall.FindAll({ param($n)
    $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Invoke-Change' -and
    $n.CommandElements[1].Extent.Text.Contains('router policy, application profile, release record')
}, $true)) 'policy-removal command'
$oracle = One ($dryrun.FindAll({ param($n)
    $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Check' -and
    $n.Extent.Text.Contains('the policy keys of both registry views, the driver copy of DwmForceCpu')
}, $true)) 'packaged policy assertion'
$setup = foreach ($name in 'softwareKeyWow', 'userKey') {
    $assignment = One ($uninstall.FindAll({ param($n)
        $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq ('$' + $name)
    }, $true)) "$name assignment"
    $assignment.Extent.Text
}
# Validate every extracted fragment before evaluation, including nested expressions. Registry commands are mocked,
# but .NET methods, module-qualified commands or changes to the callbacks must not bypass those mocks.
function Assert-Fragment($Ast, [string]$Kind) {
    $commands = switch ($Kind) {
        'common' { @('Write-Host', 'Write-Info') }
        'policy' { @('Invoke-Change', 'Remove-ItemProperty', 'Remove-Item') }
        'oracle' { @('Check') }
        'setup' { @() }
        default { throw "Unknown fragment $Kind" }
    }
    $writes = switch ($Kind) {
        'common' { @('$script:CurrentStep', '$script:Mutated') }
        'setup' { @('$softwareKeyWow', '$userKey') }
        default { @() }
    }
    foreach ($node in $Ast.FindAll({ param($n) $true }, $true)) {
        if ($node -is [Management.Automation.Language.TypeExpressionAst] -or
            $node -is [Management.Automation.Language.InvokeMemberExpressionAst]) {
            throw "Unexpected type or method invocation in $Kind"
        }
        if ($node -is [Management.Automation.Language.MemberExpressionAst] -and
            -not ($Kind -eq 'oracle' -and $node.Extent.Text -ceq '$r.text')) {
            throw "Unexpected member access in $Kind"
        }
        if ($node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -cnotin $writes) { throw "Unexpected assignment in $Kind" }
        if ($node -is [Management.Automation.Language.CommandAst]) {
            $name = $node.GetCommandName()
            if ($name -and $name -cin $commands) { continue }
            if ($Kind -eq 'common' -and $node.InvocationOperator -eq 'Ampersand' -and
                $node.CommandElements[0].Extent.Text -cin @('$Action', '$script:OnChange', '$script:OnFirstChange', '$script:OnMutation')) { continue }
            throw "Unexpected command in $Kind : $name"
        }
    }
}
function Parse-Fragment([string]$Text) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Invalid extracted fragment' }
    return $ast
}
Assert-Fragment $change 'common'
Assert-Fragment $oracle 'oracle'
Assert-Fragment (Parse-Fragment ($setup -join "`n")) 'setup'
function Run-Policy([string]$Source, [bool]$Dry) {
    Assert-Fragment (Parse-Fragment $Source) 'policy'
    $script:ReleaseName = 'amdgpu-wddm'
    $script:ParametersKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
    $script:SoftwareKey = 'HKLM:\SOFTWARE\amdgpu-wddm'
    $script:DryRunMode = $Dry
    $script:OnChange = $null; $script:OnFirstChange = $null; $script:OnMutation = $null
    $script:Mutated = $false
    $script:removals = New-Object 'Collections.Generic.List[string]'
    function Remove-ItemProperty {
        [CmdletBinding()]param([string]$LiteralPath, [string]$Name, [switch]$Force)
        $script:removals.Add("value|$LiteralPath|$Name|$Force")
    }
    function Remove-Item {
        [CmdletBinding()]param([string]$LiteralPath, [switch]$Recurse, [switch]$Force)
        $script:removals.Add("key|$LiteralPath|$Recurse|$Force")
    }
    function Write-Info([string]$Text) { Write-Output $Text }
    . ([scriptblock]::Create($change.Extent.Text))
    $text = (& ([scriptblock]::Create(($setup -join "`n") + "`n" + $Source)) 6>&1 | Out-String -Width 4096)
    $script:oracleResults = New-Object 'Collections.Generic.List[bool]'
    function Check([bool]$Ok, [string]$Text) { $script:oracleResults.Add($Ok) }
    $r = [pscustomobject]@{ text = $text }
    & ([scriptblock]::Create($oracle.Extent.Text))
    [pscustomobject]@{ text = $text; calls = @($script:removals.ToArray()); mutated = $script:Mutated
        oracle = $script:oracleResults.Count -eq 1 -and $script:oracleResults[0] }
}
$expected = @(
    'value|HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters|DwmForceCpu|True',
    'key|HKLM:\SOFTWARE\amdgpu-wddm|True|True',
    'key|HKLM:\SOFTWARE\WOW6432Node\amdgpu-wddm|True|True',
    'key|HKCU:\Software\amdgpu-wddm|True|True'
) -join "`n"
$dry = Run-Policy $policy.Extent.Text $true
if (-not $dry.oracle) { "Observed dry-run text: $($dry.text)" }
Assert-Test $dry.oracle 'production dry-run line satisfies the packaged assertion'
Assert-Test ($dry.calls.Count -eq 0 -and -not $dry.mutated) 'dry run invokes no removal and reports no mutation'
$real = Run-Policy $policy.Extent.Text $false
Assert-Test (($real.calls -join "`n") -ceq $expected) 'driver switch removed first, then exactly the three owned policy keys'
Assert-Test $real.mutated 'production Invoke-Change marks the removal path as a mutation'
# Discriminating controls change production source only in memory. No changed script is written or run as a file.
$withoutDriver = $policy.Extent.Text -replace '(?m)^\s*Remove-ItemProperty[^\r\n]+', ''
if ($withoutDriver -ceq $policy.Extent.Text) { throw 'Driver-removal mutant did not change the source' }
$mutant = Run-Policy $withoutDriver $false
Assert-Test (($mutant.calls -join "`n") -cne $expected) 'negative control: missing driver switch is detected'
$withoutWow = $policy.Extent.Text.Replace('$softwareKeyWow, ', '')
if ($withoutWow -ceq $policy.Extent.Text) { throw 'WOW64-removal mutant did not change the source' }
$mutant = Run-Policy $withoutWow $false
Assert-Test (($mutant.calls -join "`n") -cne $expected) 'negative control: missing WOW64 policy is detected'
$oldDescription = $policy.Extent.Text.Replace('$($script:ParametersKey)\DwmForceCpu, ', '')
if ($oldDescription -ceq $policy.Extent.Text) { throw 'Old-description mutant did not change the source' }
$mutant = Run-Policy $oldDescription $true
Assert-Test (-not $mutant.oracle) 'negative control: old removal description fails the packaged assertion'
# These fragments are parsed and refused only, never evaluated. They exercise each formerly unguarded boundary.
foreach ($control in @(
    @{ kind='policy'; text='[Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree("unused")' },
    @{ kind='common'; text='Microsoft.PowerShell.Management\Remove-Item -LiteralPath unused' },
    @{ kind='setup'; text='$userKey = [IO.File]::ReadAllText("unused")' },
    @{ kind='oracle'; text='Check ([IO.File]::Exists("unused")) "unused"' },
    @{ kind='common'; text='$Action = "unused"; & $Action' },
    @{ kind='setup'; text='$userKey = $(Get-Item unused)' }
)) {
    $refused = $false
    try { Assert-Fragment (Parse-Fragment $control.text) $control.kind } catch { $refused = $true }
    Assert-Test $refused "unsafe $($control.kind) fragment refused before evaluation"
}
"uninstall-policy: $($script:checks) checks, $($script:failed) failed"
if ($script:failed) { exit 1 }
