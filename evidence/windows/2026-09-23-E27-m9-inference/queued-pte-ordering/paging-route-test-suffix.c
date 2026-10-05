
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
 g_VidMm.SegmentPhysical=0x200000000ull;g_VidMm.SegmentLength=0x1000000;
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
 unsigned i;long long priorWritten;int maps;
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
 check(!g_VidMm.SegmentMapping && !g_VidMm.Ready && !g_VidMm.Write && !cpu_map_live,"stop drops retained mapping after drain");
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system),"post-stop translation refused");
 cpu_map_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !g_VidMm.Write && !g_VidMm.SegmentMapping,"mapping resource failure occurs at start before readiness");
 cpu_map_fail=0;calls=cpu_map_calls;
 check(VidMmStart(&d,4096,sizeof(cpu_storage),1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"invalid segment extent rejected before mapping");
 cpu_write_setting=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && !g_VidMm.SegmentMapping && !g_VidMm.Write,"closed write gate retains diagnostic start without mapping");
 VidMmStop();cpu_write_setting=1;
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"mapping lifecycle balances ownership");
}

/* Explicit failing acceptance probe, selected with --queued-pte-ordering.
 * It must not be counted as a passing regression until the architecture changes.
 */
static void case_queued_pte_ordering(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_STREAM stream={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte;
 u32 commands[32];ULONGLONG base=0x200000000ull;PAGING_U64 mc=0;ULONG written,next;
 BC250_WDDM_PAGING_UNSUPPORTED unsupported;static u64 scratch[16];
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"ordering probe start");
 memset(cpu_storage,0,sizeof(cpu_storage));
 cpu_storage[512]=(base+8192)|7;cpu_storage[1024]=(base+12288)|7;cpu_storage[1536]=(base+16384)|7;
 cpu_storage[2048]=(base+24576)|bc250_pte_vm_flags(1,1,0,0);
 stream.Device=&d;stream.Gfx=&gfx;stream.Root=base+4096;use_retained_walk=1;
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"positive control actual resolver and walk see old page6");
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=7;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=16384;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_SUCCESS && next==1 && written==16,"actual GPU update builder accepts remap to page7");
 check(PagingResolve(&stream,0,4096,&mc),"resolve following queued update succeeds");
 printf("QUEUED_UPDATE: wanted MC=0x%llX, builder resolved MC=0x%llX\n",d.VramMcBase+28672,(unsigned long long)mc);
 check(mc==d.VramMcBase+28672,"REQUIRED: following transfer construction must resolve queued page7, not old page6");
 // Positive control only: apply the encoded WRITE_LINEAR payload to modeled RAM.
 // This is not GPU execution or a proposed CPU-write fix.
 cpu_storage[2048]=(u64)commands[4]|((u64)commands[5]<<32);
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+28672,"after modeled packet execution actual resolver sees new page7");
 // Paging-process initialization is explicitly immediate CPU_VIRTUAL in MS DDI.
 cpu_storage[2048]=(base+24576)|bc250_pte_vm_flags(1,1,0,0);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_storage+2048;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+28672,"CPU_VIRTUAL immediate update control sees new page without GPU execution");
 use_retained_walk=0;VidMmStop();
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"ordering probe cleans lifetime state");
}
