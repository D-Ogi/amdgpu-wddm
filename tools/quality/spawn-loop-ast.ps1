# The PowerShell half of spawn_loop_lint.py (BD-051): finds the polling loops that start a process each round.
# Input: -List, a UTF-8 file with one .ps1/.psm1 path a line. Output: one JSON document on stdout, a list of
# {path, error, loops: [{line, kind, allowed, starts: [{line, what}]}]} with only the loops that both wait (a sleep)
# and start a process. It parses each file with the PowerShell parser and runs nothing of it.
#
# A polling loop: a while, do, for or foreach statement, or a ForEach-Object script block, whose body waits:
# Start-Sleep (or its alias sleep), [Threading.Thread]::Sleep, a .WaitOne( call, or a call of a function of the same
# file that does one of these.
# A process start in it:
#   - a command that is an application: a name or path ending in .exe/.cmd/.bat/.com, a name that this PC resolves
#     as an application (and the file defines no function of that name), or a lab tool of the list below;
#   - & or . on a variable that the file never gives a script block (a variable that holds a tool path);
#   - Start-Process, Start-Job, Start-ScheduledTask, schtasks, [Diagnostics.Process]::Start(...);
#   - a call of a function of the same file that does one of these (any depth).
# Only the innermost such loop counts, so a loop around a loop is not counted twice.
# A loop that must start a process each round says why in a comment on its first line or on one of the three lines
# above it: "spawn-loop-ok: <reason>". It is then reported as allowed and not counted.
param([Parameter(Mandatory)][string]$List)
$ErrorActionPreference = 'Stop'
$lab = @('bc250kmd_cli', 'bc250rd_cli', 'handle64', 'procdump', 'powershell', 'pwsh', 'cmd', 'schtasks', 'ssh', 'scp',
         'python', 'py', 'tar', 'curl', 'wevtutil', 'netsh', 'reg', 'sc', 'taskkill', 'tasklist', 'wmic', 'logman', 'wpr',
         'xperf', 'typeperf', 'pnputil', 'bcdedit', 'certutil', 'presentmon', 'amdgpu_wddm_d3d12caps', 'llama-bench',
         'llama-cli', 'llama-server', 'hipinfo', 'vulkaninfo', 'nvidia-smi', 'git', 'robocopy', 'xcopy', 'ping', 'whoami',
         'query', 'quser', 'icacls', 'takeown', 'fsutil', 'diskpart', 'dism')
$starters = @('start-process', 'start-job', 'start-scheduledtask', 'saps', 'start')
$sleepers = @('start-sleep', 'sleep')
$appCache = @{}

function Test-Application([string]$name) {
    $key = $name.ToLowerInvariant()
    if ($appCache.ContainsKey($key)) { return $appCache[$key] }
    $is = $false
    $base = [IO.Path]::GetFileNameWithoutExtension($key)
    if ($key -match '\.(exe|cmd|bat|com)$' -or $lab -contains $key -or $lab -contains $base) {
        $is = $true
    } elseif ($key -notmatch '[\\/]' -and $key -match '^[a-z0-9_.-]+$') {
        $other = Get-Command -Name $name -CommandType Alias, Cmdlet, Function -ErrorAction SilentlyContinue
        if (-not $other) { $is = [bool](Get-Command -Name $name -CommandType Application -ErrorAction SilentlyContinue) }
    }
    $appCache[$key] = $is
    return $is
}

# Every node under $Ast, without descending into nested function definitions (they only define).
function Get-Nodes($Ast, [scriptblock]$Predicate) {
    $root = $Ast
    return @($Ast.FindAll({
        param($n)
        if (-not (& $Predicate $n)) { return $false }
        $p = $n.Parent
        while ($p -and $p -ne $root) {
            if ($p -is [Management.Automation.Language.FunctionDefinitionAst]) { return $false }
            $p = $p.Parent
        }
        return $true
    }, $true))
}

function Get-VariableName($expr) {
    if ($expr -is [Management.Automation.Language.VariableExpressionAst]) {
        $n = $expr.VariablePath.UserPath
        return ($n -replace '^(script|global|local|private):', '').ToLowerInvariant()
    }
    return $null
}

function Analyze([string]$Path) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    $result = [ordered]@{ path = $Path; error = ''; loops = @() }
    if ($errors -and $errors.Count) { $result.error = 'parse: ' + $errors[0].Message + ' line ' + $errors[0].Extent.StartLineNumber }

    $functions = @{}
    foreach ($f in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        $functions[$f.Name.ToLowerInvariant()] = $f
    }
    # Variables that hold a script block: a script block literal, a [scriptblock] parameter, or .GetNewClosure().
    $blocks = @{}
    foreach ($a in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] }, $true)) {
        $name = Get-VariableName $a.Left
        if (-not $name) { continue }
        $text = $a.Right.Extent.Text.TrimStart()
        if ($text.StartsWith('{') -or $text -match 'GetNewClosure\(\)|\[scriptblock\]::Create') { $blocks[$name] = $true }
    }
    foreach ($p in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.ParameterAst] }, $true)) {
        if ($p.StaticType -eq [scriptblock] -or $p.Extent.Text -match '^\s*\[scriptblock\]') {
            $blocks[(Get-VariableName $p.Name)] = $true
        }
    }

    # The direct starts and waits of one subtree, as {line, what} lists, and the functions it calls.
    $direct = {
        param($node)
        $starts = New-Object Collections.Generic.List[object]
        $waits = New-Object Collections.Generic.List[object]
        $calls = New-Object Collections.Generic.List[string]
        foreach ($c in (Get-Nodes $node { param($n) $n -is [Management.Automation.Language.CommandAst] })) {
            $line = $c.Extent.StartLineNumber
            $name = $c.GetCommandName()
            $first = $c.CommandElements[0]
            if ($c.InvocationOperator -ne 'Unknown' -and -not $name) {
                $var = Get-VariableName $first
                if ($var -and -not $blocks.ContainsKey($var)) { $starts.Add(@{ line = $line; what = '& $' + $var }) }
                continue
            }
            if (-not $name) { continue }
            $low = $name.ToLowerInvariant()
            if ($functions.ContainsKey($low)) { $calls.Add($low); continue }
            if ($sleepers -contains $low) { $waits.Add(@{ line = $line; what = $name }); continue }
            if ($starters -contains $low) { $starts.Add(@{ line = $line; what = $name }); continue }
            if (Test-Application $name) { $starts.Add(@{ line = $line; what = $name }) }
        }
        foreach ($m in (Get-Nodes $node { param($n) $n -is [Management.Automation.Language.InvokeMemberExpressionAst] })) {
            $member = $m.Member.Extent.Text.Trim("'`"").ToLowerInvariant()
            $target = $m.Expression.Extent.Text.ToLowerInvariant()
            $line = $m.Extent.StartLineNumber
            if ($member -eq 'start' -and $target -match '^\[(system\.)?diagnostics\.process\]$') { $starts.Add(@{ line = $line; what = '[Diagnostics.Process]::Start' }) }
            elseif ($member -eq 'sleep' -and $target -match 'thread\]$') { $waits.Add(@{ line = $line; what = 'Thread::Sleep' }) }
            elseif ($member -eq 'waitone') { $waits.Add(@{ line = $line; what = '.WaitOne' }) }
        }
        # Plain arrays: in Windows PowerShell 5.1, @($table.key) of a generic List throws "argument types do not match".
        return @{ starts = $starts.ToArray(); waits = $waits.ToArray(); calls = $calls.ToArray() }
    }

    # Which functions start a process, and which wait, through any depth of calls inside this file.
    $own = @{}
    foreach ($k in $functions.Keys) { $own[$k] = & $direct $functions[$k].Body }
    $spawnFn = @{}; $waitFn = @{}
    foreach ($k in $own.Keys) {
        if ($own[$k].starts.Count) { $spawnFn[$k] = $own[$k].starts[0].what }
        if ($own[$k].waits.Count) { $waitFn[$k] = $true }
    }
    do {
        $changed = $false
        foreach ($k in $own.Keys) {
            foreach ($callee in $own[$k].calls) {
                if ($spawnFn.ContainsKey($callee) -and -not $spawnFn.ContainsKey($k)) { $spawnFn[$k] = 'via ' + $callee; $changed = $true }
                if ($waitFn.ContainsKey($callee) -and -not $waitFn.ContainsKey($k)) { $waitFn[$k] = $true; $changed = $true }
            }
        }
    } while ($changed)

    $loopNodes = @($ast.FindAll({
        param($n)
        if ($n -is [Management.Automation.Language.LoopStatementAst]) { return $true }
        if ($n -is [Management.Automation.Language.CommandAst]) {
            $cn = $n.GetCommandName()
            if ($cn -and @('foreach-object', '%') -contains $cn.ToLowerInvariant()) {
                return [bool]($n.CommandElements | Where-Object { $_ -is [Management.Automation.Language.ScriptBlockExpressionAst] })
            }
        }
        return $false
    }, $true))
    $flagged = New-Object Collections.Generic.List[object]
    foreach ($loop in $loopNodes) {
        $d = & $direct $loop
        $starts = @($d.starts)
        foreach ($callee in ($d.calls | Select-Object -Unique)) {
            if ($spawnFn.ContainsKey($callee)) { $starts += @{ line = $loop.Extent.StartLineNumber; what = $callee + ' (' + $spawnFn[$callee] + ')' } }
        }
        $waits = $d.waits.Count -gt 0
        foreach ($callee in $d.calls) { if ($waitFn.ContainsKey($callee)) { $waits = $true } }
        if (-not $waits -or $starts.Count -eq 0) { continue }
        $flagged.Add(@{ ast = $loop; starts = $starts })
    }
    # The innermost loops only.
    $innermost = @($flagged | Where-Object {
        $outer = $_.ast
        -not ($flagged | Where-Object { $_.ast -ne $outer -and $_.ast.Extent.StartOffset -ge $outer.Extent.StartOffset -and
                                         $_.ast.Extent.EndOffset -le $outer.Extent.EndOffset })
    })
    $comments = @($tokens | Where-Object { $_.Kind -eq 'Comment' })
    foreach ($f in $innermost) {
        $first = $f.ast.Extent.StartLineNumber
        $allowed = ''
        foreach ($c in $comments) {
            $l = $c.Extent.StartLineNumber
            if ($l -ge ($first - 3) -and $l -le $first -and $c.Text -match 'spawn-loop-ok:\s*(\S.*)$') { $allowed = $Matches[1].Trim() }
        }
        $kind = $f.ast.GetType().Name -replace 'StatementAst$', '' -replace 'Ast$', ''
        $result.loops += [ordered]@{ line = $first; kind = $kind; allowed = $allowed
                                     starts = @($f.starts | ForEach-Object { [ordered]@{ line = $_.line; what = $_.what } }) }
    }
    return $result
}

$out = @()
foreach ($path in [IO.File]::ReadAllLines($List, [Text.Encoding]::UTF8)) {
    if (-not $path.Trim()) { continue }
    try { $out += Analyze $path } catch {
        $out += [ordered]@{ path = $path; error = 'analysis: ' + $_.Exception.Message + ' (spawn-loop-ast.ps1 line ' +
                            $_.InvocationInfo.ScriptLineNumber + ')'; loops = @() }
    }
}
[Console]::Out.Write((ConvertTo-Json -InputObject @($out) -Depth 6 -Compress))
