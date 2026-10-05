static PAGING_PT_SHADOW_SLOT route_shadow_slots[4];

static void case_kmd_routes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 BC250_DEVICE dev={{0x200000000ll},0x100000000ull,0x100000,NULL,0,0,0,0};
 BC250_GFX gfx={1,{0,0},&g_adev,0,NULL,0};BC250_PAGING_STREAM st;
 u32 buf[256];u64 out;unsigned written;static u64 scratch[16];
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"actual route window configured");
 memset(&st,0,sizeof(st));st.Device=&dev;st.Gfx=&gfx;st.Root=4096;st.Payload=buf;st.CommandOffset=128;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=0x12345123ull;isSystem=1;
 check(PagingResolve(&st,0,64,&out) && out==(translated|PAGING_SYSTEM_ADDRESS),"actual resolver preserves system physical address and offset");
 check(PagingEmit(&st,buf,256,out,0x100000200ull,64,&written)==0 && written==83,"system to local builds mapped transfer");
 check(buf[8+3]==97 && buf[33+3]==(u32)(gfx.PagingWindow.mc+0x123) && buf[33+5]==0x200,"actual route marker includes prior command offset and uses source window");
 check(PagingEmit(&st,buf,256,0x100000200ull,out,64,&written)==0 && buf[33+5]==(u32)(gfx.PagingWindow.mc+4096+0x123),"local to system uses destination window");
 check(PagingEmit(&st,buf,256,out,out,64,&written)==0 && buf[4]!=0 && buf[6]!=0,"system to system maps two separate slots");
 st.Fill=1;st.Pattern=0xABCD1234;out=(0x12345200ull|PAGING_SYSTEM_ADDRESS);
 check(PagingEmit(&st,buf,256,0,out,64,&written)==0 && written==81 && buf[33]==SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL) && buf[36]==st.Pattern,"system fill uses mapped transaction and preserves pattern");
 st.Fill=0;isSystem=0;translated=0x200000200ull;
 check(PagingResolve(&st,0,64,&out) && out==0x100000200ull,"local resolver retains physical-to-MC conversion");
 check(PagingEmit(&st,buf,256,out,out+64,64,&written)==0 && written==7,"local/local keeps direct packet path");
 isSystem=1;translated=0x1000000000000000ull;
 check(!PagingResolve(&st,0,64,&out),"unrepresentable system page rejected instead of truncation");
 translated=0x12345000;gfx.PagingWindowReady=0;check(!PagingResolve(&st,0,64,&out),"unavailable aperture rejected");
 {
  unsigned done=0,next,dw,totalDw=0,passes=0;int result;
  gfx.PagingWindowReady=1;fragmented=1;isSystem=1;st.CommandOffset=0;st.Fill=0;
  do {
   st.Payload=buf;
   result=PagingStreamBuild(&st,PagingResolve,PagingEmit,0,0x10000,0x30000,5*4096,done,buf,180,&dw,&next);
   check(result==PagingStreamMore || result==PagingStreamDone,"actual mapped page stream publishes valid partial batch");
   check(next>done && next<=5*4096 && dw==83*((next-done)/4096),"mapped multipass preserves data and command coordinates");
   if(dw==166) check(buf[8+3]==1 && buf[83+8+3]==250,"consecutive transactions use distinct command-position marker ranges");
   totalDw+=dw;done=next;passes++;
  }while(result==PagingStreamMore && passes<10);
  check(done==5*4096 && totalDw==5*83 && passes==3,"five scattered page pairs covered exactly in three bounded batches");
  fragmented=0;
 }
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_kmd_flush(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 u32 buf[32];ULONG written;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned offset;NTSTATUS status;
 dev.Gfx=&gfx;gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 test_expected_vmid=1;memset(buf,0xCC,sizeof(buf));
 status=GfxPagingBuildFlush(&dev,1,buf,0,sizeof(buf),&written,&unsupported);
 check(status==STATUS_SUCCESS && written==15 && unsupported==BC250PagingSupported,"actual KMD flush builds VMID1 packet");
 check(buf[12]==2 && buf[13]==2 && all_pattern(buf+15,17,0xCCCCCCCCu),"actual KMD flush targets app VMID not GART");
 for(offset=0;offset<=4096;offset+=4) {
  unsigned cap=PagingStreamCapacity(64,offset,65536,1024,ring.funcs->align_mask,bc250_sdma_fence_size(&ring,AMDGPU_FENCE_FLAG_INT));
  memset(buf,0xCC,sizeof(buf));
  status=GfxPagingBuildFlush(&dev,1,buf,offset,64,&written,&unsupported);
  check(cap>=16 ? status==STATUS_SUCCESS && written==15 : status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && written==0,"flush accounts for earlier DMA commands and outer fence");
  if(cap<16)check(all_pattern(buf,32,0xCCCCCCCCu),"insufficient flush publishes no partial packet");
 }
 gfx.PagingReady=0;written=123;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written && unsupported==BC250PagingNotReady,"not-ready remains explicit helper failure");
 dev.Gfx=NULL;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written,"missing engine refuses flush");
 check(GfxPagingBuildFlush(&dev,0,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"application flush refuses GART VMID");
 check(GfxPagingBuildFlush(&dev,16,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"invalid VMID rejected");
 check(GfxPagingBuildFlush(&dev,1,buf,1,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"unaligned DMA offset rejected");
 check(!flush_lock_depth && !flush_region_depth,"all flush exits balance lifetime lock/region");
 test_expected_vmid=0;memset(hub,0,sizeof(*hub));
}

static void case_kmd_updates(void)
{
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE ptes[512];
 u32 buf[1056];ULONGLONG encoded[512],physical;static u64 scratch[16];
 ULONG written,next;unsigned i;BC250_WDDM_PAGING_UNSUPPORTED unsupported;NTSTATUS rc;
 dev.VramPhysical.QuadPart=0x200000000ll;dev.VramMcBase=0x100000000ull;dev.VramLength=0x1000000;dev.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;g_VidMm.Ready=1;g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);g_VidMm.SegmentPhysical=0x200000000ull;g_VidMm.SegmentLength=0x1000000;
 g_VidMm.Pte.units=BC250_PTE_ADDR_PAGES;g_VidMm.Pte.aperture=BC250_PTE_VM;
 g_VidMm.Pte.system_segment=0;g_VidMm.Pte.vram_segment=1;
 g_VidMm.Pte.vram_base=g_VidMm.SegmentPhysical;g_VidMm.Pte.vram_size=g_VidMm.SegmentLength;
 update.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;update.NumPageTableEntries=512;
 update.PageTableAddress.GpuPhysical.SegmentId=1;update.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 update.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 check(VidMmEncodePageTable(&update,0,512,&physical,encoded) && physical==0x200001000ull,"actual encoder translates full PTE source and table physical address");
 memset(buf,0xCC,sizeof(buf));g_VidMm.GpuCalls=0;
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported);
 check(rc==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480 && written==974,"actual builder publishes480entries then requests next buffer");
 check(buf[1]==0x1000 && buf[2]==1 && buf[3]==959,"PTE table destination converted to MC");
 for(i=0;i<480;i++)if(buf[4+2*i]!=(u32)encoded[i] || buf[5+2*i]!=(u32)(encoded[i]>>32))break;
 check(i==480 && !g_VidMm.GpuCalls,"queued first batch preserves values without CPU table write");
 check(buf[967]==1 && all_pattern(buf+974,82,0xCCCCCCCCu),"first update barrier marker and tail bounded");
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,next,&written,&next,&unsupported);
 check(rc==STATUS_SUCCESS && next==512 && written==78 && buf[1]==0x1F00 && buf[3]==63,"second batch resumes table offset by480entries");
 for(i=0;i<32;i++)if(buf[4+2*i]!=(u32)encoded[480+i] || buf[5+2*i]!=(u32)(encoded[480+i]>>32))break;
 check(i==32,"second batch covers exactly remaining entries");
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,4048,65536,0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !next && all_pattern(buf,1056,0xCCCCCCCCu),"insufficient accumulated ring budget writes nothing");
 update.NumPageTableEntries=2;update.StartIndex=17;update.Flags.Repeat=1;
 check(GfxPagingBuildUpdate(&dev,&update,buf,128,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && next==2 && buf[1]==0x1088 && buf[4]==buf[6] && buf[5]==buf[7] && buf[11]==97,"Repeat and StartIndex preserved, marker includes command offset");
 update.Flags.Repeat=0;update.StartIndex=0;update.NumPageTableEntries=512;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_INVALID_PARAMETER && !written && all_pattern(buf,1056,0xCCCCCCCCu),"invalid final source entry prevents first batch publication");
 ptes[511].Flags=ptes[0].Flags;
 gfx.PagingCpuBootstrap=1;gfx.PagingReady=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && !written && next==512 && g_VidMm.GpuCalls==1,"pre-RUN bootstrap uses immediate CPU update");
 gfx.PagingCpuBootstrap=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next && g_VidMm.GpuCalls==1,"after bootstrap close not-ready cannot fall back to CPU");
 update.StartIndex=1;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects table extent overflow");
 update.StartIndex=0;update.PageTableLevel=4;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects invalid page-table level");
 check(!flush_lock_depth && !flush_region_depth,"PTE update exits balance lifetime locks");
}

static void case_cpu_updates(void)
{
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE ptes[512];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};u32 output[16];
 ULONGLONG encoded[512],physical;ULONG written,next;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned i;long long priorWritten;int maps;ULONGLONG shadowEntry;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;
 u.PageTableAddress.GpuPhysical.SegmentOffset=4096;u.NumPageTableEntries=2;u.StartIndex=5;u.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 memset(cpu_table,0xCC,4096);priorWritten=g_VidMm.Written;
 check(VidMmEncodePageTable(&u,0,2,&physical,encoded),"CPU control encoded");
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[5]==encoded[0] && cpu_table[6]==encoded[1] && g_VidMm.Written==priorWritten+2,"actual CPU physical update writes requested entries");
 check(cpu_table[4]==0xCCCCCCCCCCCCCCCCull && cpu_table[7]==0xCCCCCCCCCCCCCCCCull && !cpu_map_live,"CPU update bounds and lifetime preserved");
 memset(cpu_table,0xCC,4096);g_VidMm.SegmentMapping=NULL;priorWritten=g_VidMm.Written;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY && g_VidMm.Written==priorWritten && !cpu_map_live,"missing retained mapping refuses without write");
 dev.Gfx=&gfx;gfx.PagingCpuBootstrap=1;
 check(GfxPagingBuildUpdate(&dev,&u,output,0,sizeof(output),0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next,"bootstrap preserves zero progress after missing retained mapping");
 g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;maps=cpu_map_calls;u.NumPageTableEntries=512;u.StartIndex=0;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_map_calls==maps,"late invalid PTE rejected before destination mapping");
 for(i=0;i<512;i++)if(cpu_table[i]!=0xCCCCCCCCCCCCCCCCull)break;
 check(i==512,"late invalid PTE leaves complete destination unchanged");
 ptes[511].Flags=ptes[0].Flags;u.NumPageTableEntries=3;u.StartIndex=9;u.Flags.Repeat=1;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[9]==encoded[0] && cpu_table[10]==encoded[0] && cpu_table[11]==encoded[0] && cpu_map_calls==maps && !cpu_map_live,"CPU_VIRTUAL Repeat uses provided pointer without map/unmap");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==encoded[0],"CPU_VIRTUAL initialization registers physical identity and encoded entry");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,8,&shadowEntry)==PAGING_PT_MISSING,"partial initialization does not invent adjacent PTEs");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 ptes[0].PageAddress=77;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==cpu_table[9],"immediate bootstrap updates known logical table with CPU table");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 // Fill registration capacity, then ensure a new physical table cannot be partly written.
 { unsigned slot;ULONGLONG value=0;
   for(slot=2;slot<=4;slot++)check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+slot*4096,0,1,&value,1)==PAGING_PT_OK,"fill small host registration capacity");
   u.PageTableAddress.CpuVirtual=cpu_storage+5*512;cpu_storage[5*512+9]=0xCCCCCCCCCCCCCCCCull;
   check(VidMmUpdatePageTable(&u)==STATUS_INSUFFICIENT_RESOURCES && cpu_storage[5*512+9]==0xCCCCCCCCCCCCCCCCull,"registration full refuses before CPU destination write");
 }
 g_VidMm.Write=0;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY,"closed CPU write gate does not claim success");
 g_VidMm.Write=1;u.Flags.Use64KBPages=1;
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER,"unsupported CPU large-page mode rejected");
 check(VidMmUpdatePageTable(NULL)==STATUS_INVALID_PARAMETER,"NULL CPU update refused");
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"failure paths release maps and snapshot/builder locks");
}

static void case_retained_mapping(void)
{
 BC250_DEVICE d={0};ULONGLONG pa,base=0x200000000ull;BOOLEAN system;int calls;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=sizeof(cpu_storage);
 cpu_map_fail=0;cpu_write_setting=1;calls=cpu_map_calls;
 shadow_pool_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !shadow_pool_live && cpu_map_calls==calls,"shadow storage failure before mapping/readiness");
 shadow_pool_fail=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && g_VidMm.Ready && g_VidMm.Write && cpu_map_live==1 && cpu_map_calls==calls+1,"actual VidMm start acquires one retained segment mapping");
 memset(cpu_storage,0,sizeof(cpu_storage));
 cpu_storage[512]=(base+8192)|7;cpu_storage[1024]=(base+12288)|7;cpu_storage[1536]=(base+16384)|7;
 cpu_storage[2048]=(base+24576)|bc250_pte_vm_flags(1,1,0,0);
 calls=cpu_map_calls;
 check(VidMmTranslateRetained(base+4096,0x123,&pa,&system) && pa==base+24576+0x123 && !system,"actual retained walk resolves local leaf and offset");
 cpu_storage[2048]=0x100000|bc250_pte_vm_flags(1,1,1,1);
 check(VidMmTranslateRetained(base+4096,0x234,&pa,&system) && pa==0x100234 && system,"retained walk resolves system leaf without mapping host data");
 cpu_storage[1024]=0;
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system) && !pa,"retained walk refuses missing directory");
 check(!VidMmTranslateRetained(base+sizeof(cpu_storage),0,&pa,&system),"retained walk bounds root against segment");
 check(cpu_map_calls==calls && cpu_map_live==1,"walk performs no dynamic map/unmap");
 VidMmStop();
 check(!g_VidMm.SegmentMapping && !g_VidMm.Ready && !g_VidMm.Write && !cpu_map_live && !shadow_pool_live && !g_VidMm.Shadow.Slots,"stop drops retained mapping and shadow after drain");
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system),"post-stop translation refused");
 cpu_map_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !g_VidMm.Write && !g_VidMm.SegmentMapping && !shadow_pool_live,"mapping resource failure occurs at start before readiness");
 cpu_map_fail=0;calls=cpu_map_calls;
 check(VidMmStart(&d,4096,sizeof(cpu_storage),1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"invalid segment extent rejected before mapping");
 cpu_write_setting=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && !g_VidMm.SegmentMapping && !g_VidMm.Write,"closed write gate retains diagnostic start without mapping");
 VidMmStop();cpu_write_setting=1;
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"mapping lifecycle balances ownership");
}

/* Construction ordering acceptance, also run by the default routing suite. */
static void case_queued_pte_ordering(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_STREAM stream={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte,ptes[512];
 DXGKARG_BUILDPAGINGBUFFER build={0};
 u32 commands[1056],priv[1100],dma[1056];ULONGLONG base=0x200000000ull,pa,entry;BOOLEAN system;
 PAGING_U64 mc=0;ULONG written,next;unsigned i;
 BC250_WDDM_PAGING_UNSUPPORTED unsupported;static u64 scratch[16];
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"ordering probe start");
 memset(cpu_storage,0,sizeof(cpu_storage));
 // Actual immediate CPU initialization registers all four pinned table identities.
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i==4?6:i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"register initialized paging hierarchy");
 }
 stream.Device=&d;stream.Gfx=&gfx;stream.Root=base+4096;use_retained_walk=1;
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"positive control logical resolver sees initialized page6");
 pte.PageAddress=7;u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
 u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=16384;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_SUCCESS && next==1 && written==16,"actual GPU update builder stages remap to page7");
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"unpublished candidate cannot change construction state");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 memset(dma,0xCC,sizeof(dma));
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);
 build.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES+written*4-1;build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short private space refuses publication");
 check(build.pDmaBuffer==dma && build.pDmaBufferPrivateData==priv && all_pattern(dma,1056,0xCCCCCCCCu) && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private refusal leaves DMA pointers, bytes and logical state untouched");
 build.DmaBufferPrivateDataSize=sizeof(priv);build.DmaSize=written*4-1;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short DMA space refuses publication");
 build.DmaSize=sizeof(dma);build.DmaBufferGpuVirtualAddress=~0ull-16;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private address overflow cannot advance logical state");
 build.DmaBufferGpuVirtualAddress=0;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,2)==STATUS_INVALID_PARAMETER && !priv[0] && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"invalid logical batch invalidates candidate without advance");
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"actual publication helper commits valid batch");
 check(build.pDmaBuffer==dma+written && build.DmaSize==sizeof(dma)-written*4 && build.pDmaBufferPrivateData==(unsigned char*)priv+PAGING_PRIVATE_HEADER_BYTES+written*4 && memcmp(dma,commands,written*4)==0,"publication copies exact command bytes and advances both capacities");
 check(PagingResolve(&stream,0,4096,&mc),"logical resolver accepts published update before GPU executes");
 printf("QUEUED_UPDATE: wanted MC=0x%llX, builder resolved MC=0x%llX\n",d.VramMcBase+28672,(unsigned long long)mc);
 check(mc==d.VramMcBase+28672,"REQUIRED: following transfer construction resolves queued page7");
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+24576 && !system,"GPU-visible table remains page6 until GPU executes queued update");
 cpu_storage[2048]=(u64)commands[4]|((u64)commands[5]<<32);
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+28672,"modeled GPU payload application catches up with construction view");
 // Full update must expose only each accepted multipass prefix.
 for(i=0;i<512;i++){ptes[i].Flags=pte.Flags;ptes[i].PageAddress=6+(i&1);}
 u.NumPageTableEntries=512;u.pPageTableEntries=ptes;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480,"stage first480-entry logical batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"publish first multipass prefix");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+16384,479,&entry)==PAGING_PT_OK && PagingPtShadowRead(&g_VidMm.Shadow,base+16384,480,&entry)==PAGING_PT_MISSING,"first prefix cannot expose not-yet-built suffix");
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),480,&written,&next,&unsupported)==STATUS_SUCCESS && next==512,"stage remaining32-entry batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmPublishPagingRecord(&build,written,TRUE,480,next)==STATUS_SUCCESS,"publish second multipass prefix");
 for(i=0;i<512;i++)check(PagingResolve(&stream,(PAGING_U64)i*4096,4096,&mc) && mc==d.VramMcBase+(6+(i&1))*4096,"logical whole-table multipass translation");
 // An unregistered application page must not consume a pinned paging-table slot.
 u.NumPageTableEntries=1;u.pPageTableEntries=&pte;u.PageTableAddress.GpuPhysical.SegmentOffset=5*4096;
 i=g_VidMm.Shadow.Used;
 check(VidMmCommitPagingUpdate(&u,0,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==i,"ordinary application PTE updates do not register logical tables");
 check(!VidMmTranslateRetainedPaging(base+5*4096,0,&pa,&system),"unknown logical root never falls back to live VRAM");
 use_retained_walk=0;VidMmStop();
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"ordering probe cleans lifetime state");
}

static void case_copy_range_builder(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE range={0};BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 u32 buf[64];ULONG written;ULONGLONG src,dst;unsigned cap;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 range.NumPageTableEntries=512;range.SrcPageTableAddress=0x10000;range.DstPageTableAddress=0x20000;
 translated=0x200004000ull;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildCopyRange(&d,0x200001000ull,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && written==34 && src==translated && dst==translated,"actual copy-range builder uses retained staging and reports resolved physical identity");
 check(buf[3]==0x4000 && buf[4]==1 && buf[5]==0x80000 && buf[6]==1 && buf[22]==0x4000,"copy-range builder converts resolved PA to MC, not raw GPU VA");
 check(all_pattern(buf+34,30,0xCCCCCCCCu),"range builder preserves output tail");
 range.NumPageTableEntries=2;range.SrcStartPteIndex=3;range.DstStartPteIndex=7;translation_by_offset=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,128,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && src==translated+24 && dst==translated+56 && buf[1]==15 && buf[3]==0x4018 && buf[22]==0x4038 && buf[10]==97 && buf[27]==98,"copy-range indices and command-offset marker sequence are independent");
 translation_by_offset=0;range.NumPageTableEntries=512;range.SrcStartPteIndex=range.DstStartPteIndex=0;
 for(cap=0;cap<192;cap+=4) {
  memset(buf,0xCC,sizeof(buf));
  check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,cap,&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !src && !dst && all_pattern(buf,64,0xCCCCCCCCu),"range capacity refusal publishes no commands or identities");
 }
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,3984,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written,"accumulated ring bytes plus outer fence bound copy range");
 range.SrcStartPteIndex=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"source index plus count cannot exceed advertised table");
 range.SrcStartPteIndex=0;range.DstPageTableAddress++;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"copy table GPU VA must have documented64KiB alignment");
 range.DstPageTableAddress--;translationOk=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingNoTranslation,"missing logical translation refuses range");
 translationOk=1;isSystem=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingSystemMemory,"current local-only page-table segment requires local copy addresses");
 isSystem=0;gfx.PagingCopyStaging.size=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_DEVICE_NOT_READY && unsupported==BC250PagingNotReady,"missing staging resource refuses before output");
 check(!flush_lock_depth && !flush_region_depth,"range builder balances lifetime lock on all exits");
}

static void case_copy_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[40];DXGKARG_BUILDPAGINGBUFFER b={0};
 u32 dma[1100],priv[1500];u64 values[41],entry;unsigned i,prior,passes=0;
 const u64 physical=0x200004000ull;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=physical;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;translation_by_offset=1;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);
 for(i=0;i<41;i++)values[i]=100+i;
 check(PagingPtShadowApply(&g_VidMm.Shadow,physical,0,41,values,1)==PAGING_PT_OK,"copy publication initialized source/destination control");
 memset(ranges,0,sizeof(ranges));
 for(i=0;i<40;i++) {
  ranges[i].NumPageTableEntries=1;ranges[i].SrcPageTableAddress=0x10000;ranges[i].DstPageTableAddress=0x20000;
  ranges[i].SrcStartPteIndex=i;ranges[i].DstStartPteIndex=i+1;
 }
 b.CopyPageTableEntries.NumRanges=40;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.DmaBufferGpuVirtualAddress=~0ull-16;
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0],"copy private header refusal preserves progress and invalidates record");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==101 && all_pattern(dma,1100,0xCCCCCCCCu),"refused copy leaves logical state and DMA unchanged");
 b.DmaBufferGpuVirtualAddress=0;b.DmaSize=192;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && b.MultipassOffset==1 && b.DmaSize==56 && b.DmaBufferWriteOffset==0,"short buffer accepts exactly one whole range and preserves input offset");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==100 && PagingPtShadowRead(&g_VidMm.Shadow,physical,2,&entry)==PAGING_PT_OK && entry==102,"accepted prefix visible and unaccepted suffix unchanged");
 while(b.MultipassOffset<40 && passes++<5) {
  NTSTATUS status;
  prior=b.MultipassOffset;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  b.DmaBufferWriteOffset=64;
  status=WddmBuildPagingCopies(&d,1,&b);
  check(b.MultipassOffset>prior && b.DmaBufferWriteOffset==64,"copy multipass makes progress and restores caller offset");
  check(status==(b.MultipassOffset==40?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER),"copy list reports insufficient until all ranges accepted");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,(unsigned)(sizeof(dma)-b.DmaSize),0,NULL,NULL),"accepted copy records cover exact contiguous command span");
  for(i=1;i<=40;i++)check(PagingPtShadowRead(&g_VidMm.Shadow,physical,i,&entry)==PAGING_PT_OK && entry==(i<=b.MultipassOffset?100:100+i),"dependent range sees prior accepted logical copy across buffer boundary");
 }
 check(b.MultipassOffset==40 && passes==2,"live ring limit forces list into two further buffers");
 check(VidMmCommitPagingCopy(physical+1,physical+8,1)==STATUS_INVALID_PARAMETER,"unaligned physical copy identity refused");
 check(VidMmCommitPagingCopy(physical+4088,physical+8,2)==STATUS_INVALID_PARAMETER,"logical copy cannot cross table boundary");
 check(VidMmCommitPagingCopy(physical+4096,physical+8,1)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_MISSING,"unknown source clears registered destination knowledge");
 check(VidMmCommitPagingCopy(physical,physical+8192,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==1,"ordinary application destination never registered by copy");
 g_VidMm.Ready=0;
 check(VidMmCommitPagingCopy(physical,physical+8,1)==STATUS_DEVICE_NOT_READY,"post-stop copy commit refuses");
 b.MultipassOffset=0;b.CopyPageTableEntries.NumRanges=1;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_DEVICE_NOT_READY && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0] && b.DmaBufferWriteOffset==64 && all_pattern(dma,1100,0xCCCCCCCCu),"failed logical commit cannot publish DMA or advance copy progress");
 b.CopyPageTableEntries.NumRanges=0;
 check(WddmBuildPagingCopies(&d,0,&b)==STATUS_SUCCESS && b.MultipassOffset==0,"empty range list is a completed no-op");
 b.CopyPageTableEntries.NumRanges=1;b.CopyPageTableEntries.pRanges=NULL;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"nonempty range list requires array");
 b.CopyPageTableEntries.NumRanges=0;b.MultipassOffset=1;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"out-of-list progress refused");
 check(!cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"copy publication exits balance locks");
 translation_by_offset=0;
}

static void case_copy_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[2]={{0}};DXGKARG_BUILDPAGINGBUFFER b={0};
 static u64 scratch[16];u64 staging[512],pa,expected;BOOLEAN system;
 u32 dma[256],priv[300];unsigned i;const u64 base=0x200000000ull;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"real-copy walker startup");
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=7;i++) {
  u.PageTableLevel=i<4?4-i:0;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.PageAddress=i<4?i+1:i==5?7:i==6?5:4;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy CPU initialization registers table identities");
 }
 u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 for(i=1;i<=3;i++) {
  u.StartIndex=i*16;pte.PageAddress=i+3;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy maps64KiB-aligned VAs to distinct tables");
 }
 expected=cpu_storage[7*512];use_retained_walk=1;
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && !system,"positive control source VA initially names physical page6");
 ranges[0].NumPageTableEntries=1;ranges[0].SrcPageTableAddress=0x20000;
 ranges[0].DstPageTableAddress=0x10000;ranges[0].DstStartPteIndex=48;
 ranges[1].NumPageTableEntries=1;ranges[1].SrcPageTableAddress=0x30000;
 ranges[1].DstPageTableAddress=0x20000;ranges[1].DstStartPteIndex=1;
 b.CopyPageTableEntries.NumRanges=2;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPagingCopies(&d,base+4096,&b)==STATUS_SUCCESS && b.MultipassOffset==2,"actual list builder accepts two address-dependent copies");
 check(dma[3]==5*4096 && dma[22]==4*4096+48*8,"first command copies remap PTE into actual leaf table");
 check(dma[34+3]==7*4096 && dma[34+22]==5*4096+8,"second command resolves new source physical page7 through actual logical walker");
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+7*4096,"logical hierarchy reflects accepted remap before execution");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+5*4096,1,&pa)==PAGING_PT_OK && pa==expected,"second accepted copy metadata comes from newly mapped source page");
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && cpu_storage[5*512+1]==0,"live GPU tables unchanged by command construction");
 // Independent data execution model: decode the two COPY_LINEAR addresses per
 // transaction, using staging as a separate host allocation. Not a GPU emulator.
 for(i=0;i<2;i++) {
  const u32*packet=dma+i*34;u64 source=((u64)packet[4]<<32)|packet[3];
  u64 destination=((u64)packet[23]<<32)|packet[22];unsigned bytes=packet[1]+1;
  check(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes,"interpreted copy addresses within host VRAM model");
  if(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes) {
   memcpy(staging,(unsigned char*)cpu_storage+(size_t)(source-d.VramMcBase),bytes);
   memcpy((unsigned char*)cpu_storage+(size_t)(destination-d.VramMcBase),staging,bytes);
  }
 }
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+7*4096 && cpu_storage[5*512+1]==expected,"modeled execution makes live tables agree with accepted construction state");
 VidMmStop();use_retained_walk=0;
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"real-copy integration balances mapping and locks");
}
