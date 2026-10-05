static void case_graph_shadow_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE entries[512];
 DXGKARG_BUILDPAGINGBUFFER build={0};BC250_PAGING_ENDPOINT src={0},dst={0};
 PAGING_GRAPH_BATCH graph={0};PAGING_PAGE_MOVE moves[9];
 unsigned source[3]={0,1,2},destination[3]={1,2,0},readers[3],writer[3],queue[3],forward[3],n=0;
 unsigned char system[3]={0,0,0};u64 pages[3],base=0x200000000ull;
 static u64 memory[4][512],initial[3][512];
 u32 commands[128],dma[128],priv[160];unsigned i,j,k,used=0;ULONG written;NTSTATUS status;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(LONGLONG)base;
 d.VramMcBase=0x100000000ull;d.VramLength=8ull<<30;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"graph publication reserves actual VidMm state");
 for(i=0;i<3;i++) {
  pages[i]=base+(4ull+i)*4096;
  for(j=0;j<512;j++){entries[j].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;entries[j].PageAddress=0x100+i*512+j;}
  update.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;update.PageTableLevel=0;
  update.PageTableAddress.CpuVirtual=cpu_storage+(4+i)*512;update.NumPageTableEntries=512;update.pPageTableEntries=entries;
  check(VidMmUpdatePageTable(&update)==STATUS_SUCCESS,"graph publication initializes source table bytes");
  memcpy(initial[i],cpu_storage+(4+i)*512,4096);memcpy(memory[i],initial[i],4096);
 }
 check(PagingPageGraphPlan(source,destination,3,3,readers,writer,queue,forward,moves,9,9,&n) && n==4,
       "graph publication obtains actual cyclic physical plan");
 src.Length=dst.Length=4096;
 for(i=0;i<n;i++) {
  src.Address=moves[i].source==PAGING_PERMUTATION_SCRATCH?gfx.PagingCopyStaging.mc:pages[moves[i].source]-base+d.VramMcBase;
  dst.Address=moves[i].destination==PAGING_PERMUTATION_SCRATCH?gfx.PagingCopyStaging.mc:pages[moves[i].destination]-base+d.VramMcBase;
  status=GfxPagingBuildCopyPage(&d,&src,&dst,0,4096,commands+used,64+used*4,sizeof(commands)-used*4,&written,
                               &(BC250_PAGING_COPY_SLICE){0});
  check(status==STATUS_SUCCESS && written==7,"graph publication builds actual direct-copy packet per action");
  if(status!=STATUS_SUCCESS || written!=7)break;
  used+=written;
 }
 graph.Pages=pages;graph.SystemPages=system;graph.Identities=3;graph.Moves=moves;graph.Count=n;graph.Bytes=4096;
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,used*4);memset(dma,0xCC,sizeof(dma));
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaBufferWriteOffset=64;
 for(i=0;i<2;i++) {
  build.DmaSize=i?sizeof(dma):used*4-1;
  build.DmaBufferPrivateDataSize=i?PAGING_PRIVATE_HEADER_BYTES+used*4-1:sizeof(priv);
  check(WddmPublishPagingRecordCore(&build,used,FALSE,0,0,0,0,0,0,0,0,NULL,&graph)==STATUS_INVALID_PARAMETER &&
        build.pDmaBuffer==dma && build.pDmaBufferPrivateData==priv && all_pattern(dma,128,0xCCCCCCCCu),
        "graph publication rejects short capacity before publishing commands");
  for(j=0;j<3;j++)for(k=0;k<512;k++){u64 v=0;check(PagingPtShadowRead(&g_VidMm.Shadow,pages[j],k,&v)==PAGING_PT_OK && v==initial[j][k],
        "capacity refusal leaves complete logical table snapshot unchanged");}
 }
 build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmPublishPagingRecordCore(&build,used,FALSE,0,0,0,0,0,0,0,0,NULL,&graph)==STATUS_SUCCESS &&
       build.pDmaBuffer==dma+used && build.DmaSize==sizeof(dma)-used*4 && !memcmp(dma,commands,used*4),
       "graph publication commits metadata and publishes identical accepted packet bytes");
 check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+used*4,64,used*4,0,NULL,NULL),
       "graph publication private range matches command bytes");
 for(i=0;i<n;i++) {
  u32* cmd=dma+i*7;u64 from=(u64)cmd[3]|((u64)cmd[4]<<32),to=(u64)cmd[5]|((u64)cmd[6]<<32);
  unsigned si=4,di=4;
  if(from==gfx.PagingCopyStaging.mc)si=3;if(to==gfx.PagingCopyStaging.mc)di=3;
  for(j=0;j<3;j++){if(from==pages[j]-base+d.VramMcBase)si=j;if(to==pages[j]-base+d.VramMcBase)di=j;}
  check(cmd[0]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && cmd[1]+1==4096 && si<4 && di<4 && si!=di,
        "graph publication replay decodes actual packet physical endpoints");
  if(si>=4 || di>=4 || si==di)break;
  memcpy(memory[di],memory[si],4096);
 }
 for(i=0;i<3;i++)for(j=0;j<512;j++) {
  u64 v=0;
  check(PagingPtShadowRead(&g_VidMm.Shadow,pages[i],j,&v)==PAGING_PT_OK && v==initial[(i+2)%3][j] && v==memory[i][j],
        "published logical table cycle matches independently replayed GPU bytes");
  check(cpu_storage[(4+i)*512+j]==initial[i][j],"logical publication does not write live table backing");
 }
 VidMmStop();check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth,"graph publication lifetime and lock fully released");
}
