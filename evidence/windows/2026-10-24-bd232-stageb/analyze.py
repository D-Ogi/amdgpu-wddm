"""Stage B exact membership, interpolation, independent native reference and route validation."""
import argparse, gzip, hashlib, json, math, struct
from pathlib import Path
if not __debug__:
    raise RuntimeError('Optimized Python disables validation assertions; run without -O or PYTHONOPTIMIZE')
p=argparse.ArgumentParser()
p.add_argument('prefix',type=Path); p.add_argument('--reference',type=Path); p.add_argument('--reference-proof',type=Path); p.add_argument('--output',type=Path,required=True)
a=p.parse_args(); assert not a.output.exists()
MASKS=[0,65535]+[1<<i for i in range(16)]+[(1<<i)|(1<<j) for i in range(16) for j in range(i+1,16)]
POS={8:[(9,5),(7,11),(13,9),(5,3),(3,13),(1,7),(11,15),(15,1)],16:[(9,9),(7,5),(5,10),(12,7),(3,6),(10,13),(13,11),(11,3),(6,14),(8,1),(4,2),(2,12),(0,8),(15,4),(14,15),(1,0)]}
LB=6*64*16*64; PB=6*64*16*8; STRIDE=LB+PB+256; SIZE=138*STRIDE
def sha(data): return hashlib.sha256(data).hexdigest()
def load(prefix):
    meta_path=Path(str(prefix)+'.json'); meta=json.loads(meta_path.read_text(encoding='utf-8-sig'))
    path=Path(str(prefix)+'.bin'); raw=path.read_bytes() if path.exists() else gzip.decompress(Path(str(path)+'.gz').read_bytes())
    assert len(raw)==SIZE and meta['schema']=='s16f8-stage-b-1' and meta['dimensions']==[138,6,64,16,16]
    assert meta['masks']==MASKS and meta['arm_bytes']==STRIDE and meta['logical_bytes']==LB and meta['physical_bytes']==PB
    run=json.loads(Path(str(prefix)+'-result.json').read_text(encoding='utf-8-sig'))
    assert run['exit_code']==0 and not run['timed_out'] and run['observed_engine'] and run['driver_state_verified']
    assert meta['native_samples']==run['native_samples'] and meta['program']==run['program'] and meta['negative']==run['negative']
    assert run['icd_verification_required']==(not run['host_native_control'])
    modules={m['sha256'].lower() for m in run['modules']}
    assert run['expected_engine_sha256'].lower() in modules
    assert not run['icd_verification_required'] or (run['observed_icd'] and run['expected_icd_sha256'].lower() in modules)
    item=next(i for i in run['artifacts'] if i['path']==path.name)
    assert item['sha256'].lower()==sha(raw) and item['bytes']==SIZE
    item=next(i for i in run['artifacts'] if i['path']==meta_path.name)
    assert item['sha256'].lower()==sha(meta_path.read_bytes())
    return meta,raw,run
meta,raw,run=load(a.prefix); program=meta['program']; native=program=='native'
assert program in ('native','paired','hostpaired')
assert (run['mode']=='exposed16' and meta['native_samples']==8) if program=='paired' else run['mode']=='off'
assert program!='hostpaired' or (run['host_native_control'] and meta['native_samples']==16)
assert program!='paired' or (not run['host_native_control'] and run['icd_verification_required'])
assert not native or meta['negative']==0
assert native or a.reference, 'Paired results require independent native16 reference'
ref=None
if a.reference:
    rm,rr,rs=load(a.reference)
    assert rm['program']=='native' and rm['negative']==0 and rs['mode']=='off'
    assert rm['native_samples']==(meta['native_samples'] if native else 16)
    assert all(meta[k]==rm[k] for k in ('masks','vertices','dimensions'))
    def client(r): return next(i['sha256'].lower() for i in r['artifacts'] if i['path']=='s16f8-client.exe')
    assert client(run)==client(rs) and run['expected_engine_sha256']==rs['expected_engine_sha256']
    proof=json.loads((a.reference_proof or a.reference.parent/'analysis.json').read_text())
    assert proof['status']=='PASS' and proof['raw_sha256']==sha(rr) and proof['analyzer_sha256']==sha(Path(__file__).read_bytes())
    assert proof['metadata_sha256']==sha(Path(str(a.reference)+'.json').read_bytes())
    assert proof['client_sha256'].lower()==client(rs) and proof['engine_sha256'].lower()==rs['expected_engine_sha256'].lower()
    assert proof['program']=='native' and proof['negative']==0 and proof['samples']==rm['native_samples']
    ref=(rm,rr,rs)
authority=ref[1] if ref else raw; samples=meta['native_samples'] if native else 16
def record(data,arm,pr,pixel,sid): return struct.unpack_from('<4I10f2I',data,arm*STRIDE+((pr*64+pixel)*16+sid)*64)
def physical(data,arm,pr,pixel,sid): return struct.unpack_from('<2I',data,arm*STRIDE+LB+((pr*64+pixel)*16+sid)*8)
def header(data,arm): return struct.unpack_from('<64I',data,arm*STRIDE+LB+PB)
def f(bits): return struct.unpack('<f',struct.pack('<I',bits))[0]
errors=[]; error_count=0; classes={}
def fail(kind,detail):
    global error_count
    error_count+=1; classes[kind]=classes.get(kind,0)+1
    if len(errors)<20: errors.append({'kind':kind,'detail':detail})
geometry=[[sum(1<<sid for sid in range(16) if record(authority,1,pr,pixel,sid)[0]) for pixel in range(64)] for pr in range(6)]
for pair in range(3):
    for pixel in range(64):
        x,y=geometry[2*pair][pixel],geometry[2*pair+1][pixel]
        if (x|y)!=(1<<samples)-1 or x&y: fail('reference_complementarity',[pair,pixel,x,y])
totals=[0]*16; physical_total=0; raw_physical_cov_errors=0; max_interp=[0.0]*10; max_pull=[0.0]*8
native_audit=0; vertices=[]; upper_witness=[0]*8; lower_witness=[0]*8
for sid in range(8):
    if samples==16:
        for row in geometry:
            for g in row:
                bits=g&((1<<sid)|(1<<(sid+8)))
                upper_witness[sid]+=bits==1<<(sid+8); lower_witness[sid]+=bits==1<<sid
        if not upper_witness[sid] or not lower_witness[sid]: fail('missing_geometric_pair_witness',sid)
for arm,mask in enumerate(MASKS):
    h=header(raw,arm); arm_logical=0; arm_physical=0; arm_upper=0
    if h[31]: fail('guard_error',[arm,h[31]])
    if any(h[32:]): fail('reserved_header_nonzero',arm)
    vertices.append(list(h[:18]))
    if ref and list(h[:18])!=list(header(authority,arm)[:18]): fail('vertex_work_changed',arm)
    if arm==1 and any(c==0 for c in h[:18]): fail('missing_full_mask_vertex_side_effect',list(h[:18]))
    for pr in range(6):
        vv=meta['vertices'][3*pr:3*pr+3]; (ax,ay),(bx,by),(cx,cy)=[v['screen'] for v in vv]
        denominator=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy)
        for pixel in range(64):
            c=geometry[pr][pixel]&mask; folded=(c&255)|(c>>8)
            for sid in range(16):
                r=record(raw,arm,pr,pixel,sid); count,cov,checksum,front=r[:4]; vals=r[4:14]; owner,reserved=r[14:]
                expect=int(bool(c&(1<<sid)))
                if count!=expect: fail('logical_count',[arm,pr,pixel,sid,count,expect])
                if cov!=(1<<sid if expect else 0): fail('logical_api_coverage',[arm,pr,pixel,sid,cov])
                if checksum!=count*((pr+1)*257+sid+1): fail('logical_checksum',[arm,pr,pixel,sid])
                if count>1: fail('multiple_writers',[arm,pr,pixel,sid,count])
                if not count:
                    if any(r): fail('inactive_payload',[arm,pr,pixel,sid])
                else:
                    totals[sid]+=count; arm_logical+=count; arm_upper+=count if sid>=8 else 0
                    if owner!=(sid if native else sid%8) or reserved: fail('owner_or_reserved',[arm,pr,pixel,sid,owner,reserved])
                    if front!=int(denominator>0): fail('front_face',[arm,pr,pixel,sid,front])
                    if not all(math.isfinite(v) for v in vals): fail('nonfinite',[arm,pr,pixel,sid]); continue
                    if sid>=samples: fail('sample_id_out_of_range',[arm,pr,pixel,sid]); continue
                    dx,dy=POS[samples][sid]; x,y=pixel%8+dx/16,pixel//8+dy/16
                    l0=((by-cy)*(x-cx)+(cx-bx)*(y-cy))/denominator
                    l1=((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/denominator; weights=[l0,l1,1-l0-l1]
                    reciprocal=sum(l/v['clip'][3] for l,v in zip(weights,vv))
                    z=sum(l*v['clip'][2]/v['clip'][3] for l,v in zip(weights,vv)); w=1/reciprocal
                    affine=[sum(l*v['affine'][i] for l,v in zip(weights,vv)) for i in range(2)]
                    projective=[sum(l*v['projective'][i]/v['clip'][3] for l,v in zip(weights,vv))/reciprocal for i in range(2)]
                    oracle=[x,y,z,w]+affine+projective+[affine[0]*projective[1]+z,projective[0]+affine[1]*w]
                    diff=[abs(q-e) for q,e in zip(vals,oracle)]; max_interp=[max(q,e) for q,e in zip(max_interp,diff)]
                    if diff[0] or diff[1] or any(d>1e-5 for d in diff[2:]): fail('logical_interpolation_or_body',[arm,pr,pixel,sid,diff])
                    if ref:
                        expected_record=record(authority,arm,pr,pixel,sid)
                        if r[:4]!=expected_record[:4] or any(abs(q-e)>1e-5 for q,e in zip(vals,expected_record[4:14])):
                            fail('native_reference_payload',[arm,pr,pixel,sid])
                pc,rawcov=physical(raw,arm,pr,pixel,sid)
                if native: pe=int(bool(c&(1<<sid))); ce=(1<<sid) if pe else 0
                elif program=='paired': pe=int(sid<8 and bool(folded&(1<<sid))); ce=c if pe else 0
                else: pe=int(sid==0 and bool(c)); ce=c if pe else 0
                if pc!=pe: fail('physical_execution_frequency',[arm,pr,pixel,sid,pc,pe])
                if rawcov!=ce:
                    raw_physical_cov_errors+=1; fail('physical_raw_coverage',[arm,pr,pixel,sid,rawcov,ce])
                arm_physical+=pc
    if h[18]!=arm_physical or h[29]!=arm_logical or h[30]!=arm_upper: fail('header_totals',[arm,list(h[18:21]),h[29],h[30],arm_physical,arm_logical,arm_upper])
    physical_total+=arm_physical; native_audit+=h[19]
    pulls=[f(v) for v in h[21:29]]; max_pull=[max(q,e) for q,e in zip(max_pull,pulls)]
    if native:
        if h[19]!=arm_physical or h[20] or any((bits&0x80000000) or not math.isfinite(v) or not 0<=v<=1e-5 for bits,v in zip(h[21:29],pulls)): fail('native_own_offset_pull',[arm,h[19],h[20],pulls])
    elif h[19] or h[20] or any(h[21:29]): fail('unexpected_native_audit_in_paired',arm)
result={'status':'PASS' if not error_count else 'FAIL','program':program,'negative':meta['negative'],'samples':samples,
        'scope':'Manual paired-body diagnostic only. Hostpaired is native16 pixel-frequency surrogate, not AMD scheduling proof.',
        'raw_bytes':len(raw),'raw_sha256':sha(raw),'analyzer_sha256':sha(Path(__file__).read_bytes()),
        'metadata_sha256':sha(Path(str(a.prefix)+'.json').read_bytes()),
        'client_sha256':next(i['sha256'] for i in run['artifacts'] if i['path']=='s16f8-client.exe'),
        'engine_sha256':run['expected_engine_sha256'],'reference_raw_sha256':sha(authority) if ref else None,
        'logical_counts':totals,'logical_total':sum(totals),'physical_total':physical_total,
        'physical_raw_coverage_errors':raw_physical_cov_errors,'native_own_offset_audits':native_audit,
        'max_own_offset_pull_error':max_pull,'max_logical_interpolation_error':max_interp,
        'upper_only_witnesses':upper_witness,'lower_only_witnesses':lower_witness,
        'vertex_counts_by_arm':vertices,'error_count':error_count,'error_classes':classes,'first_errors':errors}
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ('first_errors','vertex_counts_by_arm')},indent=2))
raise SystemExit(bool(error_count))
