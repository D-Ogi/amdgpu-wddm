/* Finite vendor SetPixelClock transaction. AMD Linux 7d0a66e4, MIT,
 * command_table2.c set_pixel_clock_v7 establishes the operation. CR selectors
 * below are firmware-defined operands, not invented public register names.
 * No raw index window or general PSP command channel is exposed. */
#include "link_clock_ports.inc"
typedef struct CLOCK_SCOPE {
    BC250_LINK_ATOM* Link;
    uint32_t Selector, LastSelector, LastData, Before2, Before4, BeforeCntl, BeforeUpdate;
    unsigned HaveSelector, HaveData, Have2, Have4, HaveCntl, HaveUpdate;
    unsigned MailboxReady, MailboxArgument, MailboxSent;
} CLOCK_SCOPE;
int Bc250LinkAtomScopedRegister(const BC250_LINK_ATOM* l,uint32_t r,int write)
{
    (void)write;
    if (!l || !l->ClockScope) return 0;
    return r==CLOCK_PHY2 || r==CLOCK_PHY4 || r==CLOCK_PLL_UPDATE ||
        r==CLOCK_CR_ADDR || r==CLOCK_CR_DATA || r==CLOCK_CNTL ||
        r==CLOCK_DEBUG || r==CLOCK_TX_DEBUG || r==CLOCK_PSP33 || r==CLOCK_PSP34;
}
static int clock_selector(uint32_t value)
{
    unsigned lane;
    if (value==0xc000 || value==0x56 || value==0x46 || value==0x50 ||
        value==0x84 || value==0x85) return 1;
    for (lane=0;lane<4;++lane)
        if (value==0x3007+lane*0x100 || value==0x3008+lane*0x100) return 1;
    for (lane=0;lane<8;++lane)
        if (value==0x2021+lane*3 || value==0x2022+lane*3) return 1;
    return 0;
}
static int clock_data(CLOCK_SCOPE* s,uint32_t value)
{
    unsigned lane;
    if (!s->HaveSelector) return 0;
    if (s->Selector==0xc000) return value==0;
    if (s->Selector==0x56) return value==5;
    if (s->Selector==0x46 || s->Selector==0x50) return value==0x30;
    for (lane=0;lane<4;++lane) {
        if (s->Selector==0x3008+lane*0x100) return value==0x9b;
        if (s->Selector==0x3007+lane*0x100)
            return s->HaveData && s->LastSelector==s->Selector &&
                value==((s->LastData&0xffffe000u)|0x2546);
    }
    if (s->Selector==0x84 || s->Selector==0x85) {
        for (lane=0;lane<8;++lane)
            if (s->HaveData && s->LastSelector==
                (s->Selector==0x84?0x2021u:0x2022u)+lane*3)
                return value==s->LastData;
    }
    return 0;
}
static int scope_owned(void* p)
{ CLOCK_SCOPE* s=p;return s->Link->Io.Owned(s->Link->Io.Context); }
static int scope_allowed(void* p,uint32_t r,int write)
{ CLOCK_SCOPE* s=p;return s->Link->Io.Allowed(s->Link->Io.Context,r,write); }
static uint64_t scope_now(void* p)
{ CLOCK_SCOPE* s=p;return s->Link->Io.NowUs(s->Link->Io.Context); }
static int scope_delay(void* p,uint32_t us)
{ CLOCK_SCOPE* s=p;return s->Link->Io.DelayUs(s->Link->Io.Context,us); }
static int scope_read(void* p,uint32_t r,uint32_t* value)
{
    CLOCK_SCOPE* s=p;int result;
    if (r==CLOCK_CR_DATA && !s->HaveSelector) return -1;
    result=s->Link->Io.Read(s->Link->Io.Context,r,value);
    if (result) return result;
    if (r==CLOCK_CR_DATA) {
        s->LastData=*value;s->LastSelector=s->Selector;s->HaveData=1;
    } else if (r==CLOCK_PHY2) {s->Before2=*value;s->Have2=1;}
    else if (r==CLOCK_PHY4) {s->Before4=*value;s->Have4=1;}
    else if (r==CLOCK_CNTL) {s->BeforeCntl=*value;s->HaveCntl=1;}
    else if (r==CLOCK_PLL_UPDATE) {s->BeforeUpdate=*value;s->HaveUpdate=1;}
    else if (r==CLOCK_PSP33) s->MailboxReady=(*value&0x80000000u)!=0;
    return 0;
}
static int scope_write(void* p,uint32_t r,uint32_t value)
{
    CLOCK_SCOPE* s=p;int result,allowed=1;
    if (r==CLOCK_CR_ADDR) allowed=clock_selector(value);
    else if (r==CLOCK_CR_DATA) allowed=clock_data(s,value);
    else if (r==CLOCK_PHY2) allowed=s->Have2 && value==(s->Before2|8u);
    else if (r==CLOCK_PHY4) allowed=s->Have4 && value==((s->Before4&0xf8f8f8f8u)|0x07070707u);
    else if (r==CLOCK_PLL_UPDATE) allowed=s->HaveUpdate && value==(s->BeforeUpdate|1u);
    else if (r==CLOCK_DEBUG) allowed=value==0;
    else if (r==CLOCK_TX_DEBUG) allowed=value==0x4000;
    else if (r==CLOCK_CNTL) allowed=s->HaveCntl &&
        (value==((s->BeforeCntl&0xff0fffffu)|0x00400000u) ||
         value==(s->BeforeCntl|0x1000u) || value==(s->BeforeCntl|0x2000u) ||
         value==(s->BeforeCntl|0x4000u) || value==(s->BeforeCntl|0x8000u));
    else if (r==CLOCK_PSP34) allowed=!s->MailboxSent && s->MailboxReady && value==0;
    else if (r==CLOCK_PSP33) allowed=!s->MailboxSent && s->MailboxReady &&
        s->MailboxArgument && value==0x00050000;
    if (!allowed) return -1;
    result=s->Link->Io.Write(s->Link->Io.Context,r,value);
    if (result) return result;
    if (r==CLOCK_CR_ADDR) {s->Selector=value;s->HaveSelector=1;}
    if (r==CLOCK_PSP34) s->MailboxArgument=1;
    if (r==CLOCK_PSP33) {s->MailboxSent=1;s->MailboxReady=0;}
    return 0;
}
