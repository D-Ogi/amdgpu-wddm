import sys
sys.path.insert(0, 'P:/BC-250/bc250-win/tools/win')
from target import Target
from pathlib import Path

t = Target()
dst = Path('P:/BC-250/scratch/witcher3/dx12/run010/log-summary-after-010.txt')
t.pull('C:\\BC250\\m12\\witcher3-dx12\\log-summary-after-010.txt', str(dst))
print(dst.stat().st_size)
