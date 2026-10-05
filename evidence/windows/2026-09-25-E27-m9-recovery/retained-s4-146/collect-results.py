from pathlib import Path
import sys
r=Path('P:/bc-250');sys.path.insert(0,str(r/'bc250-win/tools/win'));from target import Target
x=Target();d=r/'scratch/m9/resume146/results';d.mkdir(exist_ok=True)
for name in ['probe.out','probe.err','probe.exit','before-driver.log','resumed-driver.log','after-driver.log','power-events.xml','processes-before.json','ready.txt','hibernate-request.txt','hibernate-return.txt']:
 x.pull('C:\\BC250\\m9\\resume146-s4\\'+name,str(d/name));print('collected',name,flush=True)
for phase in ['control','postcontrol']:
 for name in ['before.log','after.log','m8.out','m8.err','stories15M.out','stories15M.err','tinyllama.out','tinyllama.err']:
  x.pull('C:\\BC250\\m9\\resume146-'+phase+'\\'+name,str(d/(phase+'-'+name)))
print('all_results_collected')
