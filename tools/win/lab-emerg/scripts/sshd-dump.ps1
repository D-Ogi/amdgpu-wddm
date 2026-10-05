# Full user-mode dump of sshd.exe (non-invasive, no suspend) plus the OpenSSH binary versions, for offline symbolization.
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
$out = 'C:\BC250\tmp\sshd-stacks'
New-Item -ItemType Directory -Force $out | Out-Null
$p = Get-Process sshd -ErrorAction SilentlyContinue | Select-Object -First 1
$dmp = Join-Path $out "sshd-$($p.Id).dmp"
& $cdb -pvr -p $p.Id -y 'C:\BC250\tools\cdb\nosym' -c ".dump /ma $dmp; qd" 2>&1 | Out-Null
Get-Item $dmp | ForEach-Object { "dump $($_.FullName) $($_.Length)" }
foreach ($f in 'sshd.exe', 'sshd-session.exe', 'sshd-auth.exe', 'ssh-shellhost.exe') {
  $i = Get-Item "C:\Program Files\OpenSSH\$f" -ErrorAction SilentlyContinue
  if ($i) { "$f $($i.VersionInfo.FileVersion) $($i.VersionInfo.ProductVersion) $($i.Length) $((Get-FileHash $i.FullName).Hash.Substring(0,8))" }
}
