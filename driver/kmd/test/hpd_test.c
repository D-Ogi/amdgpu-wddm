#include <stdio.h>
#include <string.h>
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../../shim/bc250_hpd.h"

static unsigned checks,failed;
#define CHECK(x) do { ++checks;if (!(x)) { ++failed;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x); } } while (0)
#define IM(f) HPD0_DC_HPD_INT_CONTROL__##f##_MASK
#define SM(f) HPD0_DC_HPD_INT_STATUS__##f##_MASK
typedef struct MODEL { unsigned long control,interrupt,status,writes,failAt; } MODEL;
static long Read(void* c,unsigned long r,unsigned long* v)
{
    MODEL* m=c;
    switch (r) {
    case BC250_REG_DMU_HPD0_DC_HPD_INT_STATUS: *v=m->status;return 0;
    case BC250_REG_DMU_HPD0_DC_HPD_INT_CONTROL: *v=m->interrupt;return 0;
    case BC250_REG_DMU_HPD0_DC_HPD_CONTROL: *v=m->control;return 0;
    default: CHECK(0);return BC250_DISP_STATUS_ACCESS_DENIED;
    }
}
static long Write(void* c,unsigned long r,unsigned long v)
{
    MODEL* m=c;
    ++m->writes;
    if (m->writes==m->failAt) return BC250_DISP_STATUS_DEVICE;
    switch (r) {
    case BC250_REG_DMU_HPD0_DC_HPD_INT_CONTROL:
        if (v & IM(DC_HPD_INT_ACK)) m->status &= ~SM(DC_HPD_INT_STATUS);
        if (v & IM(DC_HPD_RX_INT_ACK)) m->status &= ~SM(DC_HPD_RX_INT_STATUS);
        m->interrupt=v & ~(IM(DC_HPD_INT_ACK)|IM(DC_HPD_RX_INT_ACK));return 0;
    case BC250_REG_DMU_HPD0_DC_HPD_CONTROL: m->control=v;return 0;
    default: CHECK(0);return BC250_DISP_STATUS_ACCESS_DENIED;
    }
}
int main(void)
{
    MODEL m={0};BC250_HPD h={0};BC250_DISP_IO io={0};int sense=0,rx=0;unsigned i;
    io.Context=&m;io.Read=Read;io.Write=Write;
    m.control=0x122; // retained debounce fields; no enable
    m.status=SM(DC_HPD_SENSE_DELAYED);
    CHECK(Bc250HpdArm(&h,&io)==0 && h.Armed);
    CHECK(!(m.interrupt & IM(DC_HPD_INT_POLARITY))); // next edge is disconnect
    CHECK(Bc250HpdInterrupt(&h,&sense,&rx)==0);
    m.status=SM(DC_HPD_INT_STATUS);
    CHECK(Bc250HpdInterrupt(&h,&sense,&rx)==1 && !sense && !rx);
    CHECK(m.interrupt & IM(DC_HPD_INT_POLARITY));
    CHECK(!(m.status & SM(DC_HPD_INT_STATUS)));
    m.status=SM(DC_HPD_INT_STATUS)|SM(DC_HPD_SENSE_DELAYED)|SM(DC_HPD_RX_INT_STATUS);
    CHECK(Bc250HpdInterrupt(&h,&sense,&rx)==1 && sense && rx);
    CHECK(!(m.interrupt & IM(DC_HPD_INT_POLARITY)));
    CHECK(!(m.status & (SM(DC_HPD_INT_STATUS)|SM(DC_HPD_RX_INT_STATUS))));
    m.status|=SM(DC_HPD_RX_INT_STATUS);
    CHECK(Bc250HpdInterrupt(&h,&sense,&rx)==1 && sense && rx);
    CHECK(Bc250HpdDisarm(&h)==0 && !h.Saved && !h.Armed);
    CHECK(m.control==0x122 && m.interrupt==0);
    // A pre-enabled firmware source must be quiesced when our ISR is removed.
    memset(&m,0,sizeof(m));memset(&h,0,sizeof(h));
    m.interrupt=IM(DC_HPD_INT_EN)|IM(DC_HPD_RX_INT_EN)|IM(DC_HPD_INT_POLARITY);
    m.control=HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK;
    CHECK(Bc250HpdArm(&h,&io)==0 && h.Armed);
    CHECK(Bc250HpdDisarm(&h)==0 && !h.Saved && !h.Armed);
    CHECK(m.interrupt==IM(DC_HPD_INT_POLARITY));
    CHECK(m.control==HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK);
    // A failed write at each arm step leaves an explicit restore obligation.
    for (i=1;i<=4;++i) {
        memset(&m,0,sizeof(m));memset(&h,0,sizeof(h));m.failAt=i;
        CHECK(Bc250HpdArm(&h,&io)<0 && h.Saved && !h.Armed);
        m.failAt=0;
        CHECK(Bc250HpdDisarm(&h)==0 && m.control==0 && m.interrupt==0);
    }
    printf("hpd_test: %u checks, %u failures\n",checks,failed);
    return failed?1:0;
}
