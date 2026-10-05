from pathlib import Path
import hashlib,json,re,shutil
repo=Path('bc250-win');scratch=Path('scratch/m9')
e=repo/'evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-os-private-queue'
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
sha=hashlib.sha256(Path('scratch/build/bc250kmd-07133/package-umd/bc250kmd.sys').read_bytes()).hexdigest().upper()
assert sha=='37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87'
install=read(scratch/'native133-install.log');control=read(scratch/'native133-control.log');final=read(scratch/'native133-final.log')
assert 'candidate133_transition_complete' in install and 'driver_version=0.7.133.1' in install and sha in install
assert 'limited_run_complete' in control and 'CommandNotFoundException' not in control
assert '907971 checks, 0 failures' in read(scratch/'paging-queued-builders-route-v3.log')
assert '711 checks, 0 failures' in read(scratch/'paging-queued-builders-integration.log')
assert '711 checks, 123 failures' in read(scratch/'paging-queued-builders-mutation.log')
assert 'sdmava end status0x00000000 result0 fault0x0' in install
trials=re.findall(r'startup: SDMA control (\d) status (\d+) NT (0x[0-9A-F]+) result (-?\d+) fence (\d+)/(\d+) compare (\d+) matched (\d+)',install)
assert len(trials)==4
for i,t in enumerate(trials):assert t[0]==str(i) and t[1:4]==('0','0x00000000','0') and t[4]==t[5] and t[7]=='1'
art=scratch/'native133-artifacts';res=scratch/'native133-residency-artifacts'
validation={'sys_sha256':sha,'builder_checks':907971,'builder_failures':0,'queue_checks':711,'queue_failures':0,'late_release_mutation_failures':123}
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
native=list(map(int,re.findall(r'native DMA transfers (\d+) fills (\d+) bytes (\d+)',final)[-1]));assert native[0]>0 and native[1]>0
validation['native_transfers_fills_bytes']=native
assert 'DMA mapping matches 8/8' in final and 'no TDR' in final
assert 'boot=2026-09-24T11:44:14' in final and 'dwm_pid=4448 start=2026-09-24T13:58:18 responding=True' in final
assert 'D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D' in final
validation['final_time']=re.findall(r'final_time=(\S+)',final)[-1]
validation['capture_reserved_heap']=list(map(int,re.findall(r'capture plans reserved (\d+) heap (\d+)',final)[-1]))
validation['native_exits']='all zero; worker, eight shaders, both models,64MiB/1GiB residency'
assert validation['capture_reserved_heap']==[0,0]
validation['exact_disjoint_checks']=int(re.findall(r'native DMA exact disjoint checks (\d+)',final)[-1]);assert validation['exact_disjoint_checks']>0
small=read(res/'native64m.out');assert read(res/'native64m.exit').strip()=='0'
assert re.findall(r'GPU_READBACK bytes=(\d+) all_words_match=1 fence=(\d+)',small)==[(str(1<<26),str(n)) for n in [64,128,192,256]]
assert re.findall(r'CYCLE (\d) PASS',small)==['1','2','3']
validation['restore_ms']=list(map(int,re.findall(r'BULK_RESIDENT bytes=1073741824 elapsed_ms=(\d+) success=1',r)));assert len(validation['restore_ms'])==3
previous=read(Path('scratch/m9/native132-residency-artifacts/native1g.out'))
validation['previous132_restore_ms']=list(map(int,re.findall(r'BULK_RESIDENT bytes=1073741824 elapsed_ms=(\d+) success=1',previous)))
validation['os_dwm_retained']=True
validation['queue_admissions']=int(re.findall(r'OS-private queue admissions=(\d+)',final)[-1])
assert validation['queue_admissions']==validation['sdma'][0]
for stage in ['before','after']:
 text=read(res/('native1g-'+stage+'.log'))
 validation['large_'+stage+'_native']=list(map(int,re.findall(r'native DMA transfers (\d+) fills (\d+) bytes (\d+)',text)[-1]))
validation['large_native_delta']=[b-a for a,b in zip(validation['large_before_native'],validation['large_after_native'])]
assert validation['large_native_delta'][2]==10*(1<<30)

# Validate before creating the immutable directory.
e.mkdir(exist_ok=False)
for n in ['native133-install.log','native133-control.log','native133-small.log','native133-residency.log','native133-final.log','native133-build.log','queued133-preflight.log','paging-queued-builders-route.log','paging-queued-builders-route-v2.log','paging-queued-builders-route-v3.log','paging-queued-builders-queue.log','paging-queued-builders-queue-v2.log','paging-queued-builders-integration.log','paging-queued-builders-mutation.log']:
 (e/n).write_bytes(redact((scratch/n).read_bytes()))
for folder,src in [('control',art),('residency',res)]:
 (e/folder).mkdir()
 for p in src.iterdir():(e/folder/p.name).write_bytes(redact(p.read_bytes()))
for n in ['native133-install.ps1','native133-control.ps1','native133-residency.ps1','native133-final.ps1','native133-small.ps1','preserve-native133.py']:
 shutil.copyfile(scratch/n,e/n)
shutil.copyfile(repo/'experiments/E27-m9-inference/os-private-queue/README.md',e/'PLAN.md')
names=['driver/kmd/gfx.c','driver/kmd/wddm.c','driver/kmd/vidmm.c','driver/kmd/bc250kmd.h','driver/kmd/bc250kmd_escape.h','driver/kmd/bc250kmd.inf','driver/kmd/paging_private.c','driver/kmd/paging_private.h','driver/kmd/paging_stream.c','driver/kmd/paging_stream.h','driver/shim/bc250_sdma.c','driver/shim/bc250_sdma_paging.c','driver/shim/bc250_pte.c','driver/shim/include/bc250_sdma.h','driver/shim/include/bc250_pte.h','experiments/E27-m9-inference/generate-paging-route-test.py','experiments/E27-m9-inference/paging-route-test-prefix.c','experiments/E27-m9-inference/paging-route-test-suffix.c','driver/shim/test/run_paging.ps1']
names += ['experiments/E27-m9-inference/'+name for name in ['generate-paging-queue-test.py','paging-queue-test-prefix.c','paging-queue-test-suffix.c','build-paging-queue-test.cmd']]
ids={}
for n in names:
 dst=e/'source'/n;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/n,dst);ids[n]=hashlib.sha256(dst.read_bytes()).hexdigest()
ids['private_scanout_bmp']=hashlib.sha256((scratch/'native133-scanout.bmp').read_bytes()).hexdigest()
shutil.copyfile(scratch/'paging-queued-builders-mutated.c',e/'late-release-fixture.c')
shutil.copyfile(scratch/'paging-queued-builders-positive.c',e/'queue-fixture.c')
for name in ['direct.bin','native.bin']:
 shutil.copyfile(scratch/'paging-queued-builders-fixtures'/name,e/name)
(e/'sources.json').write_text(json.dumps(ids,indent=2)+'\n',encoding='utf-8')
(e/'validation.json').write_text(json.dumps(validation,indent=2)+'\n',encoding='utf-8')
print(json.dumps(validation,indent=2))
