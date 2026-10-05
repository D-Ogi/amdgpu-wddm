
static void case_kmd_routes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 BC250_DEVICE dev={{0x200000000ll},0x100000000ull,0x100000};
 BC250_GFX gfx={1,{0,0},&g_adev};BC250_PAGING_STREAM st;
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
