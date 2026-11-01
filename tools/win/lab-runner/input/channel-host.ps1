# LAB (Windows PowerShell 5.1, one elevated ssh session held by channel.py for one game session): the far end of
# the held control channel (BD-108). It reads one request per line on stdin, runs a script that channel.py staged in
# -Dir, in this process, and writes the output between two marker lines:
#   in:   REQ <id> <base64 of {"script": "input-exec.ps1", "params": {"Session": "game-rottr", ...}}>
#   out:  @@BEGIN <id>
#         ...the script's output, one line per record...
#         @@END <id> <exit code> <UTC when the request came> <UTC when the script ended>
# Why. One command used to cost three ssh connections (target.py ps: a directory, the push, the run) and an elevated
# PowerShell start, about 2 s on an idle lab and up to 10 s beside a running game. Here a command costs none of that.
# The held session also keeps working while sshd accepts no new connection (BD-051 affects only new connections).
# The session ends at -Seconds (at most 1230, the game-session bound plus its closure), on a QUIT line, or when the
# host closes the connection. Only scripts in -Dir with a plain name run; the request cannot name a path.
param([int]$Seconds = 900, [Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$Seconds = [Math]::Min([Math]::Max($Seconds, 10), 1230)
$Dir = [IO.Path]::GetFullPath($Dir)
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
$out = New-Object IO.StreamWriter([Console]::OpenStandardOutput(), (New-Object Text.UTF8Encoding $false))
$out.NewLine = "`n"
$out.AutoFlush = $false
$in = New-Object IO.StreamReader([Console]::OpenStandardInput(), (New-Object Text.UTF8Encoding $false))
function Send([string]$Text) { $out.WriteLine($Text) }
function Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
Send "@@CHANNEL READY pid $PID bound $Seconds s"
$out.Flush()
$pending = $in.ReadLineAsync()
while ([DateTime]::UtcNow -lt $deadline) {
    # A bounded wait, so that the deadline holds while no request comes.
    if (-not $pending.Wait(500)) { continue }
    $line = $pending.Result
    if ($null -eq $line -or $line -eq 'QUIT') { break }
    $pending = $in.ReadLineAsync()
    if ($line -notmatch '^REQ ([0-9a-f]{32}) ([A-Za-z0-9+/=]+)$') { Send "@@BAD request line"; $out.Flush(); continue }
    $id = $Matches[1]; $body = $Matches[2]
    $received = Utc
    Send "@@BEGIN $id"
    $code = 0
    try {
        $req = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($body)) | ConvertFrom-Json
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
Send "@@CHANNEL END $(Utc)"
$out.Flush()
