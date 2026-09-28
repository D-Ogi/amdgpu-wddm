"""Fill the candidate hash into run-cpucopy-NNN.ps1 and derive launch/worker/status/cleanup helpers.

Usage: python make-cpucopy.py <NNN> <CANDHASH64>
"""
import sys
from pathlib import Path

n, cand = sys.argv[1:3]
assert len(cand) == 64 and cand.upper() == cand
src = Path('P:/BC-250/scratch/witcher3/dx12')
run = src / f'run-cpucopy-{n}.ps1'
t = run.read_text(encoding='utf-8').replace('__CANDHASH__', cand).replace('__CAND8__', cand[:8])
assert '__CAND' not in t
run.write_text(t, encoding='utf-8', newline='\n')

launch = f"""# Launch cpucopy-{n} (two ICDs swapped in place in turn, vkcube CPU present with the timing log) in the interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\\BC250\\m12\\witcher3-dx12'
$task = 'BC250-M12-cpucopy-{n}'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) {{ throw 'Owner STOP' }}
if (Test-Path "$dir\\done-cpucopy-{n}.json") {{ throw 'Existing run' }}
if (Test-Path 'C:\\BC250\\m12\\cpucopy-{n}') {{ throw 'Existing output' }}
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) {{ throw 'Existing task' }}
$running = Get-ScheduledTask -TaskPath '\\' | Where-Object {{ $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }}
if ($running) {{ throw ('A BC250 test task is running: ' + (($running | ForEach-Object {{ $_.TaskName }}) -join ', ')) }}
if (Get-Process vkcube -ErrorAction SilentlyContinue) {{ throw 'vkcube running' }}
if ((Get-FileHash -LiteralPath 'C:\\BC250\\m10\\wsi-final\\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') {{ throw 'Registered ICD is not 93B1D1FD' }}
if ((Get-FileHash -LiteralPath 'C:\\BC250\\m12\\icd-candidates\\vulkan_radeon.{cand[:8]}.dll').Hash -ne '{cand}') {{ throw 'Candidate hash' }}
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) {{ throw 'No interactive user' }}
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\\worker-cpucopy-{n}.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 8)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
"""
worker = f"""$ErrorActionPreference = 'Stop'
$dir = 'C:\\BC250\\m12\\witcher3-dx12'
$code = 125
try {{
  & "$dir\\run-cpucopy-{n}.ps1" *> "$dir\\run-cpucopy-{n}.log"
  $code = $LASTEXITCODE
}} catch {{
  $_ | Out-String | Add-Content "$dir\\run-cpucopy-{n}.log"
}} finally {{
  @{{ exit = $code; utc = [DateTime]::UtcNow.ToString('o') }} | ConvertTo-Json | Set-Content "$dir\\done-cpucopy-{n}.json"
}}
exit $code
"""
status = f"""# Status of cpucopy-{n}: task state, done file, log tail; unregisters the task once it is not running.
$dir = 'C:\\BC250\\m12\\witcher3-dx12'
$task = 'BC250-M12-cpucopy-{n}'
"utc " + [DateTime]::UtcNow.ToString('o')
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
"task " + $(if ($t) {{ $t.State }} else {{ 'absent' }})
"done " + (Test-Path "$dir\\done-cpucopy-{n}.json")
if (Test-Path "$dir\\done-cpucopy-{n}.json") {{ Get-Content "$dir\\done-cpucopy-{n}.json" }}
"vkcube " + ((Get-Process vkcube -ErrorAction SilentlyContinue | Measure-Object).Count)
if (Test-Path "$dir\\run-cpucopy-{n}.log") {{ Get-Content "$dir\\run-cpucopy-{n}.log" | Where-Object {{ $_ -notmatch '^t=' }} | Select-Object -Last 40 }}
if ($t -and $t.State -ne 'Running' -and (Test-Path "$dir\\done-cpucopy-{n}.json")) {{ Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task_unregistered' }}
"registered_icd " + (Get-FileHash -LiteralPath 'C:\\BC250\\m10\\wsi-final\\vulkan_radeon.dll').Hash.Substring(0, 8)
"dwm " + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
"""
pull = f"""import sys
from pathlib import Path
sys.path.insert(0, 'P:/BC-250/bc250-win/tools/win')
from target import Target

t = Target()
r = Path('P:/BC-250/scratch/witcher3/dx12/cpucopy-{n}')
r.mkdir(parents=True, exist_ok=True)
names = ['before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt', 'before-log-summary.txt', 'after-log-summary.txt', 'error.txt']
for s in ['A-cached', 'B-wc']:
    for sz in ['640x480', '1920x1200']:
        tag = s + '-' + sz
        names += ['present-' + tag + '.csv', 'modules-' + tag + '.json', 'vkcube-' + tag + '.stdout.txt', 'vkcube-' + tag + '.stderr.txt', tag + '.png']
for n in names:
    try:
        t.pull('C:\\\\BC250\\\\m12\\\\cpucopy-{n}\\\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        (r / n).unlink(missing_ok=True)
        if n != 'error.txt':
            print(n, 'FAILED', e)
for n in ['run-cpucopy-{n}.log', 'done-cpucopy-{n}.json']:
    t.pull('C:\\\\BC250\\\\m12\\\\witcher3-dx12\\\\' + n, str(r / n))
    print(n, (r / n).stat().st_size)
raw = (r / 'run-cpucopy-{n}.log').read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\\xff\\xfe', b'\\xfe\\xff') else raw.decode('utf-8', 'replace')
(r / 'run-cpucopy-{n}.utf8.log').write_text(txt, encoding='utf-8')
print('\\n'.join(l for l in txt.splitlines() if not l.startswith('t=')))
"""
(src / f'launch-cpucopy-{n}.ps1').write_text(launch, encoding='utf-8', newline='\n')
(src / f'worker-cpucopy-{n}.ps1').write_text(worker, encoding='utf-8', newline='\n')
(src / f'status-cpucopy-{n}.ps1').write_text(status, encoding='utf-8', newline='\n')
(src / f'pull-cpucopy-{n}.py').write_text(pull, encoding='utf-8', newline='\n')
print('wrote cpucopy', n, cand[:8])
