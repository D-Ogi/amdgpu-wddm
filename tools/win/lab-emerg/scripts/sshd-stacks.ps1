# Non-invasive (-pvr: no suspend) user-mode stacks of sshd.exe and its sshd-session children, export names only.
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
$out = 'C:\BC250\tmp\sshd-stacks'
New-Item -ItemType Directory -Force $out | Out-Null
$ids = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^sshd' } | ForEach-Object { "$($_.ProcessId):$($_.Name)" })
foreach ($e in $ids) {
  $procId, $name = $e.Split(':')
  $log = Join-Path $out "$name-$procId.txt"
  & $cdb -pvr -p $procId -y 'C:\BC250\tools\cdb\nosym' -logo $log -c '.lines -d; ~*kn 40; lmf; qd' 2>&1 | Out-Null
  "==== $name $procId"
  Get-Content $log | Where-Object { $_ -match '^\s*[0-9a-f]{2} |^\s*\.?\s*\d+\s+Id:|^#' } | ForEach-Object { [string]$_ }
}
