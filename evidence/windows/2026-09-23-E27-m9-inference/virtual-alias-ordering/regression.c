/* Opt-in regression: actual virtual DDI packets must preserve an initial source
 * snapshot when distinct VAs form a physical three-page cycle. A disjoint copy
 * uses the identical translator/packet decoder as a positive control. */
static void case_virtual_alias_ordering(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16];
 static unsigned char memory[6][4096],expected[6][4096];
 const u64 physical[6]={0x100123000ull,0x300789000ull,0x200456000ull,
                       0x400abc000ull,0x500def000ull,0x600987000ull};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned cycle,i,j,k;u32 dma[1024],priv[1100];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"virtual graph temporary window");
 virtual_graph_map=1;
 for(cycle=0;cycle<2;cycle++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 moved=0;unsigned words;NTSTATUS status;
  for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[cycle?(i+1)%3:3+i];}
  for(i=0;i<6;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<3;i++)memcpy(expected[cycle?(i+1)%3:3+i],memory[i],4096);
  for(i=0;i<6;i++) {
   ULONGLONG pa=0;BOOLEAN system=FALSE;
   check(VidMmTranslatePaging(4096,(16ull+i)*4096+123,&pa,&system) && system &&
         pa==virtual_graph_pages[i]+123,"virtual graph translation positive control preserves page and offset");
  }
  b.TransferVirtual.SourceVirtualAddress=16*4096;b.TransferVirtual.DestinationVirtualAddress=19*4096;
  b.TransferVirtual.TransferSizeInBytes=3*4096;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildVirtualTransfer(&d,4096,&b,&moved);words=(unsigned)((u32*)b.pDmaBuffer-dma);
  check(status==STATUS_SUCCESS && moved==12288 && b.MultipassOffset==3 && words==3*83,
        "virtual three-page transfer publishes complete current packet stream");
  if(status!=STATUS_SUCCESS || words!=3*83)continue;
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),0,words*4,0,NULL,NULL),
        "virtual graph records cover published packets");
  for(k=0;k<words;k+=83) {
   u32* packet=dma+k;
   u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
   u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
   unsigned si=6,di=6;
   for(i=0;i<6;i++){if(sp==physical[i])si=i;if(dp==physical[i])di=i;}
   check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[34]+1==4096 && si<6 && di<6 && si!=di,
         "virtual graph decodes actual mapped-copy packets");
   if(si>=6 || di>=6 || si==di)break;
   memcpy(memory[di],memory[si],4096);
  }
  check(k==words && !memcmp(memory,expected,sizeof(memory)),cycle?
        "virtual cross-page cycle preserves initial source bytes":
        "virtual disjoint positive control preserves initial source bytes");
 }
 virtual_graph_map=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
 check(!flush_lock_depth && !flush_region_depth,"virtual graph releases engine lifetime ownership");
}
