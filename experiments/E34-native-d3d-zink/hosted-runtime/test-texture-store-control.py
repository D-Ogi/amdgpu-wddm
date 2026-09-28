import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path

here=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('control',here/'analyze-runtime-audit-control.py')
control=importlib.util.module_from_spec(spec);spec.loader.exec_module(control)
source=Path(sys.argv[1]).resolve()
result=control.analyze(source,'audit-client005')
assert result['descriptor_gpu_interval']['completed'] > 0
rejected=[]
with tempfile.TemporaryDirectory(dir=source.parent) as temp:
    target=Path(temp)
    for name in ['done.json','watchdog-done.json','control-result.json','manifest.json','modules.json','stdout.log','stderr.log']:
        shutil.copyfile(source/name,target/name)
    changes=[
        ('stdout.log','draws=18','draws=0'),
        ('stdout.log','pixels expected=ffff00ff bad=0','pixels expected=ffff00ff bad=1'),
        ('stdout.log','bytes=3686400','bytes=0'),
        ('stderr.log','kind=get_descriptor','kind=descriptor_copy'),
        ('stderr.log','stores_begun=','stores_missing='),
        ('modules.json','B514FF61','00000000'),
    ]
    for name,before,after in changes:
        p=target/name;original=p.read_text(encoding='utf-8-sig');assert before in original
        p.write_text(original.replace(before,after))
        try:
            control.analyze(target,'audit-client005')
        except ValueError:
            rejected.append(before)
        else:
            raise AssertionError('Accepted invalid control: '+before)
        finally:
            p.write_text(original)
print(json.dumps(dict(descriptors=result['descriptor_gpu_interval'],rejected=rejected),indent=2))
