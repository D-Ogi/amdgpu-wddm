static void case_retained_graph_capture(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 fence[16];
 static unsigned char memory[4][4096],expected[3][4096];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned kind,partial,i,j,k;u64 physical[3];u32 commands[1024];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=384;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"captured graph window");
 for(kind=0;kind<3;kind++)for(partial=0;partial<2;partial++) {
  PAGING_CAPTURE_OWNER owner={0};PAGING_GRAPH_CAPTURE* c=NULL;PAGING_GRAPH_BATCH batch;
  unsigned band=0,action=0,nextBand=0,nextAction=0,passes=0;
  unsigned start=partial?17:0,bytes=partial?8192+17:12288;int result;
  physical[0]=kind==2?0x200010000ull:0x100123000ull;physical[1]=0x200020000ull;physical[2]=0x200030000ull;
  virtual_graph_map=1;virtual_graph_local_mask=kind==2?63:kind==1?30:0;
  for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[(i+1)%3];}
  check(GfxPagingCaptureVirtualGraph(&d,4096,16*4096+start,19*4096+start,bytes,&c)==STATUS_SUCCESS && c,
        "capture accepts system, mixed or local virtual cycle");
  if(!c)continue;
  check(c->PageCount==3 && c->Identities==3 && c->Owner.Root==4096 && c->Owner.Bytes==bytes,
        "capture retains complete operation identity");
  for(i=0;i<3;i++)check(c->Pages[c->SourceIndex[i]]==physical[i] && c->Pages[c->DestinationIndex[i]]==physical[(i+1)%3] &&
       c->SystemPages[c->SourceIndex[i]]==(kind==0 || (kind==1 && i==0)),"capture preserves physical addresses and access domain");
  check(PagingCaptureAttach(&owner,&c->Owner) && PagingCaptureFind(&owner,c->Owner.Token)==&c->Owner,"context owns captured immutable arrays");
  // Every attempted VA lookup now fails. Batches must use captured identities.
  virtual_graph_map=0;translationOk=0;
  for(i=0;i<6;i++)virtual_graph_pages[i]=0xABC000+(u64)i*4096;
  for(i=0;i<4;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<bytes;i++)expected[((start+i)/4096+1)%3][(start+i)%4096]=memory[(start+i)/4096][(start+i)%4096];
  check(GfxPagingPlanCapturedGraph(c,0,0,0,&batch,&nextBand,&nextAction)==PagingPermutationNeedCycle &&
        !batch.Count && !nextBand && !nextAction && !c->Owner.Band && !c->Owner.Action,
        "capacity refusal leaves captured progress unchanged");
  memset(commands,0xCC,sizeof(commands));
  check(GfxPagingEmitCapturedGraph(&d,c,0,0,commands,64,0,&written,&batch,&nextBand,&nextAction)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
        !written && !batch.Count && !nextBand && !nextAction && all_pattern(commands,1024,0xCCCCCCCCu),
        "captured emitter refuses empty capacity without bytes or progress");
  do {
   unsigned balance=0,word=0;
   memset(memory[3],0xCD,sizeof(memory[3]));memset(commands,0xCC,sizeof(commands));
   status=GfxPagingEmitCapturedGraph(&d,c,band,action,commands,64,sizeof(commands),&written,&batch,&nextBand,&nextAction);
   result=nextBand==c->BandCount?PagingPermutationDone:PagingPermutationMore;
   check(status==STATUS_SUCCESS && batch.Count && written && written<=1024 &&
         (nextBand!=band || nextAction!=action) && !c->Owner.Band && !c->Owner.Action,
         "captured packets use changed-VA-independent addresses and propose progress without committing it");
   if(status!=STATUS_SUCCESS || !batch.Count || !written || written>1024)break;
   for(k=0;k<batch.Count;k++) {
    u32* packet=commands+word;unsigned mapped,words,n,from=4,to=4,fromOffset=0,toOffset=0;
    u64 sm,dm,sp=0,dp=0;unsigned sourceSystem,destinationSystem;
    if(word>=written)break;
    mapped=packet[0]!=SDMA_PKT_HEADER_OP(SDMA_OP_COPY);words=mapped?83:7;
    check(words<=written-word,"captured packet fits emitted range");if(words>written-word)break;
    if(mapped) {
     sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
     dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
     sm=(u64)packet[36]|((u64)packet[37]<<32);dm=(u64)packet[38]|((u64)packet[39]<<32);n=packet[34]+1;
     check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY),"captured mapped packet includes actual copy");
    } else {sm=(u64)packet[3]|((u64)packet[4]<<32);dm=(u64)packet[5]|((u64)packet[6]<<32);n=packet[1]+1;}
    sourceSystem=sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096;
    destinationSystem=dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096;
    if(sm==gfx.PagingCopyStaging.mc){from=3;check(balance==1,"captured restore has current-batch save");balance--;}
    else {
     u64 pa=sourceSystem?sp+sm-gfx.PagingWindow.mc:sm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     fromOffset=(unsigned)(pa&4095);for(i=0;i<3;i++)if((pa&~4095ull)==physical[i])from=i;
    }
    if(dm==gfx.PagingCopyStaging.mc){to=3;balance++;}
    else {
     u64 pa=destinationSystem?dp+dm-gfx.PagingWindow.mc-4096:dm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     toOffset=(unsigned)(pa&4095);for(i=0;i<3;i++)if((pa&~4095ull)==physical[i])to=i;
    }
    check(from<4 && to<4 && from!=to && n==batch.Bytes && n<=4096-fromOffset && n<=4096-toOffset,
          "captured actual COPY addresses identify bounded original physical bytes");
    if(from>=4 || to>=4 || from==to || n>4096-fromOffset || n>4096-toOffset)break;
    check((batch.Moves[k].source==PAGING_PERMUTATION_SCRATCH || sourceSystem==batch.SystemPages[batch.Moves[k].source]) &&
          (batch.Moves[k].destination==PAGING_PERMUTATION_SCRATCH || destinationSystem==batch.SystemPages[batch.Moves[k].destination]),
          "captured packet access domains match associated metadata descriptor");
    memcpy(memory[to]+toOffset,memory[from]+fromOffset,n);word+=words;
   }
   check(k==batch.Count && word==written && balance==0,"captured batch accounts for all packet bytes and completes scratch ownership");
   check(all_pattern(commands+written,1024-written,0xCCCCCCCCu),"captured emitter preserves unused DMA storage");
   band=nextBand;action=nextAction;passes++;
  }while(result==PagingPermutationMore && passes<16);
  check(result==PagingPermutationDone && !memcmp(memory,expected,sizeof(expected)),"captured cycle preserves initial bytes after mapping changes");
  check(PagingCaptureDetach(&owner,c->Owner.Token)==&c->Owner && !owner.Head,"completed capture relinquishes owner before release");
  ExFreePoolWithTag(c,c->Owner.PoolTag);translationOk=1;
 }
 virtual_graph_map=0;virtual_graph_local_mask=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
 check(!shadow_pool_live && !flush_lock_depth && !flush_region_depth,"captured graph storage and engine readers released");
}
