from pathlib import Path
import sys,base64
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'));from target import Target
w=Path(__file__).parent
s=r"""$ErrorActionPreference='Stop'
$out='C:\BC250\m10\cts-smoke-06'
Get-ScheduledTask BC250-M10-CtsSmoke06 | Select-Object State | ConvertTo-Json -Compress
Get-Process deqp-vk -ErrorAction SilentlyContinue | Select-Object Id,CPU,StartTime | ConvertTo-Json -Compress
if(Test-Path "$out\progress.txt"){Get-Content "$out\progress.txt" -Tail 8}
if(Test-Path "$out\result.txt"){Get-Content "$out\result.txt"}
"""
p=Target().ssh('powershell -NoProfile -EncodedCommand '+base64.b64encode(s.encode('utf-16-le')).decode(),timeout=20)
print(p.stdout)
(w/'last-status.log').write_text(p.stdout+p.stderr,encoding='utf-8')
