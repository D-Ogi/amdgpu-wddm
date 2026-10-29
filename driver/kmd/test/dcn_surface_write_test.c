#include "../dcn_surface_write.h"
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static struct {
    unsigned reads, writes, count, failRead, failWrite, after, stalls;
    unsigned long addr[32], value[32], writtenAddr[13], writtenValue[13];
} hw;
static unsigned Find(unsigned long addr)
{
    unsigned i;
    for (i=0;i<hw.count;++i) if (hw.addr[i]==addr) return i;
    CHECK(hw.count < 32);
    if (hw.count >= 32) return 0;
    hw.addr[hw.count]=addr; return hw.count++;
}
static void Set(unsigned long addr,unsigned long value) { hw.value[Find(addr)]=value; }
static unsigned long Get(unsigned long addr) { return hw.value[Find(addr)]; }
static long Read(void* c,unsigned long addr,unsigned long* value)
{
    (void)c;
    CHECK(hw.writes==0); // No read may fail after the first attempted write.
    if (++hw.reads==hw.failRead) return BC250_DISP_STATUS_DEVICE;
    *value=Get(addr); return 0;
}
static long Write(void* c,unsigned long addr,unsigned long value)
{
    unsigned n=hw.writes++;
    (void)c; CHECK(n<13);
    CHECK(addr!=BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK);
    CHECK(addr!=BC250_REG_DMU_HUBP0_DCHUBP_CNTL && addr!=BC250_REG_DMU_HUBP1_DCHUBP_CNTL);
    if (n>=13) return BC250_DISP_STATUS_INVALID;
    if (hw.failWrite==n+1 && !hw.after) return BC250_DISP_STATUS_DEVICE;
    Set(addr,value); hw.writtenAddr[n]=addr; hw.writtenValue[n]=value;
    return hw.failWrite==n+1 ? BC250_DISP_STATUS_DEVICE : 0;
}
static void Stall(void* c,unsigned long us) { (void)c;(void)us;++hw.stalls; }
static BC250_DISP_IO Setup(unsigned pipe,unsigned long seed)
{
    BC250_DISP_IO io={0};
    const unsigned long regs[2][10]={
#define REGS(p) { BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_CONTROL, BC250_REG_DMU_HUBP##p##_DCSURF_ADDR_CONFIG, \
 BC250_REG_DMU_HUBP##p##_DCSURF_TILING_CONFIG, BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_PITCH, \
 BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_PITCH_C, BC250_REG_DMU_HUBP##p##_DCSURF_SURFACE_CONFIG, \
 BC250_REG_DMU_HUBPRET##p##_HUBPRET_CONTROL, BC250_REG_DMU_HUBPREQ##p##_DCSURF_FLIP_CONTROL, \
 BC250_REG_DMU_HUBPREQ##p##_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, BC250_REG_DMU_HUBPREQ##p##_DCSURF_PRIMARY_SURFACE_ADDRESS }
        REGS(0), REGS(1)
    };
    unsigned i;
    memset(&hw,0,sizeof(hw));
    for(i=0;i<10;++i) { seed=1664525ul*seed+1013904223ul; Set(regs[pipe][i],seed); }
    Set(BC250_REG_DMU_OTG0_OTG_CONTROL,OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK);
    Set(BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,0);
    Set(BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
        OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK);
    Set(pipe ? BC250_REG_DMU_HUBP1_DCHUBP_CNTL : BC250_REG_DMU_HUBP0_DCHUBP_CNTL,0);
    Set(regs[pipe][7],(Get(regs[pipe][7]) & ~(HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK |
        HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK)) |
        HUBPREQ0_DCSURF_FLIP_CONTROL__HUBPREQ_MASTER_UPDATE_LOCK_STATUS_MASK);
    io.Read=Read;io.Write=Write;io.Stall=Stall;return io;
}
static BC250_SURFACE_WRITE_REQUEST Request(unsigned format)
{
    BC250_SURFACE_WRITE_REQUEST r={0};
    r.Surface.Format=format;r.Surface.Width=1920;r.Surface.Height=1080;r.Surface.PitchBytes=7680;
    r.Surface.McBase=0x800000000ull;r.Surface.VramPhysical=0x100000000ull;r.Surface.VramLength=0x10000000ull;
    r.Surface.AllocationMcAddress=r.Surface.McBase+4096;r.Surface.AllocationBytes=16ull*1024*1024;
    r.NumPipes=4;r.NumBanks=4;r.NumShaderEngines=1;r.NumRbPerSe=2;r.MaxCompressedFrags=1;
    return r;
}
static void Successful(void)
{
    unsigned pipe,fmt,seed,n;
    for(pipe=0;pipe<2;++pipe) for(fmt=0;fmt<BC250_LINEAR_FORMAT_COUNT;++fmt) for(seed=0;seed<128;++seed) {
        BC250_SURFACE_WRITE_REQUEST r=Request(fmt);
        BC250_SURFACE_WRITE_PLAN plan;
        BC250_DISP_IO io=Setup(pipe,seed);
        unsigned long ctl=Get(pipe ? BC250_REG_DMU_HUBPREQ1_DCSURF_SURFACE_CONTROL : BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL);
        unsigned long dcc=HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK |
            HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_IND_64B_BLK_MASK |
            HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_EN_MASK |
            HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_IND_64B_BLK_MASK;
        unsigned long tmz=HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
            HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK;
        CHECK(Bc250SurfaceWritePrepare(&r,&plan));
        CHECK(Bc250SurfaceWriteApplyLocked(&io,pipe,&plan,&n)==0);
        CHECK(n==13 && hw.writes==13 && hw.reads==13 && !hw.stalls && !io.Refusals);
        CHECK(hw.writtenAddr[11]==(pipe ? BC250_REG_DMU_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH : BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH));
        CHECK(hw.writtenAddr[12]==(pipe ? BC250_REG_DMU_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS : BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS));
        CHECK((((unsigned long long)hw.writtenValue[11]<<32)|hw.writtenValue[12])==plan.Layout.Physical);
        // Critical repeated-register case: the TMZ update must NOT restore DCC.
        CHECK(hw.writtenValue[0]==(ctl&~dcc));
        CHECK(hw.writtenValue[10]==(ctl&~(dcc|tmz)));
        CHECK(!(hw.writtenValue[9]&(HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK |
            HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_MODE_FOR_STEREOSYNC_MASK |
            HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_IN_STEREOSYNC_MASK)));
        CHECK(!(hw.writtenValue[2]&HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK));
        CHECK(Bc250DispGet(hw.writtenValue[3],HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)==1919);
    }
}
static void Faults(void)
{
    unsigned pipe,i,after,n;
    BC250_SURFACE_WRITE_REQUEST r=Request(BC250_LINEAR_RGB10A2);
    BC250_SURFACE_WRITE_PLAN plan;
    CHECK(Bc250SurfaceWritePrepare(&r,&plan));
    for(pipe=0;pipe<2;++pipe) {
        unsigned long expectedAddr[13],expectedValue[13];
        BC250_DISP_IO io=Setup(pipe,42);
        CHECK(Bc250SurfaceWriteApplyLocked(&io,pipe,&plan,&n)==0);
        memcpy(expectedAddr,hw.writtenAddr,sizeof(expectedAddr));memcpy(expectedValue,hw.writtenValue,sizeof(expectedValue));
        for(i=1;i<=13;++i) {
            io=Setup(pipe,42);hw.failRead=i;
            CHECK(Bc250SurfaceWriteApplyLocked(&io,pipe,&plan,&n)==BC250_DISP_STATUS_DEVICE);
            CHECK(!n && !hw.writes && hw.reads==i);
        }
        for(after=0;after<2;++after) for(i=1;i<=13;++i) {
            unsigned observed=i-1+after;
            io=Setup(pipe,42);hw.failWrite=i;hw.after=after;
            CHECK(Bc250SurfaceWriteApplyLocked(&io,pipe,&plan,&n)==BC250_DISP_STATUS_DEVICE);
            CHECK(n==i && hw.writes==i && !hw.stalls);
            CHECK(!memcmp(expectedAddr,hw.writtenAddr,observed*sizeof(unsigned long)));
            CHECK(!memcmp(expectedValue,hw.writtenValue,observed*sizeof(unsigned long)));
            CHECK(Get(BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK)&OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK);
        }
        for(i=0;i<8;++i) {
            unsigned long f=pipe ? BC250_REG_DMU_HUBPREQ1_DCSURF_FLIP_CONTROL : BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL;
            io=Setup(pipe,42);
            switch(i) {
            case 0:Set(BC250_REG_DMU_OTG0_OTG_CONTROL,0);break;
            case 1:Set(BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK);break;
            case 2:Set(BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK);break;
            case 3:Set(pipe ? BC250_REG_DMU_HUBP1_DCHUBP_CNTL : BC250_REG_DMU_HUBP0_DCHUBP_CNTL,HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK);break;
            case 4:Set(f,Get(f)|HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK);break;
            case 5:Set(f,Get(f)&~HUBPREQ0_DCSURF_FLIP_CONTROL__HUBPREQ_MASTER_UPDATE_LOCK_STATUS_MASK);break;
            case 6:Set(f,Get(f)|HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK);break;
            default:Set(pipe ? BC250_REG_DMU_HUBPREQ1_DCSURF_FLIP_CONTROL2 : BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL2,
                HUBPREQ0_DCSURF_FLIP_CONTROL2__SURFACE_TRIPLE_BUFFER_ENABLE_MASK);break;
            }
            CHECK(Bc250SurfaceWriteApplyLocked(&io,pipe,&plan,&n)==(i==4 ? BC250_DISP_STATUS_BUSY : BC250_DISP_STATUS_MISMATCH));
            CHECK(!n && !hw.writes);
        }
    }
}
static void Invalid(void)
{
    unsigned i,n;
    BC250_SURFACE_WRITE_PLAN plan,zero={0};
    BC250_SURFACE_WRITE_REQUEST r=Request(BC250_LINEAR_BGRA8);
    BC250_DISP_IO io=Setup(0,0);
    for(i=0;i<8;++i) {
        r=Request(BC250_LINEAR_BGRA8);
        switch(i) {
        case 0:r.NumPipes=0;break;case 1:r.NumBanks=3;break;
        case 2:r.NumShaderEngines=0;break;case 3:r.NumRbPerSe=3;break;
        case 4:r.MaxCompressedFrags=0;break;case 5:r.PipeInterleave=0xffffffffu;break;
        case 6:r.NumPipes=0x80000000u;break;default:r.Surface.SurfaceOffset=r.Surface.AllocationBytes;break;
        }
        memset(&plan,0xff,sizeof(plan));
        CHECK(!Bc250SurfaceWritePrepare(&r,&plan));CHECK(!memcmp(&plan,&zero,sizeof(plan)));
    }
    r=Request(BC250_LINEAR_BGRA8);CHECK(Bc250SurfaceWritePrepare(&r,&plan));
    CHECK(Bc250SurfaceWriteApplyLocked(&io,2,&plan,&n)==BC250_DISP_STATUS_INVALID);
    CHECK(!hw.reads && !hw.writes && !n);
    plan.Words[12]^=4096;
    CHECK(Bc250SurfaceWriteApplyLocked(&io,0,&plan,&n)==BC250_DISP_STATUS_INVALID);
    CHECK(!hw.reads && !hw.writes);
    {
        union { BC250_SURFACE_WRITE_REQUEST request; BC250_SURFACE_WRITE_PLAN plan; } overlap;
        overlap.request=r;CHECK(Bc250SurfaceWritePrepare(&overlap.request,&overlap.plan));CHECK(overlap.plan.Ready==1);
    }
}
int main(void)
{
    Successful();Faults();Invalid();
    printf("Surface writer ordering, lock and fault boundaries: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
