# gamebar-trace.ps1 - the Game Bar half of the M15.11 acceptance oracle
# (experiments/E50-m15-11-encoder-mft-lab, stages 1 and 5).
#
# Question: when Game Bar is asked to capture on unit A, which encoder MFT does it pick, and
# does it refuse for lack of a hardware encoder?
#
# What it changes on the lab (nothing driver-related, but it is not purely read-only):
#   - starts and stops one ETW session named BC250MF, writing an .etl under -OutDir;
#   - with -AutoHotkey, creates one scheduled task in the interactive session that presses
#     Win+Alt+R twice (start capture, stop capture) and deletes the task afterwards;
#   - Game Bar itself may write a video file under the signed-in user's Videos\Captures.
# No registry policy value, no driver, no firmware, no reboot. Everything it creates it removes.
#
# Run elevated through the usual tooling, after telling the owner through the overlay:
#   python tools\win\target.py ps driver\umd\mft-h264\tests\gamebar-trace.ps1 -- -Seconds 20
# Without -AutoHotkey the script only traces; press Win+Alt+R twice on the lab's screen inside
# the window (the owner sees the screen, so this works without any keystroke injection).
#
# Bounded: the default is 20 s of tracing, well inside the three-minute trial limit.

[CmdletBinding()]
param(
  [int]$Seconds = 20,
  [string]$OutDir = 'C:\BC250\tmp\m15-11-gamebar',
  [switch]$AutoHotkey
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$session = 'BC250MF'

function Log([string]$m) {
  Write-Output ('[' + (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff') + 'Z] ' + $m)
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$etl = Join-Path $OutDir ('gamebar-' + (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '.etl')

Log 'discovering the providers this build actually registers (no guessed GUIDs)'
$provs = & logman.exe query providers 2>$null
$want = @()
foreach ($line in $provs) {
  if ($line -match '^(?<name>\S.*?)\s{2,}(?<guid>\{[0-9A-Fa-f-]{36}\})\s*$') {
    $n = $matches['name'].Trim()
    $g = $matches['guid']
    if ($n -match 'MediaFoundation|Media-Foundation|GameBar|Game-Bar|GameDVR|Game-DVR|AppCapture|DxgKrnl') {
      $want += [pscustomobject]@{ Name = $n; Guid = $g }
    }
  }
}
if ($want.Count -eq 0) {
  Log 'no matching providers registered; nothing to trace. Report this and stop.'
  exit 1
}
$want | ForEach-Object { Log ('  provider ' + $_.Guid + '  ' + $_.Name) }

$provFile = Join-Path $OutDir 'providers.txt'
# logman -pf format: "{guid}" <keywords-hex> <level>
($want | ForEach-Object { '"' + $_.Guid + '"  0xffffffffffffffff  5' }) | Set-Content -Encoding Ascii $provFile

Log ('starting ETW session ' + $session)
& logman.exe create trace $session -ow -o $etl -pf $provFile -nb 16 16 -bs 1024 -mode Circular -f bincirc -max 64 -ets 2>&1 | ForEach-Object { Log ('  ' + $_) }

$task = 'BC250_M1511_GameBarHotkey'
try {
  if ($AutoHotkey) {
    Log 'arming the interactive-session hotkey task (start capture, wait, stop capture)'
    $helper = Join-Path $OutDir 'press-hotkey.ps1'
    $helperBody = @'
# runs in the interactive session: Win+Alt+R twice, with a pause between
Add-Type -Namespace Bc250 -Name K -MemberDefinition @"
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, System.UIntPtr dwExtraInfo);
"@
$LWIN=0x5B; $ALT=0x12; $R=0x52; $UP=0x0002
function Hit {
  [Bc250.K]::keybd_event($LWIN,0,0,[UIntPtr]::Zero)
  [Bc250.K]::keybd_event($ALT,0,0,[UIntPtr]::Zero)
  [Bc250.K]::keybd_event($R,0,0,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [Bc250.K]::keybd_event($R,0,$UP,[UIntPtr]::Zero)
  [Bc250.K]::keybd_event($ALT,0,$UP,[UIntPtr]::Zero)
  [Bc250.K]::keybd_event($LWIN,0,$UP,[UIntPtr]::Zero)
}
Hit
Start-Sleep -Seconds 8
Hit
'@
    Set-Content -Encoding Unicode -Path $helper -Value $helperBody
    $sessionId = ((& query.exe session 2>$null) | Select-String -Pattern '^\s*>?console\s+(\S+)\s+(\d+)' | ForEach-Object { $_.Matches[0].Groups[2].Value } | Select-Object -First 1)
    Log ('  console session id: ' + $sessionId)
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $helper + '"')
    $prin = New-ScheduledTaskPrincipal -GroupId 'INTERACTIVE' -RunLevel Limited
    Register-ScheduledTask -TaskName $task -Action $act -Principal $prin -Force | Out-Null
    Start-ScheduledTask -TaskName $task
    Log '  hotkey task started'
  } else {
    Log ('NOT automated: press Win+Alt+R on the lab screen now, and again after ~8 s (window: ' + $Seconds + ' s)')
  }

  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
}
finally {
  Log ('stopping ETW session ' + $session)
  & logman.exe stop $session -ets 2>&1 | ForEach-Object { Log ('  ' + $_) }
  if ($AutoHotkey) {
    try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue } catch {}
    Log '  hotkey task removed'
  }
}

Log 'what Game Bar left behind (names and sizes only)'
$u = (Get-CimInstance Win32_ComputerSystem).UserName
Log ('  signed-in user: ' + $u)
foreach ($root in @((Join-Path $env:SystemDrive 'Users'))) {
  Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $cap = Join-Path $_.FullName 'Videos\Captures'
    if (Test-Path $cap) {
      Get-ChildItem $cap -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 5 |
        ForEach-Object { Log ('  capture ' + $_.LastWriteTimeUtc.ToString('s') + 'Z  ' + $_.Length + ' B  ' + $_.Name) }
    }
  }
}

Log ('etl: ' + $etl)
Log 'decode on the development PC, not here (tracerpt or WPA); pull it with target.py'
Log 'done'
