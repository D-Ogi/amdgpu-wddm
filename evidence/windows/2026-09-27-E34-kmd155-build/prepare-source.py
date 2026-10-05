from pathlib import Path
import subprocess,json,hashlib
r=Path('P:/bc-250');main=r/'bc250-win';dst=r/'scratch/g0-kmd155-source';out=r/'scratch/g0-hosted/kmd155-preparation'
assert not dst.exists();out.mkdir(exist_ok=False)
subprocess.run(['git','-C',str(main),'worktree','add','-b','g0/kmd155-gpu-present',str(dst),'d7846ec'],check=True)
commits=['c1f326d','9befe71','6e7830b','757fae6','6c3af0f']
for commit in commits:
    patch=subprocess.check_output(['git','-C',str(main),'diff',commit+'^',commit,'--','driver/kmd','tools/quality/quick.ps1','experiments/E34-native-d3d-zink/hosted-runtime'])
    p=out/(commit+'.patch');p.write_bytes(patch)
    subprocess.run(['git','-C',str(dst),'apply','--check',str(p)],check=True)
    subprocess.run(['git','-C',str(dst),'apply',str(p)],check=True)
p=dst/'driver/kmd/bc250kmd_escape.h';s=p.read_text();assert '0x0007009Au' in s;s=s.replace('0x0007009Au       // revision 154: gated GPU Present producer/consumer','0x0007009Bu       // revision 155: GDI allocation validation and acquired Present IBs');p.write_text(s)
p=dst/'driver/kmd/bc250kmd.inf';s=p.read_text();assert '0.7.154.0' in s;s=s.replace('0.7.154.0','0.7.155.0');p.write_text(s)
base='c3499f1b12dcc57902fdd522c3ad10498abc5620'
allfiles=subprocess.check_output(['git','-C',str(main),'ls-tree','-r','--name-only',base,'--','driver','third_party'],text=True).splitlines()
different=[]
for f in allfiles:
    a=subprocess.check_output(['git','-C',str(main),'show',base+':'+f]).replace(b'\r\n',b'\n')
    b=(dst/f).read_bytes().replace(b'\r\n',b'\n')
    if a!=b:different.append(f)
expected={'driver/kmd/wddm.c','driver/kmd/bc250kmd_escape.h','driver/kmd/bc250kmd.inf','driver/kmd/dcn_translate.c','driver/kmd/dcn_translate.h','driver/kmd/test/dcn_translate_test.c'}
assert set(different)==expected,different
changed=subprocess.check_output(['git','-C',str(dst),'diff','--name-only'],text=True).splitlines()
new=subprocess.check_output(['git','-C',str(dst),'ls-files','--others','--exclude-standard'],text=True).splitlines()
files=changed+new
report={'baseline':base,'parent':'d7846ec','imports':commits,'compared_existing_driver_and_third_party_files':len(allfiles),'changed_existing':different,'method':'git blob bytes versus candidate, CRLF normalized only','working_files':{f:hashlib.sha256((dst/f).read_bytes()).hexdigest() for f in files},'patches':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.patch')}}
(out/'audit.json').write_text(json.dumps(report,indent=2)+'\n')
subprocess.run(['git','-C',str(dst),'diff','--check'],check=True)
subprocess.run(['git','-C',str(dst),'add','--',*files],check=True)
subprocess.run(['git','-C',str(dst),'commit','--only','-m','Build diagnostic155 Present and GDI validation on exact153 baseline','--',*files],check=True)
print(json.dumps({'compared':len(allfiles),'changed_existing':different,'source':subprocess.check_output(['git','-C',str(dst),'rev-parse','HEAD'],text=True).strip()}))
