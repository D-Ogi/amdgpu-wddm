from pathlib import Path
import hashlib,shutil
repo=Path('bc250-win');out=Path('scratch/m9/bd021-observation-source');out.mkdir(exist_ok=False)
paths=['driver/kmd/vidmm.c','experiments/E27-m9-inference/generate-paging-route-test.py','experiments/E27-m9-inference/paging-route-test-prefix.c','experiments/E27-m9-inference/paging-route-test-suffix.c']
rows=[]
for rel in paths:
 p=out/rel;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/rel,p)
 rows.append(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {rel}')
(out/'SHA256SUMS.txt').write_text('\n'.join(rows)+'\n')
for directory in ['scratch/m9/bd020','scratch/m9/bd021']:
 d=Path(directory);rows=[]
 for p in sorted(d.glob('*')):
  if p.is_file() and p.name not in ['SHA256SUMS.txt','FINAL-SHA256SUMS.txt']:
   rows.append(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}')
 (d/'FINAL-SHA256SUMS.txt').write_text('\n'.join(rows)+'\n')
print((out/'SHA256SUMS.txt').read_text())
