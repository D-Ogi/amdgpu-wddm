from pathlib import Path
import sys,shutil,hashlib,subprocess,base64,time,io,tarfile,difflib
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'));from target import Target
base=r/'scratch/m10';w=base/'wsi-final'
shutil.copy2(r/'scratch/mesa-radv-main-build/src/amd/vulkan/vulkan_radeon.dll',w/'vulkan_radeon.dll')
shutil.copy2(base/'wsi-extent/radeon_icd.json',w/'radeon_icd.json')
h=hashlib.sha256((w/'vulkan_radeon.dll').read_bytes()).hexdigest().upper();(w/'sha256.txt').write_text(h+'\n')
t=Target();t.push([str(w/'vulkan_radeon.dll'),str(w/'radeon_icd.json')],r'C:\BC250\m10\wsi-final')
old='287CEF57354F1B1BEEAE24BDEA8977BFC86CC226CBF58AD85E1F783E50D1C7AC'
def convert(s):return s.replace('wsi-extent','wsi-final').replace(old,h)
def run(script,*args):
 p=subprocess.run([sys.executable,str(script),*args],cwd=r,timeout=150)
 assert p.returncode==0,script
def clone(source,dest,names):
 d=base/dest;d.mkdir(exist_ok=True)
 for name in names:
  (d/name).write_text(convert((base/source/name).read_text()).replace(source,dest).replace('CtsSmoke05','CtsSmoke06').replace('Lifecycle03','Lifecycle04').replace('Timing02','Timing03'))
 return d
cts=clone('cts-smoke-05','cts-smoke-06',['launch.py','launch.ps1','worker.ps1','status.py','cases.txt'])
print('FINAL_ICD',h,flush=True);run(cts/'launch.py')
for attempt in range(18):
 s=r"if(Test-Path C:\BC250\m10\cts-smoke-06\result.txt){Get-Content C:\BC250\m10\cts-smoke-06\result.txt}"
 p=t.ssh('powershell -NoProfile -EncodedCommand '+base64.b64encode(s.encode('utf-16-le')).decode(),timeout=15)
 if p.stdout.strip():
  assert p.stdout.strip()=='PASS',p.stdout;break
 time.sleep(3)
else:raise RuntimeError('CTS did not finish; inspect before more GPU work')
p=t.ssh(r'cmd /c "tar -cf - -C C:\BC250\m10\cts-smoke-06 *.qpa *.out *.err *.txt *.log"',timeout=30,binary=True)
assert p.returncode==0
with tarfile.open(fileobj=io.BytesIO(p.stdout)) as tar:
 for m in tar:
  assert m.isfile() and '/' not in m.name and '\\' not in m.name
  (cts/m.name).write_bytes(tar.extractfile(m).read())
print('FINAL_CTS_PASS',flush=True)
life=clone('lifecycle-03','lifecycle-04',['run.ps1','launch.py'])
shutil.copy2(base/'lifecycle-03/vkcube-observed.exe',life/'vkcube-observed.exe')
run(life/'launch.py')
assert (life/'native.exit').read_text().strip()=='0'
assert 'LIFECYCLE restore' in (life/'native.out').read_text()
assert not any(token in ((life/'native.out').read_text()+(life/'native.err').read_text()) for token in ['VUID-','SYNC-HAZARD','Validation Error'])
print('FINAL_LIFECYCLE_PASS',flush=True)
s=convert((base/'color-oracle/launch-extent.py').read_text())
(base/'color-oracle/launch-final.py').write_text(s)
for fmt in [37,44]:
 run(base/'color-oracle/launch-final.py',str(fmt),str(fmt)+'-release')
 print('FINAL_PIXELS_PASS',fmt,flush=True)
timing=clone('timing-02','timing-03',['run.ps1','launch.py'])
shutil.copy2(base/'timing-02/vkcube-observed.exe',timing/'vkcube-observed.exe')
run(timing/'launch.py')
assert (timing/'native.exit').read_text().strip()=='0'
print('FINAL_TIMING_PASS',flush=True)
