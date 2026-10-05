# LAB, interactive session: started by app-run.ps1 through the one-shot task, as the logged-on user. Reads
# <Run>\spec.json, starts the client, waits at most its seconds, ends the client's process tree if it is still
# running (kill_at_end for windows that never exit by themselves), and writes <Run>\child.json.
param([Parameter(Mandatory)][string]$Run)
$ErrorActionPreference='Stop'
$c=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');user=[Environment]::UserName;session=(Get-Process -Id $PID).SessionId}
try {
 $s=Get-Content -LiteralPath (Join-Path $Run 'spec.json') -Raw | ConvertFrom-Json
 $start=@{FilePath=[string]$s.exe;PassThru=$true;WorkingDirectory=$Run}
 if([string]$s.arguments){$start.ArgumentList=[string]$s.arguments}
 # A console client shares this hidden console: no window appears on the owner's desktop.
 if($s.console){$start.RedirectStandardOutput=(Join-Path $Run 'stdout.txt');$start.RedirectStandardError=(Join-Path $Run 'stderr.txt');$start.NoNewWindow=$true}
 $p=Start-Process @start
 $c.pid=$p.Id;$c.started=[DateTime]::UtcNow.ToString('o')
 $exited=$p.WaitForExit([int]$s.seconds*1000)
 $c.killed=$false
 if(!$exited){
  & taskkill.exe /T /F /PID $p.Id | Out-Null
  $c.killed=$true
  $null=$p.WaitForExit(10000)
 }
 $c.ended=[DateTime]::UtcNow.ToString('o')
 $c.exit_code=if($p.HasExited){$p.ExitCode}else{$null}
 $c.outcome=if($exited){'exited'}elseif($s.kill_at_end){'ended-at-bound'}else{'killed-at-bound'}
} catch {
 $c.outcome='failed';$c.error=[string]$_
}
[IO.File]::WriteAllText((Join-Path $Run 'child.json'),($c|ConvertTo-Json -Depth 4))
