// AMD DP MSA vectors and failure injection. MMIO and time are local models.
#include <stdio.h>
#include <string.h>
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../../shim/bc250_dp_stream.h"
static unsigned checks,failures;
#define CHECK(x) do { ++checks;if(!(x)) {++failures;printf("FAIL %u: %s\n",(unsigned)__LINE__,#x);} } while(0)
#define REG(n) BC250_REG_DMU_##n
typedef struct MODEL { unsigned long reg[65536];unsigned access,failAt,after,writes,stuck; } MODEL;
static MODEL model;
static long access(MODEL* m)
{ ++m->access;if(m->failAt && m->access>m->failAt)++m->after;return m->access==m->failAt?BC250_DISP_STATUS_DEVICE:0; }
static long Read(void* p,unsigned long r,unsigned long* v)
{ MODEL* m=p;long s=access(m);CHECK(r/4<65536);if(s>=0)*v=m->reg[r/4];return s; }
static long Write(void* p,unsigned long r,unsigned long v)
{
    MODEL* m=p;long s=access(m);CHECK(r/4<65536);
    if(s>=0) {
        if(r==REG(DP0_DP_STEER_FIFO) && (v&DP0_DP_STEER_FIFO__DP_STEER_FIFO_RESET_MASK))
            CHECK(!(m->reg[REG(DP0_DP_VID_STREAM_CNTL)/4]&DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK));
        if(r==REG(DP0_DP_VID_STREAM_CNTL) && !m->stuck) {
            v&=~DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK;
            if(v&DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK)v|=DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK;
        }
        m->reg[r/4]=v;++m->writes;
    }
    return s;
}
static void Stall(void* p,unsigned long us) {(void)p;(void)us;}
static unsigned long field(unsigned long reg,unsigned long mask)
{ return Bc250DispGet(model.reg[reg/4],mask); }
#define F(r,f) field(REG(DP0_##r),DP0_##r##__##f##_MASK)
static const BC250_TARGET_TIMING hd={148500,1920,280,88,44,1080,45,4,5,0x1e};
static const BC250_TARGET_TIMING svga={40000,800,256,40,128,600,28,1,4,0x1e};
static long op(BC250_DISP_IO* io,unsigned n)
{
    if(n==0)return Bc250DpStreamProgram(io,&hd);
    if(n==1)return Bc250DpStreamBlank(io);
    return Bc250DpStreamUnblank(io,148500,10);
}
static void setup(unsigned op)
{
    memset(&model,0,sizeof(model));
    if(op==1)model.reg[REG(DP0_DP_VID_STREAM_CNTL)/4]=
        DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK|DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK;
}
int main(void)
{
    BC250_DISP_IO io={0};unsigned n,i,total;BC250_TARGET_TIMING bad;
    io.Context=&model;io.Read=Read;io.Write=Write;io.Stall=Stall;io.BudgetUs=110000;
    CHECK(Bc250DpStreamProgram(&io,&hd)==0);
    CHECK(F(DP_MSA_TIMING_PARAM1,DP_MSA_HTOTAL)==2200 && F(DP_MSA_TIMING_PARAM1,DP_MSA_VTOTAL)==1125);
    CHECK(F(DP_MSA_TIMING_PARAM2,DP_MSA_HSTART)==192 && F(DP_MSA_TIMING_PARAM2,DP_MSA_VSTART)==41);
    CHECK(F(DP_MSA_TIMING_PARAM3,DP_MSA_HSYNCWIDTH)==44 && F(DP_MSA_TIMING_PARAM3,DP_MSA_VSYNCWIDTH)==5);
    CHECK(F(DP_MSA_TIMING_PARAM3,DP_MSA_HSYNCPOLARITY)==0 && F(DP_MSA_TIMING_PARAM3,DP_MSA_VSYNCPOLARITY)==0);
    CHECK(F(DP_MSA_TIMING_PARAM4,DP_MSA_HWIDTH)==1920 && F(DP_MSA_TIMING_PARAM4,DP_MSA_VHEIGHT)==1080);
    CHECK(F(DP_MSA_COLORIMETRY,DP_MSA_MISC0)==0x20);
    CHECK(F(DP_PIXEL_FORMAT,DP_COMPONENT_DEPTH)==1 && F(DP_PIXEL_FORMAT,DP_PIXEL_ENCODING)==0);
    CHECK(Bc250DpStreamUnblank(&io,148500,10)==0);
    CHECK(F(DP_VID_M,DP_VID_M)==18022 && F(DP_VID_N,DP_VID_N)==32768);
    CHECK(F(DP_VID_TIMING,DP_VID_M_N_GEN_EN)==1 && F(DP_VID_STREAM_CNTL,DP_VID_STREAM_ENABLE)==1);
    CHECK(io.StalledUs==100);
    total=model.writes;CHECK(Bc250DpStreamProgram(&io,&svga)==BC250_DISP_STATUS_BUSY && model.writes==total);
    CHECK(Bc250DpStreamBlank(&io)==0);
    CHECK(F(DP_VID_STREAM_CNTL,DP_VID_STREAM_ENABLE)==0 && F(DP_STEER_FIFO,DP_STEER_FIFO_RESET)==1);
    CHECK(Bc250DpStreamProgram(&io,&svga)==0);
    CHECK(F(DP_MSA_TIMING_PARAM1,DP_MSA_HTOTAL)==1056 && F(DP_MSA_TIMING_PARAM1,DP_MSA_VTOTAL)==628);
    CHECK(Bc250DpStreamUnblank(&io,40000,6)==0 && F(DP_VID_M,DP_VID_M)==8090);
    // BD-139: HBR2 and HBR3. VID_M is 0x8000 x pixel / (rate x 27 MHz), rounded down (enc1_stream_encoder_dp_unblank).
    CHECK(Bc250DpStreamUnblank(&io,645100,20)==0 && F(DP_VID_M,DP_VID_M)==39145 && F(DP_VID_N,DP_VID_N)==32768);
    CHECK(Bc250DpStreamUnblank(&io,497750,20)==0 && F(DP_VID_M,DP_VID_M)==30204);
    CHECK(Bc250DpStreamUnblank(&io,1080000,30)==0 && F(DP_VID_M,DP_VID_M)==43690);
    for(n=0;n<3;++n) {
        setup(n);io.StalledUs=0;CHECK(op(&io,n)==0);total=model.access;
        for(i=1;i<=total;++i) {
            setup(n);io.StalledUs=0;model.failAt=i;
            CHECK(op(&io,n)==BC250_DISP_STATUS_DEVICE);
            CHECK(model.access==i && !model.after);
        }
    }
    setup(1);model.stuck=1;io.BudgetUs=30;io.StalledUs=0;
    CHECK(Bc250DpStreamBlank(&io)==BC250_DISP_STATUS_BUDGET && io.StalledUs==30);
    CHECK(!F(DP_STEER_FIFO,DP_STEER_FIFO_RESET));
    setup(0);io.BudgetUs=99;io.StalledUs=0;
    CHECK(Bc250DpStreamUnblank(&io,148500,10)==BC250_DISP_STATUS_BUDGET);
    CHECK(!F(DP_VID_STREAM_CNTL,DP_VID_STREAM_ENABLE));
    setup(0);bad=hd;bad.Flags|=0x80;CHECK(Bc250DpStreamProgram(&io,&bad)==BC250_DISP_STATUS_INVALID && !model.access);
    CHECK(Bc250DpStreamUnblank(&io,148500,21)==BC250_DISP_STATUS_INVALID && !model.access);
    CHECK(Bc250DpStreamUnblank(&io,1080001,30)==BC250_DISP_STATUS_INVALID && !model.access);
    CHECK(Bc250DpStreamUnblank(&io,24999,6)==BC250_DISP_STATUS_INVALID && !model.access);
    printf("dp_stream_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
