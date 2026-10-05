# Future trials only: direct file descriptors preserve partial native output.
import sys,subprocess
from pathlib import Path
sys.path.insert(0,r"P:\bc-250\bc250-win\tools\win")
from target import Target
t=Target();remote=t.push([sys.argv[1]])[0]
log=Path(sys.argv[2])
with log.open("xb",buffering=0) as output:
 p=subprocess.Popen(t.ssh_argv('powershell -NoProfile -ExecutionPolicy Bypass -File "'+remote+'"'),stdout=output,stderr=subprocess.STDOUT)
 print("ssh_pid="+str(p.pid)+" log="+str(log),flush=True)
 code=p.wait()
print("ssh_exit="+str(code),flush=True)
sys.exit(code)
