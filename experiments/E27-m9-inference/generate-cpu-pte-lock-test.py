from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
def function(marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
actual=function('NTSTATUS VidMmUpdatePageTable(')+function('void VidMmStop(')+function('BOOLEAN VidMmTranslate(')
if '--unlocked' in sys.argv:
 actual=actual.replace('ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);','(void)0;').replace('ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);','(void)0;')
if '--reader-unlocked' in sys.argv:
 actual=actual.replace('ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);','(void)0;').replace('ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);','(void)0;')
Path(sys.argv[2]).write_text((here/'cpu-pte-lock-prefix.c').read_text()+actual+(here/'cpu-pte-lock-suffix.c').read_text())
