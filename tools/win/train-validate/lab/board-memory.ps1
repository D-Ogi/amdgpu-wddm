# The board memory state of the installed driver, read only. Generic: it names no train and no package.
#
# This is the reading half of the board-memory acceptance operation. The operation itself is an operator arm:
# it needs the control application's Board memory card, an elevated confirmation and two restarts of Windows,
# which no lab script may do by itself. This script is what the operator runs at each step of it, before and
# after every restart, so that the four readings of the operation are in the run's evidence in one shape:
#
#   1. before any change        active 8192, next start 8192, the 12288 choice enabled
#   2. after the change in the app, before the restart   active 8192, next start 12288, backup present
#   3. after the restart        active 12288, next start 12288, no pending restart
#   4. after Restore and a restart       active 8192, next start 8192, the original backup unchanged
#
# It writes nothing to the board and nothing to the registry. The probe verb performs fixed HAL reads only,
# and it needs an administrator; an SSH session of the lab is elevated.
#   probe   bc250kmd_cli bc250-board-memory-probe, the HAL read route and the decoded block
#   vram    bc250kmd_cli vram, the dxgkrnl segment statistics of the adapter
#   ram     the physical memory Windows was left with
param([ValidateSet('probe', 'vram', 'ram', 'all')][string]$Step = 'all',
      [string]$Out = 'C:\BC250\tmp\train-board-memory')
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
"board memory read, step $Step, start $([DateTime]::UtcNow.ToString('o'))"
"cli   $(if (Test-Path -LiteralPath $cli) { (Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash.Substring(0,8) } else { 'ABSENT' })"

if ($Step -in 'probe', 'all') {
    '--- bc250kmd_cli bc250-board-memory-probe'
    $probe = & $cli bc250-board-memory-probe 2>&1
    $probe | Out-File -Encoding utf8 "$out\probe.json"
    # One JSON line. The block bytes stay in the file; the line below is what the run reads.
    $text = ($probe | Out-String)
    if ($text -match '"status"\s*:\s*(\d+)') { 'probe status {0}' -f $Matches[1] } else { 'probe status unread' }
    if ($text -match '"transport_status"\s*:\s*"(0x[0-9A-Fa-f]+)"') { 'probe transport {0}' -f $Matches[1] }
}

if ($Step -in 'vram', 'all') {
    '--- bc250kmd_cli vram'
    $vram = & $cli vram 2>&1
    $vram | Out-File -Encoding utf8 "$out\vram.txt"
    $vram | Select-Object -First 12 | ForEach-Object { '  ' + "$_" }
}

if ($Step -in 'ram', 'all') {
    '--- physical memory Windows holds'
    $total = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    'windows ram {0} MiB' -f [int]([Math]::Round($total / 1MB))
}
'board memory read done'
