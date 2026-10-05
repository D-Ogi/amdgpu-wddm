"""Extract actual CPU access gate and fence callbacks; model kernel primitives on Windows."""
from pathlib import Path
import sys
import re
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
def function(name):
    a=re.search(r'^(?:static )?(?:BC250_GFX\*|BOOLEAN|void) '+re.escape(name)+r'\(',s,re.M).start()
    brace=s.index('{',a);b=brace+1;depth=1
    while depth:
        if s[b]=='{':depth+=1
        elif s[b]=='}':depth-=1
        b+=1
    return s[a:b]+'\n'
names=['GfxAccessAcquire','GfxAccessRelease','GfxAccessClose','GfxAccessOpen','GfxFenceArrivedAccess','GfxFenceArrived','GfxPagingFenceArrivedAccess','GfxPagingFenceArrived']
text=(here/'gfx-access-test-prefix.c').read_text()+''.join(function(n) for n in names)+(here/'gfx-access-test-suffix.c').read_text()
if len(sys.argv)>3 and sys.argv[3]=='--disable-drain':
    text=text.replace('(void)KeWaitForSingleObject(&Device->GfxAccessDrained, Executive, KernelMode, FALSE, NULL);','SetEvent(closeStarted);')
Path(sys.argv[2]).write_text(text)
