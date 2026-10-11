# Host check of game-runtime.ps1 before readiness (2026-10-10, after 549): the control channel takes only the intro
# keys (tap or hold of Space 39, Esc 01, Enter 1C) and the actions that send no input, and refuses anything else at
# once; the profile's readiness.intro_skip presses its key every few seconds while no menu is detected and stops at
# the menu cue, the menu pass, readiness, its press limit or an unaccepted key-up. PS 5.1 parse; the functions and
# the state initialisers are extracted from game-runtime.ps1 itself. No key is ever sent on this PC: Send-Key,
# Focus-Game, Run-Action and the readiness answers are mocks.
param([string]$Runtime = "$PSScriptRoot\game-runtime.ps1",
      [string]$Profile = "$PSScriptRoot\profiles\witcher3.json")
$ErrorActionPreference = 'Stop'
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $Runtime), [ref]$null, [ref]$errors)
"game-runtime.ps1 parse errors: $(@($errors).Count)"
if (@($errors).Count) { throw 'game-runtime.ps1 does not parse' }
function Extract($name) {
    $f = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true) | Select-Object -First 1
    if (!$f) { throw "$name not found" }
    return $f.Extent.Text
}
function Assignment($left) {
    $a = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq $left }, $true) | Select-Object -First 1
    if (!$a) { throw "$left not found" }
    return $a.Extent.Text
}
function Check($ok, $text) { '{0} {1}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $text; if (!$ok) { $script:failed = $true } }
function Note($text) { $script:notes += , $text }
$script:notes = @()
$menuInit = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$menu' }, $true) | Select-Object -First 1
function New-Menu { . ([scriptblock]::Create($menuInit.Right.Extent.Text)) }
$out = Join-Path $env:TEMP ("intro-skip-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path $out

# --- PreReady-Refusal: which commands the channel runs before readiness.
. ([scriptblock]::Create((Assignment '$script:PreReadyKeys')))
. ([scriptblock]::Create((Extract 'PreReady-Refusal')))
$accepted = @('tap:39', 'tap:01', 'tap:1C', 'tap:1c', 'hold:39:300', 'hold:1C:2000', 'shot:0.5', 'ocr', 'note:intro', 'wait:1500', 'quit',
    'tap:39;wait:1500;shot:0.5', ' tap:01 ; shot:0.5 ', '')
foreach ($c in $accepted) { $r = PreReady-Refusal ($c -split ';'); Check ($r -eq '') "before readiness runs [$c]" }
$refused = [ordered]@{ 'tap:12' = 'tap:12'; 'tap:39;tap:11' = 'tap:11'; 'hold:11:3000' = 'hold:11:3000'; 'look:40:0:60:33' = 'look:40:0:60:33'
    'click:left' = 'click:left'; 'point:0.5:0.5' = 'point:0.5:0.5'; 'tapx:48' = 'tapx:48'; 'world' = 'world'; 'note:world' = 'note:world'
    'shot:0.5;world' = 'world'; 'tap:139' = 'tap:139'; 'hold:1C:abc' = 'hold:1C:abc'; 'tap:' = 'tap:'; 'unknown' = 'unknown' }
foreach ($c in $refused.Keys) { $r = PreReady-Refusal ($c -split ';'); Check ($r -eq $refused[$c]) "before readiness refuses [$c] at [$r]" }

# --- Control-Step against command files: the gate before readiness, the foreground rule, and no gate after it.
. ([scriptblock]::Create((Extract 'Control-Step')))
. ([scriptblock]::Create((Assignment '$control')))
$control.enabled = $true; $control.dir = Join-Path $out 'control'; $null = New-Item -ItemType Directory -Path $control.dir
$script:menu = New-Menu
$witcherMenu = $true
$script:safetyStop = ''
function Control-Open { $script:open }
function Focus-Game { $script:focusCalls++; $script:fg }
function Run-Action([string]$a) { $script:ran += , $a; if ($a -eq 'quit') { $control.quit = $true }; return 'ran ' + $a }
function World-Signal { @{ world = $false; telemetry = $false; why = 'mock' } }
function Walk-Marker([int]$t, [string]$why, [switch]$Verified) { 'walk ' + $t + 's: start (mock)' }
function Set-GamePriority { }
function Cmd($n, $text) { [IO.File]::WriteAllText((Join-Path $control.dir ('cmd-{0:d3}.txt' -f $n)), $text) }
function Done($n) { $p = Join-Path $control.dir ('done-{0:d3}.txt' -f $n); if (Test-Path $p) { (Get-Content -LiteralPath $p -Raw).Trim() } else { '<pending>' } }
$script:open = $false; $script:fg = $true; $script:ran = @(); $script:focusCalls = 0
Cmd 1 'tap:39;wait:1500;shot:0.5'
Cmd 2 'tap:12'
Cmd 3 'hold:1C:300'
Cmd 4 'world'
Cmd 5 'shot:0.5;look:40:0:60:33'
Control-Step 190
Check ((Done 1) -eq "ran tap:39`nran wait:1500`nran shot:0.5") "intro keys run before readiness: [$((Done 1) -replace "`n", ' | ')]"
Check ((Done 2) -like 'refused before readiness: tap:12 *') "E before readiness is refused at once: [$(Done 2)]"
Check ((Done 3) -eq 'ran hold:1C:300') "Enter held before readiness runs: [$(Done 3)]"
Check ((Done 4) -like 'refused before readiness: world *' -and !$control.walk_marked) "a world mark before readiness is refused, marker $($control.walk_marked)"
Check ((Done 5) -like 'refused before readiness: look:40:0:60:33 *') "a batch with one other action is refused whole: [$(Done 5)]"
Check (($script:ran -join ',') -eq 'tap:39,wait:1500,shot:0.5,hold:1C:300') "only the accepted actions reached Run-Action: [$($script:ran -join ',')]"
Check ($control.preready_commands -eq 2 -and $control.preready_refusals -eq 3 -and $control.commands -eq 2) ('counts: before readiness {0} run, {1} refused; commands {2}' -f $control.preready_commands, $control.preready_refusals, $control.commands)
# The game without the foreground (or without a window yet): a shot runs, a key does not.
$script:fg = $false; $script:ran = @(); $script:focusCalls = 0
Cmd 6 'shot:0.5;ocr'
Cmd 7 'tap:39'
Control-Step 200
Check ((Done 6) -eq "ran shot:0.5`nran ocr" -and (Done 7) -eq 'refused: game not foreground' -and $script:focusCalls -eq 1) ("no foreground: shot [{0}], Space [{1}], focus calls {2}" -f ((Done 6) -replace "`n", ' | '), (Done 7), $script:focusCalls)
# After readiness every action runs as before, and a world mark writes the marker.
$script:open = $true; $script:fg = $true; $script:ran = @()
Cmd 8 'tap:12'
Cmd 9 'note:world'
Control-Step 210
# note:world is the world mark both copies of the runtime read (the template also takes the shorter 'world').
Check ((Done 8) -eq 'ran tap:12' -and (Done 9) -eq 'ran note:world' -and $control.walk_marked) "after readiness: E [$(Done 8)], world mark [$(Done 9)], marker $($control.walk_marked)"
# Without the channel nothing is read.
$control.enabled = $false; Cmd 10 'tap:39'; Control-Step 220
Check ((Done 10) -eq '<pending>') "channel disabled: [$(Done 10)]"

# --- Intro-Step: Space every every_seconds while no menu is detected.
. ([scriptblock]::Create((Extract 'Intro-Stop')))
. ([scriptblock]::Create((Extract 'Intro-Step')))
$introIf = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Clauses[0].Item1.Extent.Text -eq '$introSkip' }, $true) | Select-Object -First 1
if (!$introIf) { throw 'intro skip initialiser not found' }
$gpShipped = Get-Content -LiteralPath $Profile -Raw | ConvertFrom-Json
Check ($gpShipped.readiness.mode -eq 'witcher3-menu' -and $gpShipped.readiness.intro_skip.key -eq '39') "the witcher3 profile skips intros with Space: $($gpShipped.readiness.intro_skip | ConvertTo-Json -Compress)"
function New-Intro($prof) {
    $script:gp = $prof
    . ([scriptblock]::Create((Assignment '$introSkip')))
    . ([scriptblock]::Create((Assignment '$intro')))
    . ([scriptblock]::Create($introIf.Extent.Text))
    $script:introSkip = $introSkip; $script:intro = $intro
}
$timer = [pscustomobject]@{ Elapsed = [TimeSpan]::Zero }
function Control-Open { $script:open }
function Menu-Signal([int]$t) { if ($script:menuSignalThrows) { throw 'Menu-Signal on a generic route' }; $script:cue }
function Focus-Game { $script:fg }
function Send-Key([uint16]$scan) { $script:sent += , $scan; return , $script:keyAnswer }
# Runs turns $step seconds apart from launch second $from, the first window at $window (seconds after launch).
# Its own variables must not shadow the runtime's ($generic, $menu, $intro): Intro-Step reads them through the caller's scope.
function Run-Intro([string]$name, $prof, [int]$from, [int]$turns, [int]$window, [int]$cueAt = -1, [int]$openAt = -1, [int]$fgFrom = 0, $answer = @(1, 1, 0), [int]$step = 3, [switch]$genericRoute) {
    New-Intro $prof
    $script:menu = New-Menu; $script:sent = @(); $script:notes = @(); $script:keyAnswer = [uint32[]]$answer; $script:menuSignalThrows = [bool]$genericRoute
    $script:witcherMenu = ![bool]$genericRoute
    $script:generic = [ordered]@{ window_at = $null }
    $launch = [DateTime]::UtcNow.AddSeconds(-($from + $turns * $step))
    for ($i = 0; $i -lt $turns; $i++) {
        $t = $from + $i * $step
        $timer.Elapsed = [TimeSpan]::FromSeconds($t)
        $script:windowUtc = if ($window -ge 0 -and $t -ge $window) { $launch.AddSeconds($from + $turns * $step - ($t - $window)) } else { $null }
        if ($genericRoute -and $window -ge 0 -and $t -ge $window) { $script:generic.window_at = $window }
        $script:cue = if ($cueAt -ge 0 -and $t -ge $cueAt) { 'dx12user.settings 2026-10-10T23:15:58Z' } else { '' }
        $script:open = $openAt -ge 0 -and $t -ge $openAt
        $script:fg = $t -ge $fgFrom
        Intro-Step $t
    }
    return [pscustomobject]@{ name = $name; sent = $script:sent; intro = $script:intro; notes = $script:notes }
}
$w3 = $gpShipped
# 549's own timeline: the first window at 16 s, the video waiting on Space, the account panel's settings write after it.
$r = Run-Intro '549 timeline' $w3 0 40 16 -cueAt 100
$times = @($r.intro.key_results | ForEach-Object { $_[0] })
Check ($r.intro.presses -ge 10 -and @($r.sent | Where-Object { $_ -ne 0x39 }).Count -eq 0 -and $times[0] -ge 26 -and $r.intro.stop_reason -like 'menu detected: dx12user.settings*' -and $r.intro.stopped_at -eq 102) ('{0}: {1} Space presses at [{2}], stopped at {3}: {4}' -f $r.name, $r.intro.presses, ($times -join ','), $r.intro.stopped_at, $r.intro.stop_reason)
$gaps = @(for ($i = 1; $i -lt $times.Count; $i++) { $times[$i] - $times[$i - 1] })
Check (@($gaps | Where-Object { $_ -lt 4 }).Count -eq 0) "presses at least every_seconds apart: gaps [$($gaps -join ',')]"
Check (@($r.notes | Where-Object { $_ -like 'intro *s: key 39 press *' }).Count -eq $r.intro.presses -and @($r.notes | Where-Object { $_ -like 'intro *s: skip stopped, menu detected*' }).Count -eq 1) "every press and the stop are noted ($($r.notes.Count) notes)"
$r = Run-Intro 'no window yet' $w3 0 20 -window (-1)
Check ($r.intro.presses -eq 0 -and $null -eq $r.intro.stopped_at) "$($r.name): presses $($r.intro.presses)"
$r = Run-Intro 'channel open at 180 s' $w3 150 20 16 -openAt 180
Check ($r.intro.stop_reason -eq 'readiness: the control channel is open' -and $r.intro.stopped_at -eq 180 -and @($r.intro.key_results | Where-Object { $_[0] -ge 180 }).Count -eq 0) "$($r.name): stopped at $($r.intro.stopped_at) ($($r.intro.stop_reason)), presses $($r.intro.presses)"
$r = Run-Intro 'press limit' $w3 0 200 16
Check ($r.intro.presses -eq $w3.readiness.intro_skip.max_presses -and $r.intro.stop_reason -like 'the limit of *') "$($r.name): presses $($r.intro.presses), $($r.intro.stop_reason)"
$r = Run-Intro 'desktop in front until 60 s' $w3 0 30 16 -fgFrom 60
Check ($r.intro.refusals -ge 5 -and @($r.intro.key_results | Where-Object { $_[0] -lt 60 }).Count -eq 0 -and $r.intro.presses -ge 4) "$($r.name): refusals $($r.intro.refusals), presses $($r.intro.presses)"
$r = Run-Intro 'key-up not accepted' $w3 0 30 16 -answer @(1, 0, 3)
Check ($r.intro.presses -eq 1 -and $r.intro.stop_reason -eq 'RELEASE UNRESOLVED') "$($r.name): presses $($r.intro.presses), $($r.intro.stop_reason)"
# The menu pass already holds a cue or has pressed E: no Space on top of it.
New-Intro $w3; $script:menu = New-Menu; $script:menu.attempts = 1; $script:sent = @(); $script:notes = @(); $script:open = $false; $script:fg = $true; $script:cue = ''
$script:witcherMenu = $true; $script:windowUtc = [DateTime]::UtcNow.AddSeconds(-60); $timer.Elapsed = [TimeSpan]::FromSeconds(76)
Intro-Step 76
Check ($script:sent.Count -eq 0 -and $intro.stop_reason -eq 'the menu pass has started') "menu pass started: sent $($script:sent.Count), $($intro.stop_reason)"
# A profile without intro_skip presses nothing.
$plain = $gpShipped | ConvertTo-Json -Depth 8 | ConvertFrom-Json; $plain.readiness = [pscustomobject]@{ mode = 'witcher3-menu' }
$r = Run-Intro 'profile without intro_skip' $plain 0 40 16
Check (!$r.intro.enabled -and $r.intro.presses -eq 0 -and $r.sent.Count -eq 0) "$($r.name): enabled $($r.intro.enabled), presses $($r.intro.presses)"
# A generic profile with intro_skip: its window time, no Witcher 3 menu signal.
$gen = $gpShipped | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$gen.readiness = [pscustomobject]@{ mode = 'generic'; control_after_window_seconds = 10; min_jobs = 50; intro_skip = [pscustomobject]@{ key = '1C'; every_seconds = 5; after_window_seconds = 0; max_presses = 3 } }
$r = Run-Intro 'generic profile, Enter' $gen 0 20 6 -genericRoute
Check ($r.intro.presses -eq 3 -and @($r.sent | Where-Object { $_ -ne 0x1C }).Count -eq 0 -and $r.intro.stop_reason -like 'the limit of 3*') "$($r.name): presses $($r.intro.presses) of key 1C, $($r.intro.stop_reason)"

if ((Split-Path $out -Leaf) -like 'intro-skip-*' -and (Split-Path $out -Parent) -eq $env:TEMP.TrimEnd('\')) { Remove-Item -LiteralPath $out -Recurse -Force }
if ($script:failed) { throw 'Intro skip check failed' }
'ALL PASS'
