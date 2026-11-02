# LAB (Windows PowerShell 5.1, one elevated ssh session held by channel.py for one game session): the far end of
# the held control channel (BD-108). It reads one request per line, runs a script that channel.py staged in -Dir, in
# this process, and writes the output between two marker lines:
#   in:   PING <nonce>                                       (the handshake: no request goes out before its answer)
#   out:  @@PONG <nonce> <lab UTC>
#   in:   REQ <id> <base64 of {"script": "input-exec.ps1", "params": {...}, "not_after_ms": <lab Unix ms>}>
#   out:  @@GOT <id> <UTC when the request came>   (flushed at once, before the script runs)
#         @@BEGIN <id>
#         ...the script's output, one line per record...
#         @@END <id> <exit code> <UTC when the request came> <UTC when the script ended>
# Two transports. Without -Tcp the lines come on stdin and go to stdout. With -Tcp they go through one TCP connection
# to a listener on the lab's loopback (a free port), which the host reaches through the same ssh connection (ssh -D):
# stdout then carries only the READY line with the port and a one-time token, and the first line on the connection
# must be "HELLO <token>". Why the second transport: in bd108-k2 (2026-11-02) no request line came through the ssh
# session's stdin in 90 s, while the output came at once, so a held stdin cannot be the only path.
# A request that comes after its not_after_ms is refused and not run: input held on the way must never act late.
# Why at all. One command used to cost three ssh connections (target.py ps: a directory, the push, the run) and an
# elevated PowerShell start, about 2 s on an idle lab and up to 10 s beside a running game. Here a command costs none
# of that. The held session also keeps working while sshd accepts no new connection (BD-051 affects only new ones).
# The session ends at -Seconds (at most 1230, the game-session bound plus its closure), on a QUIT line, or when the
# host closes the connection. Only scripts in -Dir with a plain name run; the request cannot name a path.
param([int]$Seconds = 900, [Parameter(Mandatory)][string]$Dir, [switch]$Tcp)
$ErrorActionPreference = 'Stop'
$Seconds = [Math]::Min([Math]::Max($Seconds, 10), 1230)
$Dir = [IO.Path]::GetFullPath($Dir)
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
$utf8 = New-Object Text.UTF8Encoding $false
$stdout = New-Object IO.StreamWriter([Console]::OpenStandardOutput(), $utf8)
$stdout.NewLine = "`n"
$stdout.AutoFlush = $true
function Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
$client = $null
if ($Tcp) {
    $listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $bytes = New-Object byte[] 16
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $token = -join ($bytes | ForEach-Object { $_.ToString('x2') })
    $stdout.WriteLine("@@CHANNEL READY pid $PID bound $Seconds s tcp $($listener.LocalEndpoint.Port) $token")
    # One connection that names the token; any other one is closed. The listener stops once it is found.
    $accept = $listener.AcceptTcpClientAsync()
    while (-not $client -and [DateTime]::UtcNow -lt $deadline) {
        if (-not $accept.Wait(500)) { continue }
        $c = $accept.Result
        $r = New-Object IO.StreamReader($c.GetStream(), $utf8)
        $hello = $r.ReadLineAsync()
        if ($hello.Wait(5000) -and $hello.Result -eq "HELLO $token") {
            $client = $c
            $in = $r
            $out = New-Object IO.StreamWriter($c.GetStream(), $utf8)
        } else {
            $c.Close()
            $accept = $listener.AcceptTcpClientAsync()
        }
    }
    $listener.Stop()
    if (-not $client) { $stdout.WriteLine("@@CHANNEL END $(Utc) no connection"); exit 0 }
} else {
    $stdout.WriteLine("@@CHANNEL READY pid $PID bound $Seconds s")
    $in = New-Object IO.StreamReader([Console]::OpenStandardInput(), $utf8)
    $out = $stdout
}
$out.NewLine = "`n"
$out.AutoFlush = $false
function Send([string]$Text) { $out.WriteLine($Text) }
$pending = $in.ReadLineAsync()
while ([DateTime]::UtcNow -lt $deadline) {
    # A bounded wait, so that the deadline holds while no request comes.
    if (-not $pending.Wait(500)) { continue }
    $line = $pending.Result
    if ($null -eq $line -or $line -eq 'QUIT') { break }
    $pending = $in.ReadLineAsync()
    if ($line -match '^PING ([0-9a-f]{1,32})$') { Send "@@PONG $($Matches[1]) $(Utc)"; $out.Flush(); continue }
    if ($line -notmatch '^REQ ([0-9a-f]{32}) ([A-Za-z0-9+/=]+)$') { Send "@@BAD request line"; $out.Flush(); continue }
    $id = $Matches[1]; $body = $Matches[2]
    $received = Utc
    $nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    # At once, before the script runs: the host can then tell a request that never came from a script that hangs.
    Send "@@GOT $id $received"
    $out.Flush()
    Send "@@BEGIN $id"
    $code = 0
    try {
        $req = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($body)) | ConvertFrom-Json
        if ($req.not_after_ms -and $nowMs -gt [long]$req.not_after_ms) {
            throw "refused: the request came $($nowMs - [long]$req.not_after_ms) ms after its limit, it was not run"
        }
        $name = [string]$req.script
        if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.ps1$' -or $name -eq 'channel-host.ps1') { throw "refused script name '$name'" }
        $path = Join-Path $Dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "no staged script $name in $Dir" }
        $splat = @{}
        if ($req.params) { foreach ($p in $req.params.PSObject.Properties) { $splat[$p.Name] = $p.Value } }
        $global:LASTEXITCODE = 0
        # The script runs as under 'powershell -File': with the default error preference, not the 'Stop' of this host.
        & { param($Script, $Arguments) $ErrorActionPreference = 'Continue'; & $Script @Arguments } $path $splat *>&1 |
            ForEach-Object { Send ([string]$_) }
        $code = [int]$global:LASTEXITCODE
    } catch {
        Send ("error: " + $_.Exception.Message)
        $code = 1
    }
    Send "@@END $id $code $received $(Utc)"
    $out.Flush()
}
try { Send "@@CHANNEL END $(Utc)"; $out.Flush() } catch { }
if ($client) { $client.Close(); $stdout.WriteLine("@@CHANNEL END $(Utc)") }
