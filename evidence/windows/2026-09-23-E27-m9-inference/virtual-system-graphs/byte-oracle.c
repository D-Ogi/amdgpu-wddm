/* Actual virtual DDI byte oracle, including a cycle split into independently
 * submitted scratch-complete groups and partial ranges with untouched bytes. */
static void case_virtual_alias_ordering(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16];
 static unsigned char memory[7][4096],expected[6][4096];
 const u64 physical[6]={0x100123000ull,0x300789000ull,0x200456000ull,
                       0x400abc000ull,0x500def000ull,0x600987000ull};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned scenario,limit,i,j,k;u32 dma[1024],priv[1100];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"virtual graph temporary window");
 virtual_graph_map=1;
 for(scenario=0;scenario<3;scenario++)for(limit=0;limit<2;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 moved=0,total=0;unsigned passes=0;
  unsigned start=scenario==2?17:0,bytes=scenario==2?8192+17:12288;
  NTSTATUS status;
  ring.max_dw=limit?384:1024;
  for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[scenario?(i+1)%3:3+i];}
  for(i=0;i<7;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<bytes;i++) {
   unsigned page=(start+i)/4096,offset=(start+i)%4096;
   expected[scenario?(page+1)%3:3+page][offset]=memory[page][offset];
  }
  for(i=0;i<6;i++) {
   ULONGLONG pa=0;BOOLEAN system=FALSE;
   check(VidMmTranslatePaging(4096,(16ull+i)*4096+123,&pa,&system) && system &&
         pa==virtual_graph_pages[i]+123,"virtual graph translation positive control preserves page and offset");
  }
  b.TransferVirtual.SourceVirtualAddress=16*4096+start;b.TransferVirtual.DestinationVirtualAddress=19*4096+start;
  b.TransferVirtual.TransferSizeInBytes=bytes;
  do {
   unsigned words,balance=0,old=b.MultipassOffset;
   b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(memory[6],0xCD,sizeof(memory[6]));
   status=WddmBuildVirtualTransfer(&d,4096,&b,&moved);words=(unsigned)((u32*)b.pDmaBuffer-dma);
   check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && words && words%83==0 &&
         words<=1024 && b.MultipassOffset!=old && b.DmaSize==sizeof(dma)-words*4,
         "virtual graph emits productive bounded prefix with exact DMA accounting");
   if((status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) || !words || words%83 || words>1024)break;
   check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),0,words*4,0,NULL,NULL),
         "virtual graph records cover published packets");
   for(k=0;k<words;k+=83) {
    u32* packet=dma+k;
    u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
    u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
    u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
    unsigned si=7,di=7,so=0,off=0,n=packet[34]+1;
    if(sm==gfx.PagingCopyStaging.mc){si=6;check(balance==1,"virtual restore follows save in this buffer");balance--;}
    else if(sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096){so=(unsigned)(sm-gfx.PagingWindow.mc);for(i=0;i<6;i++)if(sp==physical[i])si=i;}
    if(dm==gfx.PagingCopyStaging.mc){di=6;balance++;}
    else if(dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096){off=(unsigned)(dm-gfx.PagingWindow.mc-4096);for(i=0;i<6;i++)if(dp==physical[i])di=i;}
    check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && n<=4096-so && n<=4096-off && si<7 && di<7 && si!=di,
          "virtual graph decodes physical copies and bounded scratch transactions");
    if(si>=7 || di>=7 || si==di || n>4096-so || n>4096-off)break;
    memcpy(memory[di]+off,memory[si]+so,n);
   }
   check(k==words && balance==0,"virtual graph scratch is complete before another job may overwrite it");
   check(all_pattern(dma+words,1024-words,0xCCCCCCCCu),"virtual graph preserves unused DMA storage");
   total+=moved;passes++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && passes<16);
  check(status==STATUS_SUCCESS && total==bytes && b.MultipassOffset==3 && (!limit || !scenario || passes>1),
        "virtual graph finishes once across bounded submissions");
  check(!memcmp(memory,expected,sizeof(expected)),scenario?
        "virtual cross-page cycle preserves initial source bytes":
        "virtual disjoint positive control preserves initial source bytes");
  {PVOID after=b.pDmaBuffer;ULONG remaining=b.DmaSize;
   check(WddmBuildVirtualTransfer(&d,4096,&b,&moved)==STATUS_SUCCESS && !moved && b.pDmaBuffer==after && b.DmaSize==remaining,
         "virtual completed token does not emit duplicate work");}
 }
 virtual_graph_map=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
 check(!flush_lock_depth && !flush_region_depth,"virtual graph releases engine lifetime ownership");
}
