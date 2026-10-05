#include <stdio.h>
#include <string.h>
#include "bc250_sdma.h"
#include "bc250_gfx.h"
#include "bc250_gmc.h"
#include "backend_mem.h"
#include "backend_trace.h"
#include "generated/rlc_cg_flags.h"
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "nbio_2_3_offset.h"
#include "soc15_common.h"
extern int cyan_skillfish_reg_base_init(struct amdgpu_device *adev);
static unsigned checks,bad,mode,selected,pulses,delays,unfreezes;
static struct amdgpu_device *dev;
struct reg {u32 off,value;};
static struct reg regs[96];
static unsigned nr;
static u32 reset_off,safe_off,control_off,selected_mask;
#define CHECK(x) do {checks++;if(!(x)){bad++;printf("FAIL line %u mode%u inst%u: %s\n",(unsigned)__LINE__,mode,selected,#x);}}while(0)
static u32 off(u32 inst,u32 name){return bc250_sdma_reg_offset(dev,inst,name)*4;}
static struct reg *find(u32 addr){unsigned i;for(i=0;i<nr;i++)if(regs[i].off==addr)return &regs[i];CHECK(0);return &regs[0];}
static void add(u32 addr,u32 value){regs[nr].off=addr;regs[nr++].value=value;}
static int read_reg(u32 addr,u32 *value)
{
 *value=find(addr)->value;
 if(addr==safe_off)*value=0; /* declared RLC ACK model */
 if(addr==off(selected,mmSDMA0_FREEZE) && (*value&SDMA0_FREEZE__FREEZE_MASK) && mode!=1 && !(mode==5 && pulses))
  *value|=SDMA0_FREEZE__FROZEN_MASK;
 if(mode==3 && pulses && addr==off(selected,mmSDMA0_GFX_RB_WPTR))*value=256; /* refuses a new empty pointer */
 if(mode==2 && pulses && addr==off(selected,mmSDMA0_F32_CNTL))*value&=~SDMA0_F32_CNTL__HALT_MASK;
 return 1;
}
static void empty_cpu(void)
{
 struct amdgpu_ring *ring=&dev->sdma.instance[selected].ring;
 unsigned j;u64 *wb=(u64*)dev->sdma.wb_mem.cpu;
 CHECK(!ring->wptr && !ring->wptr_old && !ring->count_dw);
 CHECK(wb[selected*2]==0 && wb[selected*2+1]==0);
 for(j=0;j<=ring->buf_mask;j++)CHECK(ring->ring[j]==ring->funcs->nop);
}
static void write_reg(u32 addr,u32 value)
{
 struct reg *r=find(addr);
 if(addr==reset_off){
  CHECK((value&~selected_mask)==(r->value&~selected_mask));
  if(value&selected_mask){
   CHECK(find(off(selected,mmSDMA0_F32_CNTL))->value&SDMA0_F32_CNTL__HALT_MASK);
   CHECK(!(find(off(selected,mmSDMA0_GFX_RB_CNTL))->value&SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK));
   CHECK(!(find(off(selected,mmSDMA0_GFX_IB_CNTL))->value&SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK));
  }else if(r->value&selected_mask){
   pulses++;
   /* Reset may disturb old halt/queue defaults: second quiescence is required. */
   find(off(selected,mmSDMA0_F32_CNTL))->value=0;
   find(off(selected,mmSDMA0_GFX_RB_CNTL))->value|=SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
   find(off(selected,mmSDMA0_GFX_IB_CNTL))->value|=SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
   find(off(selected,mmSDMA0_CNTL))->value|=SDMA0_CNTL__UTC_L1_ENABLE_MASK;
  }
 }
 if(addr==off(selected,mmSDMA0_FREEZE) && !(value&SDMA0_FREEZE__FREEZE_MASK)){
  CHECK(pulses==1);empty_cpu();unfreezes++;
 }
 r->value=value;
}
static void delay(unsigned us){if(us==50){CHECK(find(reset_off)->value&selected_mask);delays++;}}
static const u32 names[]={
 mmSDMA0_GFX_RB_CNTL,mmSDMA0_GFX_IB_CNTL,mmSDMA0_FREEZE,mmSDMA0_F32_CNTL,
 mmSDMA0_CNTL,mmSDMA0_STATUS1_REG,mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL,
 mmSDMA0_GFX_RB_RPTR,mmSDMA0_GFX_RB_RPTR_HI,mmSDMA0_GFX_RB_WPTR,mmSDMA0_GFX_RB_WPTR_HI,
 mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO,mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI,mmSDMA0_GFX_RB_WPTR_POLL_CNTL,
 mmSDMA0_GFX_RB_RPTR_ADDR_HI,mmSDMA0_GFX_RB_RPTR_ADDR_LO,mmSDMA0_GFX_RB_BASE,mmSDMA0_GFX_RB_BASE_HI,
 mmSDMA0_GFX_MINOR_PTR_UPDATE,mmSDMA0_GFX_DOORBELL,mmSDMA0_GFX_DOORBELL_OFFSET,
 mmSDMA0_UTCL1_CNTL,mmSDMA0_UTCL1_PAGE
};
static void check_order(struct amdgpu_device *adev)
{
 const struct bc250_reg_write *writes=backend_writes();unsigned n=0,i;
 u32 expected[48];
 #define E(x) expected[n++]=off(selected,(x))
 #define STOP() E(mmSDMA0_GFX_RB_CNTL);E(mmSDMA0_GFX_IB_CNTL);E(mmSDMA0_FREEZE);E(mmSDMA0_F32_CNTL);E(mmSDMA0_CNTL)
 STOP();expected[n++]=reset_off;expected[n++]=reset_off;STOP();
 E(mmSDMA0_FREEZE);E(mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL);E(mmSDMA0_GFX_RB_CNTL);
 E(mmSDMA0_GFX_RB_RPTR);E(mmSDMA0_GFX_RB_RPTR_HI);E(mmSDMA0_GFX_RB_WPTR);E(mmSDMA0_GFX_RB_WPTR_HI);
 E(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO);E(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI);E(mmSDMA0_GFX_RB_WPTR_POLL_CNTL);
 E(mmSDMA0_GFX_RB_RPTR_ADDR_HI);E(mmSDMA0_GFX_RB_RPTR_ADDR_LO);E(mmSDMA0_GFX_RB_BASE);E(mmSDMA0_GFX_RB_BASE_HI);
 E(mmSDMA0_GFX_MINOR_PTR_UPDATE);E(mmSDMA0_GFX_RB_WPTR);E(mmSDMA0_GFX_RB_WPTR_HI);
 E(mmSDMA0_GFX_DOORBELL);E(mmSDMA0_GFX_DOORBELL_OFFSET);
 expected[n++]=(selected?SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA1_DOORBELL_RANGE):SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA0_DOORBELL_RANGE))*4;
 E(mmSDMA0_GFX_MINOR_PTR_UPDATE);E(mmSDMA0_CNTL);E(mmSDMA0_UTCL1_CNTL);E(mmSDMA0_UTCL1_PAGE);
 E(mmSDMA0_F32_CNTL);E(mmSDMA0_GFX_RB_CNTL);E(mmSDMA0_GFX_IB_CNTL);
 CHECK(backend_write_count()==n);for(i=0;i<n && i<backend_write_count();i++)CHECK(writes[i].byte_offset==expected[i]);
 #undef STOP
 #undef E
}
static void run_case(void)
{
 struct amdgpu_device a;struct amdgpu_device *adev=&a;
 struct bc250_sdma_reset_receipt receipt;
 struct amdgpu_ring sibling;u64 wb_before[4];u32 sibling_regs[24],sibling_nbio;
 unsigned i,j,other=selected^1u;int rc;
 memset(adev,0,sizeof(*adev));dev=adev;nr=pulses=delays=unfreezes=0;
 backend_mem_reset();backend_reset_state();backend_reset_writes();backend_clear_aliases();
 CHECK(cyan_skillfish_reg_base_init(adev)==0);
 backend_mem_set_bases(0x20000000ull,0x10000000ull); /* synthetic addresses, no hardware */
 CHECK(bc250_sdma_setup(adev)==0);adev->usec_timeout=3;
 adev->cg_flags=mode==4?AMD_CG_SUPPORT_GFX_MGCG:0;
 for(i=0;i<2;i++){
  struct amdgpu_ring *ring=&adev->sdma.instance[i].ring;
  ring->wptr=64u+i*16u;ring->wptr_old=32;ring->count_dw=16;
  for(j=0;j<=ring->buf_mask;j++)ring->ring[j]=0xdead0000u+i;
  ((u64*)adev->sdma.wb_mem.cpu)[i*2]=128+i*64;
  ((u64*)adev->sdma.wb_mem.cpu)[i*2+1]=ring->wptr*4;
  for(j=0;j<sizeof(names)/sizeof(names[0]);j++)add(off(i,names[j]),0);
  find(off(i,mmSDMA0_GFX_RB_CNTL))->value=SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
  find(off(i,mmSDMA0_GFX_IB_CNTL))->value=SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
  find(off(i,mmSDMA0_CNTL))->value=SDMA0_CNTL__UTC_L1_ENABLE_MASK;
  find(off(i,mmSDMA0_GFX_RB_RPTR))->value=128+i*64;
  find(off(i,mmSDMA0_GFX_RB_WPTR))->value=(u32)(ring->wptr*4);
 }
 reset_off=SOC15_REG_OFFSET(GC,0,mmGRBM_SOFT_RESET)*4;add(reset_off,GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK);
 selected_mask=selected?GRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK:GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK;
 safe_off=SOC15_REG_OFFSET(GC,0,mmRLC_SAFE_MODE)*4;add(safe_off,0);
 control_off=SOC15_REG_OFFSET(GC,0,mmRLC_CNTL)*4;add(control_off,RLC_CNTL__RLC_ENABLE_F32_MASK);
 add(SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA0_DOORBELL_RANGE)*4,0x80000000u);
 add(SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA1_DOORBELL_RANGE)*4,0x80000000u);
 sibling=adev->sdma.instance[other].ring;memcpy(wb_before,adev->sdma.wb_mem.cpu,sizeof(wb_before));
 for(j=0;j<sizeof(names)/sizeof(names[0]);j++)sibling_regs[j]=find(off(other,names[j]))->value;
 sibling_nbio=find((other?SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA1_DOORBELL_RANGE):SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA0_DOORBELL_RANGE))*4)->value;
 backend_set_read_hook(read_reg);backend_set_write_hook(write_reg);backend_set_delay_hook(delay);
 rc=bc250_sdma_reset_retained_instance(adev,selected,&receipt);
 CHECK(rc==((mode==1 || mode==5)?BC250_ETIME:(mode==2 || mode==3)?BC250_EIO:0));
 CHECK(receipt.previous_wptr==64u+selected*16u);
 CHECK(memcmp(&sibling,&adev->sdma.instance[other].ring,sizeof(sibling))==0);
 for(j=0;j<=sibling.buf_mask;j++)CHECK(sibling.ring[j]==0xdead0000u+other);
 CHECK(((u64*)adev->sdma.wb_mem.cpu)[other*2]==wb_before[other*2]);
 CHECK(((u64*)adev->sdma.wb_mem.cpu)[other*2+1]==wb_before[other*2+1]);
 for(j=0;j<sizeof(names)/sizeof(names[0]);j++)CHECK(find(off(other,names[j]))->value==sibling_regs[j]);
 CHECK(find((other?SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA1_DOORBELL_RANGE):SOC15_REG_OFFSET(NBIO,0,mmBIF_SDMA0_DOORBELL_RANGE))*4)->value==sibling_nbio);
 CHECK(backend_doorbell_count()==0);
 CHECK(pulses==(mode==1?0u:1u));CHECK(delays==pulses);
 if(mode==0 || mode==4){CHECK(receipt.stage==BC250_SDMA_RESET_PROGRAMMED);CHECK(receipt.rptr==0 && receipt.wptr==0);empty_cpu();CHECK(unfreezes==1);if(!mode)check_order(adev);}
 else if(mode==3){CHECK(receipt.stage==BC250_SDMA_RESET_VERIFY);CHECK(adev->sdma.instance[selected].ring.wptr==0);}
 else {CHECK(unfreezes==0);CHECK(adev->sdma.instance[selected].ring.wptr==receipt.previous_wptr);for(j=0;j<=adev->sdma.instance[selected].ring.buf_mask;j++)CHECK(adev->sdma.instance[selected].ring.ring[j]==0xdead0000u+selected);}
 backend_reset_writes();CHECK(bc250_sdma_reset_retained_instance(adev,2,&receipt)==BC250_EINVAL);CHECK(backend_write_count()==0);
 CHECK(bc250_sdma_reset_retained_instance(NULL,0,&receipt)==BC250_EINVAL);
 CHECK(bc250_sdma_reset_retained_instance(adev,0,NULL)==BC250_EINVAL);
 backend_set_read_hook(NULL);backend_set_write_hook(NULL);backend_set_delay_hook(NULL);bc250_sdma_teardown(adev);backend_mem_reset();
}
int main(void){for(selected=0;selected<2;selected++)for(mode=0;mode<6;mode++)run_case();printf("SDMA single retained recovery: %u checks, %u failures; host model only\n",checks,bad);return bad?1:0;}
