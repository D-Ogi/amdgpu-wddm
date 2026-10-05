from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]);out=Path(sys.argv[2])
sources={n:(root/n).read_text(encoding='utf-8') for n in ['reference/sdma_v5_0.c','reference/gmc_v10_0.c','gfxhub_v2_0.c']}
def extract(src,name):
 at=src.index(name+'(');start=src.rfind('static ',0,at);opening=src.index('{',at);end=opening+1;depth=1
 while depth:depth+=(src[end]=='{')-(src[end]=='}');end+=1
 return src[start:end]
sdma=sources['reference/sdma_v5_0.c'];gmc=sources['reference/gmc_v10_0.c']
body='\n\n'.join(extract(sdma,n) for n in ['sdma_v5_0_ring_emit_wreg','sdma_v5_0_ring_emit_reg_wait','sdma_v5_0_ring_emit_reg_write_reg_wait'])
body=body.replace('sdma_v5_0_ring_emit_','reference_vm_')
body+='\n\n'+extract(gmc,'gmc_v10_0_emit_flush_gpu_tlb').replace('gmc_v10_0_emit_flush_gpu_tlb','reference_vm_flush',1).replace('ring->vm_hub','AMDGPU_GFXHUB(0)').replace('ring->vm_inv_eng','0u')
body+='\n\n'+extract(sources['gfxhub_v2_0.c'],'gfxhub_v2_0_get_invalidate_req').replace('gfxhub_v2_0_get_invalidate_req','reference_invalidate_req',1)
prefix='''#pragma warning(push)
#pragma warning(disable:4245)
/* PROVENANCE: Linux v6.18 AMD amdgpu, MIT. */
#define amdgpu_ring_emit_wreg reference_vm_wreg
#define amdgpu_ring_emit_reg_wait reference_vm_reg_wait
#define amdgpu_ring_emit_reg_write_reg_wait reference_vm_reg_write_reg_wait
#define gmc_v10_0_use_invalidate_semaphore(adev,hub) false
'''
suffix='''
#pragma warning(pop)
#undef amdgpu_ring_emit_wreg
#undef amdgpu_ring_emit_reg_wait
#undef amdgpu_ring_emit_reg_write_reg_wait
#undef gmc_v10_0_use_invalidate_semaphore
'''
out.write_text(prefix+body+suffix,encoding='utf-8')
out.with_suffix('.json').write_text(json.dumps({'sources':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in sources},'adaptation':'function names; GFXHUB engine0 selection; use_semaphore false for this hub; packet and request bodies unchanged'},indent=2)+'\n',encoding='utf-8')
