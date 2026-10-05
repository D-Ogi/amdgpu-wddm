# LAB (elevated SSH, one call per command from inp.py): writes the next cmd-NNN.txt for the input server of a game
# session and waits for its done-NNN.txt (100 ms poll on both sides). -Peek reports the queue and the server log tail.
param([Parameter(Mandatory)][string]$Session, [string]$ActionsB64 = '', [int]$TimeoutSec = 20, [switch]$Peek)
$ErrorActionPreference = 'Stop'
if ($Session -notmatch '^game-[a-z0-9-]{3,40}$') { throw 'bad session name' }
$dir = "C:\BC250\tmp\control\$Session"
if (-not (Test-Path $dir)) { throw "no session $Session" }
$cmds = @(Get-ChildItem -LiteralPath $dir -Filter 'cmd-*.txt' -File).Count
if ($Peek -or -not $ActionsB64) {
    "queue: $cmds commands, $(@(Get-ChildItem -LiteralPath $dir -Filter 'done-*.txt' -File).Count) done"
    Get-Content (Join-Path $dir 'server.log') -Tail 8 -ErrorAction SilentlyContinue
    return
}
$actions = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ActionsB64))
$n = $cmds + 1
$reply = Join-Path $dir ('done-{0:d3}.txt' -f $n)
[IO.File]::WriteAllText((Join-Path $dir ('cmd-{0:d3}.txt' -f $n)), $actions, [Text.Encoding]::ASCII)
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path -LiteralPath $reply) -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path -LiteralPath $reply)) { "TIMEOUT after $TimeoutSec s"; Get-Content (Join-Path $dir 'server.log') -Tail 3 -ErrorAction SilentlyContinue; return }
Start-Sleep -Milliseconds 50
"done-{0:d3} after {1:N1} s" -f $n, $sw.Elapsed.TotalSeconds
Get-Content -LiteralPath $reply
