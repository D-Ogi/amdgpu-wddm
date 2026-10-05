import sys,subprocess
from pathlib import Path
sys.path.insert(0,r"P:\bc-250\bc250-win\tools\win")
from target import Target
t=Target(r"P:\bc-250\secrets\linux-session\target.json")
with Path(sys.argv[1]).open("rb") as source,Path(sys.argv[2]).open("xb",buffering=0) as out:
 p=subprocess.Popen(t.ssh_argv("sh -s",options=("-o","StrictHostKeyChecking=yes")),stdin=source,stdout=out,stderr=subprocess.STDOUT)
 print("ssh_pid="+str(p.pid),flush=True)
 rc=p.wait()
 print("ssh_exit="+str(rc),flush=True)
 raise SystemExit(rc)
