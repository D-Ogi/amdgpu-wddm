import sys
sys.path.insert(0, 'P:/BC-250/bc250-win/tools/win')
from target import Target

t = Target()
dst = 'P:/BC-250/scratch/witcher3/dx12/pfsample-010.local.txt'
t.pull('C:\\BC250\\m12\\witcher3-dx12\\pfsample-010.txt', dst)
print(open(dst, encoding='utf-8', errors='replace').read()[:200])
