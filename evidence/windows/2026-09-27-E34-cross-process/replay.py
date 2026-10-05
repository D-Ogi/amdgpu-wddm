from pathlib import Path
import hashlib,json,shutil,subprocess
w=Path('P:/BC-250');p=Path(__file__).resolve().parent
d=w/'bc250-win/experiments/E34-native-d3d-zink/hosted-runtime'
manifest=json.loads((d/'completed-batches-manifest.json').read_text(encoding='utf-8'))
r=p/'replay';shutil.copytree(p/'lifetime-before',r)
for n,v in manifest.items():assert hashlib.sha256((r/n).read_bytes()).hexdigest()==v['before_lf_sha256']
subprocess.run(['git','-c','core.autocrlf=false','apply',str(d/'completed-batches.patch')],cwd=r,check=True)
for n,v in manifest.items():assert hashlib.sha256((r/n).read_bytes()).hexdigest()==v['after_lf_sha256']
print('PASS exact three-file patch replay')
