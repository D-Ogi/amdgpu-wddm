from pathlib import Path
import subprocess,json
p=Path(__file__).resolve().parent;results=[]
for mode,exe in [('old','test-detached-log-old.exe'),('new','test-detached-log.exe')]:
 receipt=p/f'detached-{mode}-002.txt';log=p/f'detached-{mode}-002.log';assert not receipt.exists()
 proc=subprocess.Popen([str(p/exe),str(log),str(receipt)],creationflags=subprocess.DETACHED_PROCESS,close_fds=True)
 rc=proc.wait(timeout=10);text=receipt.read_text() if receipt.exists() else 'MISSING';results.append(dict(mode=mode,pid=proc.pid,exit_code=rc,receipt=text));print(mode,rc,text.strip());assert rc==0
(p/'detached-controls002.json').write_text(json.dumps(results,indent=2)+'\n')
