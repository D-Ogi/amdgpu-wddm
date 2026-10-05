exec(compile(open('P:/BC-250/scratch/g0-hosted/dwm009/analyze-etw.py',encoding='utf-8-sig').read(),'analyze-etw.py','exec'))
owners={};stats=collections.defaultdict(collections.Counter);starts={};matched=0;unmatched=0;dur=[]
for e in events:
 name=next(iter(e.values()));ctx=e.get('hContext');pid=e.get('Process Name ( PID)','')
 if '/Context/win:Start' in name:owners[ctx]=12804 if re.search(r'\(\s*12804\s*\)',pid) else 0
 if owners.get(ctx)==12804:
  stats[ctx][name.split('/')[-2]+'/'+name.split('/')[-1]]+=1
  if '/DmaPacket/win:Start' in name:starts[(ctx,e['ulQueueSubmitSequence'])]=e
  if '/DmaPacket/win:Stop' in name:
   a=starts.pop((ctx,e['ulQueueSubmitSequence']),None)
   if a:matched+=1;dur.append(float(e['TimeStamp'])-float(a['TimeStamp']))
   else:unmatched+=1
 if '/Context/win:Stop' in name:owners.pop(ctx,None)
print('DMA matching',matched,unmatched,'pending',len(starts),'duration range',min(dur),max(dur))
print(json.dumps({k:dict(v) for k,v in stats.items()}))
(p/'etw-matched-partial.json').write_text(json.dumps({'trace_lost_events':10931,'dwm_pid':12804,'matched_dma_start_stop':matched,'unmatched_dma_stop':unmatched,'pending_dma_start':len(starts),'contexts':{k:dict(v) for k,v in stats.items()}},indent=2)+'\n',encoding='utf-8')
