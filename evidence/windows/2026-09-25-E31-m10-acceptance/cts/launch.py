from pathlib import Path
import sys,hashlib,json
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'));from target import Target
w=Path(__file__).parent;exe=r/'scratch/m10/cts-build/external/vulkancts/modules/vulkan/deqp-vk.exe'
(w/'binary.json').write_text(json.dumps({'source_commit':'93bca01861b0e3ef3c387027a9791e6d065f900c','sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'bytes':exe.stat().st_size},indent=2))
t=Target();t.push([str(exe),str(w/'worker.ps1'),str(w/'cases.txt')],r'C:\BC250\m10\cts-smoke-06')
p=t.run_script(str(w/'launch.ps1'),timeout=30)
(w/'launch.log').write_text(p.stdout+p.stderr,encoding='utf-8');print(p.stdout);print('exit',p.returncode)
if p.returncode:print(p.stderr)
