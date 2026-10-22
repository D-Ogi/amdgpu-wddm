# LAB (unit A, elevated SSH, Windows PowerShell 5.1), read only (first used by BD-170 r6). Is a game running (perhaps the owner's),
# and Tctl. A game: aces (War Thunder), launcher, cs2, witcher3, or any process whose image is under a Steam library's
# steamapps\common. Steam itself (steam, steamwebhelper) is not a game. "games 0" is the pass. Names only, no
# arguments. Nothing is stopped or written.
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o'))"
# 2026-10-22: also the GOG library (Cyberpunk 2077 runs from S:\GOG Galaxy\Games; the GOG Galaxy client itself is
# outside that folder) and the lab's own test programs, so that a trial never starts over another one.
$names = '^(aces|launcher|cs2|witcher3|Cyberpunk2077|q2rtx|ROTTR|factorio|Heaven|3DMark.*)$'
$games = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -match $names -or ($_.Path -and $_.Path -match '\\steamapps\\common\\|\\GOG Galaxy\\Games\\') })
foreach ($g in $games) { "game $($g.ProcessName) pid=$($g.Id) session=$($g.SessionId)" }
"games $($games.Count)"
$cliCandidates = @((Join-Path $env:ProgramFiles 'amdgpu-wddm\tools\bc250kmd_cli.exe'), 'C:\BC250\tools\bc250kmd_cli.exe')
$cli = @($cliCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)
if ($cli.Count) {
    $c = (& $cli[0] clock read 2>&1 | Out-String)
    if ($c -match 'temperature_mc=(\d+)') { "tctl_mc $($Matches[1])" } else { "tctl unknown: $($c.Trim())" }
} else { 'tctl unknown: no bc250kmd_cli' }
