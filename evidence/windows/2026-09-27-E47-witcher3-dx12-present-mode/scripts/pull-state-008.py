import sys
from pathlib import Path
sys.path.insert(0, 'P:/BC-250/bc250-win/tools/win')
from target import Target

t = Target()
d = Path('P:/BC-250/scratch/witcher3/dx12/run008/state')
d.mkdir(parents=True, exist_ok=True)
for n in ['memstate-before-008.json', 'memstate-after-008.json', 'cpustate-before-008.json',
          'cpustate-during-008.json', 'cpustate-after-008.json']:
    t.pull('C:\\BC250\\m12\\witcher3-dx12\\' + n, str(d / n))
    print(n, (d / n).stat().st_size)
