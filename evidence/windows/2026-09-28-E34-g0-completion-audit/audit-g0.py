from pathlib import Path
import json,hashlib
root=Path('P:/BC-250');repo=root/'bc250-win';raw=root/'scratch/g0-hosted/dwm050-ops';e=repo/'evidence/windows/2026-09-28-E34-dwm050-gpu-desktop';v=json.loads((e/'observations.json').read_text())
def obj(p):
 b=p.read_bytes();return json.JSONDecoder().raw_decode(b.decode('utf-16' if b.startswith(b'\xff\xfe') else 'utf-8-sig'))[0]
for n,x in v['inputs'].items():
 b=(raw/n).read_bytes();assert len(b)==x['bytes'] and hashlib.sha256(b).hexdigest()==x['sha256'],n
assert v['done.json']['success'] is True and v['done.json']['restoration_succeeded'] is True
assert 0<v['done.json']['trial_seconds']<=180 and v['client-done.json']['exit']==0
for n in ('gpu.bmp-check.json','screen.png-check.json'):assert v[n]['pass'] is True and v[n]['pixels']==40712 and v[n]['mismatches']==0
assert v['dma']['matched']==4498 and v['dma']['trace_loss']=={'events':0,'buffers':0}
assert all(v['dma'][k]==0 for k in ('pending','unmatched','duplicate_starts'))
assert v['present']['matched']==4274 and v['present']['checkpoint_count_checked'] is True and v['present']['runtime_maps']==0
assert v['census']['pending_store_ids']==[] and v['census']['store_checkpoint_counters'] is True
assert sorted(v['census']['live_buffer_ids'])==sorted(v['census']['live_map_ids']) and v['census']['crossing_image_maps']==[]
assert v['copy_observations']['software_blits_zero'] is True and v['copy_observations']['samples']==82 and v['copy_observations']['reuse']['equal'] is True
# This run did not supply a new positive control. The accepted list allows the preserved M702 control.
assert v['copy_observations']['positive_in_this_run'] is False
assert v['tdr-events.json']['boot_before']==v['tdr-events.json']['boot_after'] and not v['tdr-events.json']['suspect_events']
assert v['final_cpu']['confirmed']['flags']==15
for n in ('tdr-start.json','tdr-end.json'):assert v[n]['reset_calls']==v[n]['collect_debug_calls']==0
m=obj(raw/'result/manifest.json');done=v['done.json'];receipts=[obj(p) for p in (raw/'result').glob('process-*.json')]
assert receipts
for p in receipts:
 assert p['pid']==done['gpu_pid']
 hashes={x['name']:x['sha256'] for x in p['modules'] if 'dwm-hosted050' in x['path'].lower()}
 assert hashes['bc250d3d_zink.dll']==m['bc250d3d_zink.dll'] and hashes['vulkan_radeon.dll']==m['vulkan_radeon.dll']
refs=['2026-09-28-E34-window-client001','2026-09-28-E34-dwm045-copy-counters','2026-09-28-E34-audit-client005','2026-09-28-E34-dwm049-etw-runtime-joins']
assert all((repo/'evidence/windows'/n/'RESULT.md').is_file() for n in refs)
print(json.dumps({'pass':True,'input_hashes_checked':len(v['inputs']),'dwm_module_samples':len(receipts),'scope':'G0 implementation categories and accepted bounded no-copy list; not full M13 or permanent GPU deployment','whole_trial_seconds':done['trial_seconds']},indent=2))
