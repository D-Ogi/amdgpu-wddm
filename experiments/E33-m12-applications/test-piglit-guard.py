from pathlib import Path
from types import SimpleNamespace as NS
import importlib.util,tempfile,json
p=Path(__file__).with_name('piglit-guard.py');spec=importlib.util.spec_from_file_location('guard',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
for failure in ['fail','crash','timeout','warn']:
 with tempfile.TemporaryDirectory(dir='scratch/m12') as d:
  gate=m.CaseGate(Path(d)/'events.jsonl');calls=[]
  def original(test,name,log,options):calls.append(name)
  wrapped=gate.wrap(original)
  for name,result in [('positive','pass'),('unsupported','skip'),('bad',failure),('must_not_run','pass')]:
   monitor=NS(_abort_error=None);wrapped(NS(result=NS(result=result)),name,None,{'monitor':monitor})
  assert calls==['positive','unsupported','bad']
  assert gate.executed==3 and monitor._abort_error=='bad: '+failure
  rows=[json.loads(x) for x in (Path(d)/'events.jsonl').read_text().splitlines()]
  assert [x['name'] for x in rows if x['event']=='end']==calls
print('4 failure-stop controls passed; pass/skip preserved; subsequent case never executed')

with tempfile.TemporaryDirectory(dir='scratch/m12') as d:
 def denied(): raise RuntimeError('Owner STOP')
 gate=m.CaseGate(Path(d)/'events.jsonl',denied);calls=[]
 wrapped=gate.wrap(lambda *a:calls.append('executed'))
 wrapped(NS(result=NS(result='notrun')),'blocked',None,{'monitor':NS(_abort_error=None)})
 assert not calls and gate.executed==0 and 'Owner STOP' in gate.reason
print('Pre-case health/STOP rejection executes no test')
