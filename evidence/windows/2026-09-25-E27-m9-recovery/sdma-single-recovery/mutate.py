from pathlib import Path
root=Path(r'P:\bc-250\scratch\m9\sdma-single-recovery')
s=Path(r'P:\bc-250\bc250-win\driver\shim\bc250_sdma.c').read_text()
mutations={
 'omit-clear':(' amdgpu_ring_clear_ring(ring);\n ring->wptr=0;',' /* negative control: omit discard */\n ring->wptr=0;'),
 'wrong-engine':('result=bc250_sdma_soft_reset_instance(adev,instance);','result=bc250_sdma_soft_reset_instance(adev,instance^1u);'),
 'allow-adoption':('bc250_sdma_gfx_resume_instance(adev,(int)instance,true)','bc250_sdma_gfx_resume_instance(adev,(int)instance,false)')}
for name,(old,new) in mutations.items():
 assert s.count(old)==1,(name,s.count(old))
 (root/(name+'.c')).write_text(s.replace(old,new,1))
