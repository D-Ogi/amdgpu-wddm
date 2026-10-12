// PROVENANCE: AMD DC (MIT), Linux v6.18 7d0a66e4; hpd0_ack is extracted unchanged.
#include "bc250_hpd.h"
#include "../kmd/regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "bc250_atom_types.h"
#if defined(BC250_SHIM_KERNEL)
typedef unsigned char bool;
#define true 1
#else
#include <stdbool.h>
#endif

#define STATUS_REG BC250_REG_DMU_HPD0_DC_HPD_INT_STATUS
#define CONTROL_REG BC250_REG_DMU_HPD0_DC_HPD_CONTROL
#define INTERRUPT_REG BC250_REG_DMU_HPD0_DC_HPD_INT_CONTROL
#define IM(f) HPD0_DC_HPD_INT_CONTROL__##f##_MASK
#define SM(f) HPD0_DC_HPD_INT_STATUS__##f##_MASK
#define ENABLES (IM(DC_HPD_INT_EN)|IM(DC_HPD_RX_INT_EN))
#define ACKS (IM(DC_HPD_INT_ACK)|IM(DC_HPD_RX_INT_ACK))

struct irq_service { BC250_HPD* ctx; };
struct irq_source_info { uint32_t status_reg,enable_reg,ack_reg,ack_mask,ack_value; };
static uint32_t dm_read_reg(BC250_HPD* h,uint32_t r)
{
    unsigned long v=0;long s;
    if (!h->Error && (s=Bc250DispRead(h->Io,r,&v))<0) h->Error=s;
    return (uint32_t)v;
}
static void dm_write_reg(BC250_HPD* h,uint32_t r,uint32_t v)
{
    long s;
    if (!h->Error && (s=Bc250DispWrite(h->Io,r,v))<0) h->Error=s;
}
static void dal_irq_service_ack_generic(struct irq_service* service,const struct irq_source_info* info)
{
    uint32_t v=dm_read_reg(service->ctx,info->ack_reg);
    dm_write_reg(service->ctx,info->ack_reg,(v & ~info->ack_mask)|info->ack_value);
}
#define get_reg_field_value(v,r,f) Bc250DispGet((v),r##__##f##_MASK)
#define set_reg_field_value(v,w,r,f) ((v)=((v)&~r##__##f##_MASK)|Bc250DispField(r##__##f##_MASK,(w)))
#include "hpd_ack.inc"
#undef get_reg_field_value
#undef set_reg_field_value

long Bc250HpdSense(BC250_HPD* h,int* sense)
{
    unsigned long v;long s;
    *sense=0;
    s=Bc250DispRead(h->Io,STATUS_REG,&v);
    if (s>=0) *sense=(v & SM(DC_HPD_SENSE_DELAYED))!=0;
    return s;
}

long Bc250HpdArm(BC250_HPD* h,BC250_DISP_IO* io)
{
    struct irq_service service={h};
    const struct irq_source_info info={STATUS_REG,INTERRUPT_REG,INTERRUPT_REG,ACKS,ACKS};
    unsigned long value;
    h->Io=io;h->Error=0;h->Armed=0;
    if (h->Saved) return BC250_DISP_STATUS_BUSY;
    h->BeforeControl=dm_read_reg(h,CONTROL_REG);
    h->BeforeInterrupt=dm_read_reg(h,INTERRUPT_REG);
    if (h->Error) return h->Error;
    h->Saved=1; // obligation begins before a write, including a failed posted write
    // dcn10_link_encoder_enable_hpd: enable sensing. The GPIO/board debounce
    // fields are retained; the passive worker also coalesces contact bounce.
    dm_write_reg(h,CONTROL_REG,h->BeforeControl|HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK);
    (void)hpd0_ack(&service,&info); // clear old latches and arm the opposite edge
    value=dm_read_reg(h,INTERRUPT_REG);
    dm_write_reg(h,INTERRUPT_REG,(value & ~ACKS)|ENABLES);
    if (!h->Error) h->Armed=1;
    return h->Error;
}

long Bc250HpdDisarm(BC250_HPD* h)
{
    unsigned long v;long s,first=0;
    h->Armed=0;
    if (!h->Saved) return 0;
    s=Bc250DispRead(h->Io,INTERRUPT_REG,&v);
    if (s>=0) s=Bc250DispWrite(h->Io,INTERRUPT_REG,
        (v & ~(ENABLES|IM(DC_HPD_INT_POLARITY)|ACKS)) |
        // The driver no longer owns an ISR after stop. Retain firmware polarity,
        // but never restore an enabled interrupt into an unserviced device.
        (h->BeforeInterrupt & IM(DC_HPD_INT_POLARITY)) | ACKS);
    if (s<0) first=s;
    s=Bc250DispUpdate(h->Io,CONTROL_REG,HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK,h->BeforeControl);
    if (s<0 && first>=0) first=s;
    h->Error=first;
    if (!first) h->Saved=0;
    return first;
}

int Bc250HpdInterrupt(BC250_HPD* h,int* sense,int* rx)
{
    unsigned long v,control;long s;
    struct irq_service service={h};
    struct irq_source_info info={STATUS_REG,INTERRUPT_REG,INTERRUPT_REG,0,0};
    *sense=*rx=0;h->Events=0;
    if (!h->Armed) return 0;
    s=Bc250DispRead(h->Io,STATUS_REG,&v);
    if (s<0) { h->Error=s;return -1; }
    s=Bc250DispRead(h->Io,INTERRUPT_REG,&control);
    if (s<0) { h->Error=s;return -1; }
    if ((v & SM(DC_HPD_INT_STATUS)) && (control & IM(DC_HPD_INT_EN))) {
        info.ack_value|=IM(DC_HPD_INT_ACK);h->Events|=1;
    }
    if ((v & SM(DC_HPD_RX_INT_STATUS)) && (control & IM(DC_HPD_RX_INT_EN))) {
        info.ack_value|=IM(DC_HPD_RX_INT_ACK);*rx=1;h->Events|=2;
    }
    if (!info.ack_value) return 0;
    *sense=(v & SM(DC_HPD_SENSE_DELAYED))!=0;
    info.ack_mask=ACKS;h->Error=0;
    if (info.ack_value & IM(DC_HPD_INT_ACK)) (void)hpd0_ack(&service,&info);
    else dal_irq_service_ack_generic(&service,&info);
    return h->Error ? -1 : 1;
}
