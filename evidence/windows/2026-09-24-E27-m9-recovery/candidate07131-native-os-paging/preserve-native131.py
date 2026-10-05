from pathlib import Path
import hashlib,json,re,shutil
repo=Path('bc250-win');scratch=Path('scratch/m9')
e=repo/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07131-native-os-paging'
def read(p):
 b=p.read_bytes();return b.decode('utf-16') if b.startswith((b'\xff\xfe',b'\xfe\xff')) else b.decode('utf-8-sig')
def redact(b):
 if b.startswith((b'\xff\xfe',b'\xfe\xff')):
  s=b.decode('utf-16');assert s.encode('utf-16')==b
  s=re.sub(r'PCI\\VEN_[^\s\r\n]+','[PCI instance redacted]',s,flags=re.I)
  s=re.sub(r'\\\\\?\\pci#[^\s\r\n]+','[PCI interface redacted]',s,flags=re.I)
  return s.encode('utf-16')
 b=re.sub(rb'PCI\\VEN_[^\s\r\n]+',b'[PCI instance redacted]',b,flags=re.I)
 return re.sub(rb'\\\\\?\\pci#[^\s\r\n]+',b'[PCI interface redacted]',b,flags=re.I)
sha=hashlib.sha256(Path('scratch/build/bc250kmd-07131-v2/package-umd/bc250kmd.sys').read_bytes()).hexdigest().upper()
assert sha=='1F218318C57D4BC46D976B1AB4D43B659DB62233022FD1B9C0687EB1FEF9533B'
install=read(scratch/'native131-install.log');control=read(scratch/'native131-control.log');final=read(scratch/'native131-final.log')
assert 'candidate131_transition_complete' in install and 'driver_version=0.7.131.1' in install and sha in install
assert 'limited_run_complete' in control and 'CommandNotFoundException' not in control
assert '875844 checks, 0 failures' in read(scratch/'native131-route-v4.log')
assert '875844 checks, 2 failures' in read(scratch/'native131-no-write-mutation.log')
assert 'sdmava end status0x00000000 result0 fault0x0' in install
trials=re.findall(r'startup: SDMA control (\d) status (\d+) NT (0x[0-9A-F]+) result (-?\d+) fence (\d+)/(\d+) compare (\d+) matched (\d+)',install)
assert len(trials)==4
for i,t in enumerate(trials):assert t[0]==str(i) and t[1:4]==('0','0x00000000','0') and t[4]==t[5] and t[7]=='1'
art=scratch/'native131-artifacts';res=scratch/'native131-residency-artifacts'
validation={'sys_sha256':sha,'host_checks':875844,'host_failures':0,'permission_mutation_failures':2}
for n in ['m8','stories15M','tinyllama','worker']:assert read(art/(n+'.exit')).strip()=='0'
matches=re.findall(r'hash=(0x[0-9a-f]+) cpu_hash=\1 match=yes',read(art/'m8.out'));assert len(matches)==8
validation['shader_hashes']=matches
for n,layers in [('stories15M',7),('tinyllama',23)]:
 ref=repo/'evidence/linux/2026-09-21-E14-vulkan-compute-reference/llama'/f'{n}-ngl99.out'
 assert read(art/(n+'.out')).replace('\r','')==read(ref).replace('\r','')
 assert f'offloaded {layers}/{layers} layers to GPU' in read(art/(n+'.err'))
 validation[n]={'full_reference_match':True,'layers':layers,'reference_sha256':hashlib.sha256(ref.read_bytes()).hexdigest()}
for n in ['m8','stories15M','tinyllama']:assert r'radv-main-icd2\vulkan_radeon.dll' in read(art/(n+'.err'))
assert read(res/'native1g.exit').strip()=='0'
r=read(res/'native1g.out')
readbacks=re.findall(r'GPU_READBACK bytes=(\d+) all_words_match=1 fence=(\d+)',r)
assert readbacks==[(str(1<<30),str(n)) for n in [1024,2048,3072,4096]],readbacks
assert re.findall(r'CYCLE (\d) PASS',r)==['1','2','3'] and 'GPU_RESIDENCY_RESULT PASS' in r
validation['residency_readbacks']=readbacks
for tag,pat in [('gfx',r'node 0 hardware: (\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused'),('sdma',r'node 1 \(paging, open\): (\d+) hardware submitted, (\d+) completed, (\d+) timeouts, (\d+) refused')]:
 counts=list(map(int,re.findall(pat,final)[-1]));assert counts[0]==counts[1] and counts[0]>0 and counts[2:]==[0,0];validation[tag]=counts
native=list(map(int,re.findall(r'native DMA transfers (\d+) fills (\d+) bytes (\d+)',final)[-1]));assert native[0]>=21 and native[1]>=68
validation['native_transfers_fills_bytes']=native
assert 'DMA mapping matches 8/8' in final and 'no TDR' in final
assert 'boot=2026-09-24T11:44:14' in final and 'dwm_pid=4448 start=2026-09-24T13:58:18 responding=True' in final
assert 'D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D' in final
validation['final_time']=re.findall(r'final_time=(\S+)',final)[-1]
validation['capture_reserved_heap']=list(map(int,re.findall(r'capture plans reserved (\d+) heap (\d+)',final)[-1]))
validation['native_exits']='all zero; worker, eight shaders, both models, residency'
validation['os_dwm_retained']=True
# Validate before creating the immutable directory.
e.mkdir(exist_ok=False)
for n in ['native131-preflight.log','native131-install.log','native131-initial.log','native131-control.log','native131-post-model.log','native131-residency.log','native131-residency-observe.log','native131-observe.log','native131-final.log','native131-build-v2.log','native131-route-v4.log','native131-no-write-mutation.log']:
 (e/n).write_bytes(redact((scratch/n).read_bytes()))
for folder,src in [('control',art),('residency',res)]:
 (e/folder).mkdir()
 for p in src.iterdir():(e/folder/p.name).write_bytes(redact(p.read_bytes()))
for n in ['native131-install.ps1','native131-control.ps1','native131-residency.ps1','native131-final.ps1','preserve-native131.py']:
 shutil.copyfile(scratch/n,e/n)
shutil.copyfile(repo/'experiments/E27-m9-inference/native-os-paging/README.md',e/'PLAN.md')
names=['driver/kmd/gfx.c','driver/kmd/wddm.c','driver/kmd/vidmm.c','driver/kmd/bc250kmd.h','driver/kmd/bc250kmd_escape.h','driver/kmd/bc250kmd.inf','driver/kmd/paging_private.c','driver/kmd/paging_private.h','driver/kmd/paging_stream.c','driver/kmd/paging_stream.h','driver/shim/bc250_sdma.c','driver/shim/bc250_sdma_paging.c','driver/shim/bc250_pte.c','driver/shim/include/bc250_sdma.h','driver/shim/include/bc250_pte.h','experiments/E27-m9-inference/generate-paging-route-test.py','experiments/E27-m9-inference/paging-route-test-prefix.c','experiments/E27-m9-inference/paging-route-test-suffix.c','driver/shim/test/run_paging.ps1']
ids={}
for n in names:
 dst=e/'source'/n;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/n,dst);ids[n]=hashlib.sha256(dst.read_bytes()).hexdigest()
ids['private_scanout_bmp']=hashlib.sha256((scratch/'native131-scanout.bmp').read_bytes()).hexdigest()
(e/'sources.json').write_text(json.dumps(ids,indent=2)+'\n',encoding='utf-8')
(e/'validation.json').write_text(json.dumps(validation,indent=2)+'\n',encoding='utf-8')
print(json.dumps(validation,indent=2))
