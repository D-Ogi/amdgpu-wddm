/* Checked HDMI conversion: fake MMIO, private ROM optional, no device access. */
#define _CRT_SECURE_NO_WARNINGS
#include "../../shim/bc250_hdmi_link.h"
#include "../../shim/bc250_link_atom.h"
#include "../../shim/bc250_link_regs.h"
#include "../regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define R(n) (BC250_REG_DMU_DIG0_##n/4u)
#define CHECK(e) do { ++checks; if (!(e)) { ++failures; printf("line %u: %s\n",__LINE__,#e); } } while(0)
static unsigned checks,failures;
typedef struct MOCK {
    uint32_t reg[131072], original[131072], packet[8][9], oldPacket[8][9];
    uint64_t clock;
    unsigned ops,failOp,writeOps,failWrite,postFailure,corruptFe,starts,oldVerifierFailures,statusSample,denyOwned;
    unsigned delays,unknownChanges,commandReplays,gcReads,flipGcRead;
    unsigned packetProtocol,prepareStage,conflictRemaining,conflictForever,conflictReads,conflictClears;
    unsigned packetWrites,packetCommits,protocolErrors,corruptPacket,failPacketRead,failPacketWrite;
    unsigned packetReads,packetNext,expectStop,stopSeen;
} MOCK;
static unsigned slot(uint32_t r)
{
    switch(r) {
    case R(AFMT_GENERIC_HDR): return 0;
    case R(AFMT_GENERIC_0): return 1; case R(AFMT_GENERIC_1): return 2;
    case R(AFMT_GENERIC_2): return 3; case R(AFMT_GENERIC_3): return 4;
    case R(AFMT_GENERIC_4): return 5; case R(AFMT_GENERIC_5): return 6;
    case R(AFMT_GENERIC_6): return 7; case R(AFMT_GENERIC_7): return 8;
    default: return 9;
    }
}
static unsigned bank(MOCK*m)
{ return (m->reg[R(AFMT_VBI_PACKET_CONTROL)]&DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_INDEX_MASK)>>DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_INDEX__SHIFT; }
static int Owned(void*p) { return !((MOCK*)p)->denyOwned; }
static int Allowed(void*p,uint32_t r,int w) { (void)p;return Bc250HdmiLinkAllowed(r,w); }
static int Read(void*p,uint32_t r,uint32_t*v)
{
    MOCK*m=p; unsigned s=slot(r);
    if (++m->ops==m->failOp) return -1;
    // Independent device model: the indexed port can return stale data. It is
    // deliberately readable here so a reintroduced verifier sees the .46 value.
    if(s<9)++m->packetReads;
    if (s>=9 && !Bc250HdmiLinkAllowed(r,0)) return -1;
    if(r==R(HDMI_GC) && ++m->gcReads==m->flipGcRead)m->reg[r]^=DIG0_HDMI_GC__HDMI_GC_AVMUTE_CONT_MASK;
    if(s<9 && bank(m)>=8) {++m->protocolErrors;return -1;}
    if(s<9 && m->failPacketRead && r==R(AFMT_GENERIC_3))return -1;
    if(m->packetProtocol && r==R(AFMT_VBI_PACKET_CONTROL) && m->prepareStage<3) {
        ++m->conflictReads;
        if(m->conflictForever || m->conflictRemaining) {
            if(m->conflictRemaining)--m->conflictRemaining;
            m->reg[r]|=DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_CONFLICT_MASK;
        } else {
            m->reg[r]&=~DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_CONFLICT_MASK;
            if(m->prepareStage==1)m->prepareStage=2;
        }
    }
    *v=s<9?UINT32_C(0x8047267F):m->reg[r]; return 0;
}
static int Write(void*p,uint32_t r,uint32_t v)
{
    MOCK*m=p; unsigned s=slot(r); int fail=(++m->writeOps==m->failWrite);
    const BC250_HDMI_FIELD_POLICY *policy=Bc250HdmiLinkPolicy(r);
    uint32_t submitted=v;
    if (++m->ops==m->failOp) fail=1;
    if (!Bc250HdmiLinkAllowed(r,1)) return -1;
    if (!policy) return -1;
    if(((m->reg[r]^v)&policy->Unknown)!=0)++m->unknownChanges;
    if((v&policy->Status)!=0)++m->commandReplays;
    if(s<9 && bank(m)>=8) {++m->protocolErrors;return -1;}
    if (s<9 && !(m->reg[R(AFMT_CNTL)]&DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK)) return -1;
    if(m->expectStop && !m->stopSeen) {
        if(r!=R(HDMI_GENERIC_PACKET_CONTROL0) ||
           (v&(DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_CONT_MASK|
               DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_SEND_MASK))) {
            ++m->protocolErrors;return -1;
        }
        if(!fail || m->postFailure)m->stopSeen=1;
    }
    if(m->packetProtocol && s<9) {
        if(m->prepareStage!=4 || bank(m)!=0 ||
           (m->reg[BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u]&DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK)) {
            ++m->protocolErrors;return -1;
        }
        if(s!=m->packetNext) {++m->protocolErrors;return -1;}
        ++m->packetWrites;
        if(m->failPacketWrite && r==R(AFMT_GENERIC_3))fail=1;
    }
    if (fail && !m->postFailure) return -1;
    if(m->packetProtocol) {
        if(r==R(AFMT_CNTL) && (v&DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK)) {m->prepareStage=1;m->packetNext=0;}
        if(r==R(AFMT_VBI_PACKET_CONTROL)) {
            if(v&DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_CONFLICT_CLR_MASK) {
                if(m->prepareStage!=2) {++m->protocolErrors;return -1;}
                ++m->conflictClears;m->prepareStage=3;
            } else if(!(v&DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_INDEX_MASK) && m->prepareStage==3)
                m->prepareStage=4;
        }
        if(r==R(AFMT_VBI_PACKET_CONTROL1) && (v&DIG0_AFMT_VBI_PACKET_CONTROL1__AFMT_GENERIC0_IMMEDIATE_UPDATE_MASK)) {
            if(m->prepareStage!=4 || m->packetNext!=9) {++m->protocolErrors;return -1;}
            ++m->packetCommits;
        }
    }
    // A completed FIFO resync is not a latched configuration bit. The former
    // model retained DIG_START and therefore missed the .35 lab refusal.
    if (r==R(DIG_FE_CNTL)) {
        if (v&DIG0_DIG_FE_CNTL__DIG_START_MASK) ++m->starts;
        if(m->corruptFe) v^=DIG0_DIG_FE_CNTL__TMDS_PIXEL_ENCODING_MASK;
    }
    // Hardware status is autonomous; command writes self-clear. Keep powered
    // FE status set, and settle packet-access conflict so REG_WAIT can finish.
    v &= ~(policy->Command|policy->Status);
    v |= m->statusSample & policy->Status;
    if(r==R(DIG_FE_CNTL)) v|=DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
    if(r==R(HDMI_DB_CONTROL)) v|=(m->reg[r]^policy->Status)&policy->Status;
    if((submitted^v)&~policy->Configuration) ++m->oldVerifierFailures;
    if (s<9) {
        m->packet[bank(m)][s]=v^((m->corruptPacket && r==R(AFMT_GENERIC_3))?1u:0u);
        if(m->packetProtocol)++m->packetNext;
    } else m->reg[r]=v;
    return fail?-1:0;
}
static uint64_t Now(void*p) { return ++((MOCK*)p)->clock; }
static int Delay(void*p,uint32_t us) { MOCK*m=p;++m->delays;m->clock+=us;return 0; }
static void init(MOCK*m)
{
    unsigned i,j;
    memset(m,0,sizeof(*m));
    m->reg[R(DIG_BE_CNTL)]=0x00020100; m->reg[R(DIG_BE_EN_CNTL)]=0x101;
    m->reg[R(DIG_FE_CNTL)]=DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
    m->reg[R(HDMI_CONTROL)]=0x10011;
    // Reserved bits are not configuration evidence, but RMW must preserve them.
    m->reg[R(AFMT_CNTL)]=UINT32_C(0x80000000);
    m->reg[R(HDMI_DB_CONTROL)]=DIG0_HDMI_DB_CONTROL__HDMI_DB_PENDING_MASK|
        DIG0_HDMI_DB_CONTROL__VUPDATE_DB_PENDING_MASK;
    // Bit31 belongs to the four-bit INDEX, not to reserved state. Keep this
    // fixture's bank inside its eight modeled banks and preserve actual bit27.
    m->reg[R(AFMT_VBI_PACKET_CONTROL)]=UINT32_C(0x08000000)|
        (3u<<DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_INDEX__SHIFT);
    for (i=0;i<8;++i) for(j=0;j<9;++j) m->packet[i][j]=0x100000*i+0x123*j;
    memcpy(m->oldPacket,m->packet,sizeof(m->packet)); memcpy(m->original,m->reg,sizeof(m->reg));
}
static int start(MOCK*m,BC250_HDMI_LINK*l,BC250_ATOM_IMAGE*im,BC250_ATOM_PLAN*plan,void*w)
{ BC250_ATOM_CALLBACKS io={m,Owned,Allowed,Read,Write,Now,Delay};
  return Bc250HdmiLinkStart(l,&io,im,plan,w,Bc250AtomContextSize(),174500,1920,1080,2080,1119); }
static void restored(MOCK*m,BC250_HDMI_LINK*l)
{
    unsigned r;int same=1;
    BC250_HDMI_RESTORE_DETAIL streamFailure=l->StreamFailure;uint32_t streamStage=l->StreamStage;
    if(l->PacketDirty) {
        unsigned i,power=0,clock=0,selector=0;
        for(i=0;i<l->Count;++i) {
            power|=l->Offset[i]==BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u;
            clock|=l->Offset[i]==R(AFMT_CNTL);
            selector|=l->Offset[i]==R(AFMT_VBI_PACKET_CONTROL);
        }
        // Includes every injected partial-start and repeated-restore path:
        // temporary preparation must never touch unjournaled configuration.
        CHECK(power && clock && selector);
    }
    m->failOp=m->failWrite=0;
    CHECK(Bc250HdmiLinkRestore(l)==0);
    for(r=0;r<sizeof(m->reg)/sizeof(m->reg[0]);++r) {
        const BC250_HDMI_FIELD_POLICY *p=Bc250HdmiLinkPolicy(r);
        uint32_t mask=p?~(p->Command|p->Status):UINT32_MAX;
        if((m->reg[r]^m->original[r])&mask) same=0;
    }
    CHECK(same);
    // Packet 0 was inactive on entry. Its new RAM contents need not be rolled
    // back; all other packet banks must remain byte-for-byte unchanged.
    CHECK(memcmp(&m->packet[1],&m->oldPacket[1],7*sizeof(m->packet[0]))==0);
    CHECK(!m->packetReads);
    CHECK(!memcmp(&streamFailure,&l->StreamFailure,sizeof(streamFailure)) && streamStage==l->StreamStage);
}
static void restoreProtocol(MOCK*m,BC250_HDMI_LINK*l,BC250_ATOM_IMAGE*im,BC250_ATOM_PLAN*plan,void*w)
{
    unsigned before=checks,bad=failures,count,i,writes,commits,reads;
    BC250_HDMI_RESTORE_DETAIL first;
    BC250_ATOM_RESULT atom;
    init(m);memset(l,0,sizeof(*l));m->packetProtocol=1;m->conflictRemaining=3;
    CHECK(start(m,l,im,plan,w)==0);
    CHECK(m->packetWrites==9 && m->packetCommits==1 && m->conflictClears==1);
    CHECK(m->delays==3 && !m->protocolErrors && !m->packetReads);
    CHECK(m->packet[0][0]==UINT32_C(0x000D0282));
    writes=m->packetWrites;commits=m->packetCommits;reads=m->packetReads;
    m->reg[R(AFMT_CNTL)]&=~DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK;
    m->expectStop=1;restored(m,l);
    CHECK(m->stopSeen && !m->protocolErrors && !l->RestoreFailure.Valid);
    CHECK(m->packetWrites==writes && m->packetCommits==commits && m->packetReads==reads);
    CHECK(bank(m)==3 && !l->Dirty && !l->PacketDirty && !l->Count);
    // A control access failure, including a posted write, retains the complete
    // journal and first detail. Retry must not replay packet data or commands.
    for(i=0;i<3;++i) {
        init(m);memset(l,0,sizeof(*l));CHECK(start(m,l,im,plan,w)==0);
        count=l->Count;atom=l->Atom;
        l->FailureRegister=123;l->FailureOperation=6;l->FailureWanted=7;l->FailureReadback=8;l->FailureMask=9;
        if(i==0)m->failOp=m->ops+1;
        else {m->failWrite=m->writeOps+1;m->postFailure=i==2;}
        CHECK(Bc250HdmiLinkRestore(l)!=0);
        CHECK(l->Dirty && l->PacketDirty && l->Count==count);
        CHECK(l->RestoreFailure.Valid && l->RestoreFailure.Register==R(HDMI_GENERIC_PACKET_CONTROL0));
        CHECK(l->RestoreFailure.Operation==(i==0?1u:3u));
        CHECK(l->RestoreFailure.ReadbackValid==(i==0?0u:1u));
        CHECK(l->FailureRegister==123 && l->FailureOperation==6 && l->FailureWanted==7 &&
            l->FailureReadback==8 && l->FailureMask==9 && !memcmp(&atom,&l->Atom,sizeof(atom)));
        first=l->RestoreFailure;restored(m,l);
        CHECK(!memcmp(&first,&l->RestoreFailure,sizeof(first)) && !m->packetReads);
    }
    printf("Restore protocol: %u checks, %u failures; inactive packet controls and retry retention\n",checks-before,failures-bad);
}
static void policyModel(MOCK*m)
{
    unsigned r,bit,registers=0,dataRegisters=0;
    for(r=0;r<sizeof(m->reg)/sizeof(m->reg[0]);++r) {
        const BC250_HDMI_FIELD_POLICY *p=Bc250HdmiLinkPolicy(r);
        if(!p) { CHECK(!Bc250HdmiLinkAllowed(r,1));continue; }
        ++registers;
        CHECK(!(p->Configuration&(p->Command|p->Status|p->Unknown|p->PacketData)));
        CHECK(!(p->Command&(p->Status|p->Unknown|p->PacketData)));
        CHECK(!(p->Status&(p->Unknown|p->PacketData)));
        CHECK(!(p->Unknown&p->PacketData));
        if(p->PacketData) {
            ++dataRegisters;CHECK(p->PacketData==UINT32_MAX && slot(r)<9);
            CHECK(!Bc250HdmiLinkAllowed(r,0) && Bc250HdmiLinkAllowed(r,1));
        }
        for(bit=0;bit<32;++bit) {
            uint32_t b=UINT32_C(1)<<bit,after;
            if(!(b&(p->Configuration|p->Command|p->Status)))continue;
            init(m);m->reg[R(AFMT_CNTL)]|=DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK;
            m->statusSample=b;
            if(r==R(HDMI_DB_CONTROL)) m->reg[r]=0;
            CHECK(Write(m,r,(b&p->Configuration)|(b&p->Command))==0);
            CHECK(Read(m,r,&after)==0);
            CHECK((after&p->Configuration)==(b&p->Configuration));
            CHECK(!(after&p->Command));
            if(b&p->Configuration) CHECK(((after^b^b)&p->Configuration)!=0);
            if(b&p->Status) CHECK((after&b)!=0);
        }
    }
    CHECK(registers==26 && dataRegisters==9);
}
static void coldInit(MOCK*m,BC250_HDMI_LINK*l)
{
    init(m);memset(l,0,sizeof(*l));
    m->reg[R(DIG_BE_CNTL)]=(m->reg[R(DIG_BE_CNTL)]&~DIG0_DIG_BE_CNTL__DIG_MODE_MASK)|
        (3u<<DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT);
    m->reg[R(DIG_BE_EN_CNTL)]=0;
    m->reg[R(DIG_FE_CNTL)]&=~DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK;
    memcpy(m->original,m->reg,sizeof(m->reg));
}
static int coldProgram(MOCK*m,BC250_HDMI_LINK*l)
{
    BC250_ATOM_CALLBACKS io={m,Owned,Allowed,Read,Write,Now,Delay};
    return Bc250HdmiStreamProgram(l,&io,148500,1920,1080,2200,1125);
}
static void coldTests(MOCK*m,BC250_HDMI_LINK*l)
{
    unsigned ops,writes,i,j;
    coldInit(m,l);CHECK(coldProgram(m,l)==0);
    CHECK(l->Active && l->Dirty);
    CHECK(m->reg[R(HDMI_GC)]&DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK);
    CHECK(!m->reg[R(DIG_BE_EN_CNTL)] && !m->starts);
    ops=m->ops;writes=m->writeOps;
    CHECK(Bc250HdmiStreamMute(l,0)!=0);
    CHECK(m->writeOps==writes && !l->Error);
    CHECK(Bc250HdmiStreamMute(l,2)!=0);
    // Outer link owner, not this stream shim, enables TX and later unblanks.
    m->reg[R(DIG_BE_EN_CNTL)]=DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
    CHECK(Bc250HdmiStreamMute(l,0)!=0); // still no symclk acknowledgement
    m->reg[R(DIG_BE_EN_CNTL)]|=DIG0_DIG_BE_EN_CNTL__DIG_SYMCLK_BE_ON_MASK;
    CHECK(Bc250HdmiStreamMute(l,0)==0);
    CHECK(!(m->reg[R(HDMI_GC)]&DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK));
    CHECK(Bc250HdmiStreamMute(l,1)==0);
    CHECK(m->reg[R(HDMI_GC)]&DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK);
    m->reg[R(DIG_BE_EN_CNTL)]=0;
    restored(m,l);
    for(i=1;i<=ops;++i) {
        coldInit(m,l);m->failOp=i;
        CHECK(coldProgram(m,l)!=0);
        CHECK(l->StreamFailure.Valid && l->StreamStage>=2 && l->StreamStage<=6);
        CHECK(!l->StreamFailure.ReadbackValid);restored(m,l);
    }
    for(j=0;j<2;++j)for(i=1;i<=writes;++i) {
        coldInit(m,l);m->failWrite=i;m->postFailure=j;
        CHECK(coldProgram(m,l)!=0);
        CHECK(l->StreamFailure.Valid && l->StreamFailure.Operation==3 && !l->StreamFailure.ReadbackValid);
        restored(m,l);
    }
    coldInit(m,l);m->reg[R(DIG_BE_CNTL)]&=~DIG0_DIG_BE_CNTL__DIG_MODE_MASK;
    CHECK(coldProgram(m,l)!=0);CHECK(!m->writeOps);
    coldInit(m,l);m->reg[R(DIG_BE_CNTL)]&=~DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK;
    CHECK(coldProgram(m,l)!=0);CHECK(!m->writeOps);
    coldInit(m,l);m->reg[BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u]=DIG1_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
    CHECK(coldProgram(m,l)!=0);CHECK(!m->writeOps);
    coldInit(m,l);m->denyOwned=1;
    CHECK(coldProgram(m,l)!=0);CHECK(!m->writeOps);
    coldInit(m,l);CHECK(coldProgram(m,l)==0);m->denyOwned=1;
    CHECK(Bc250HdmiStreamMute(l,1)!=0);
    m->denyOwned=0;restored(m,l);
    printf("Cold HDMI: %u writes, %u I/O calls; TX-off program, delayed unmute and cleanup tested\n",writes,ops);
}
static int LinkAllowed(void*p,uint32_t r,int write)
{ (void)p;return Bc250LinkRegisterAllowed(r,write); }
static void realAtomStreamTests(MOCK*m,BC250_HDMI_LINK*l,BC250_ATOM_IMAGE*image,BC250_ATOM_PLAN*plan,void*w)
{
    BC250_LINK_ATOM atom={0};unsigned i,measured,repeat;
    const unsigned clocks[]={148500,174500},totals[][2]={{2200,1125},{2080,1119}};
    atom.Image=image;atom.Plan=plan;atom.Workspace=w;atom.WorkspaceSize=Bc250AtomContextSize();
    atom.Io.Context=m;atom.Io.Owned=Owned;atom.Io.Allowed=LinkAllowed;
    atom.Io.Read=Read;atom.Io.Write=Write;atom.Io.NowUs=Now;atom.Io.DelayUs=Delay;
    CHECK(Bc250LinkAtomValidate(&atom)==BC250_ATOM_OK);
    for(measured=0;measured<2;++measured)for(i=0;i<2;++i) {
        BC250_ATOM_STATUS status;int stream;unsigned before,after;
        coldInit(m,l);before=m->reg[R(DIG_BE_CNTL)];
        if(measured) {
            // Six .42 inherited readings, with only mode changed to the exact
            // Route postcondition. The rest remains a synthetic register model.
            m->reg[BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u]=UINT32_C(0x6DB6D800);
            m->reg[R(DIG_FE_CNTL)]=UINT32_C(0x01000000);
            m->reg[R(HDMI_CONTROL)]=UINT32_C(0x00010011);
            m->reg[R(AFMT_CNTL)]=UINT32_C(0x00000100);
            m->reg[R(HDMI_GC)]=UINT32_C(0x00000004);
        }
        for(repeat=0;repeat<3;++repeat) {
            status=Bc250LinkAtomStream(&atom,BC250_LINK_HDMI,clocks[i],4,0);
            after=m->reg[R(DIG_BE_CNTL)];
            CHECK(status==BC250_ATOM_OK && after==before);
            memcpy(m->original,m->reg,sizeof(m->reg));memcpy(m->oldPacket,m->packet,sizeof(m->packet));
            stream=Bc250HdmiStreamProgram(l,&atom.Io,clocks[i],1920,1080,totals[i][0],totals[i][1]);
            printf("Real ATOM stream %u seed%u repeat%u: status %u BE %08x -> %08x, HDMI %d error %d op %u reg %x\n",
                clocks[i],measured,repeat,status,before,after,stream,l->Error,l->FailureOperation,l->FailureRegister);
            CHECK(stream==0 && !l->StreamFailure.Valid);restored(m,l);
        }
    }
}
static void packetLifecycleTests(MOCK*m,BC250_HDMI_LINK*l)
{
    unsigned begin=checks,bad=failures,i,j,writes,commits;
    uint8_t header[4],payload[32];uint32_t word;
    const uint32_t otherSend=DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC1_CONT_MASK|
        DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC1_SEND_MASK;
    BC250_ATOM_CALLBACKS io={m,Owned,Allowed,Read,Write,Now,Delay};
    // Read returns exactly the observed .46 stale header, never an echo. The
    // independent RAM model checks bank, power, preparation, ordered 9 words
    // and the commit; software's zero return alone is not this positive control.
    for(j=0;j<2;++j) {
        coldInit(m,l);m->packetProtocol=1;m->conflictRemaining=2;
        m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=otherSend;
        m->reg[R(HDMI_GENERIC_PACKET_CONTROL1)]=3u<<DIG0_HDMI_GENERIC_PACKET_CONTROL1__HDMI_GENERIC1_LINE__SHIFT;
        memcpy(m->original,m->reg,sizeof(m->reg));
        CHECK(Bc250HdmiStreamProgram(l,&io,j?174500:148500,1920,1080,j?2080:2200,j?1119:1125)==0);
        if(l->StreamFailure.Valid)printf("Packet witness stage%u op%u reg%x wanted%08x readback%08x mask%08x valid%u\n",
            l->StreamStage,l->StreamFailure.Operation,l->StreamFailure.Register,l->StreamFailure.Wanted,
            l->StreamFailure.Readback,l->StreamFailure.Mask,l->StreamFailure.ReadbackValid);
        CHECK(!m->packetReads && !m->protocolErrors && m->packetWrites==9 && m->packetCommits==1);
        CHECK(m->packet[0][0]==UINT32_C(0x000D0282));
        Bc250HdmiLinkAvi(1920,1080,j?2080:2200,j?1119:1125,header,payload);
        for(i=0;i<8;++i) {memcpy(&word,payload+4*i,4);CHECK(m->packet[0][i+1]==word);}
        CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&otherSend)==otherSend);
        writes=m->packetWrites;commits=m->packetCommits;m->expectStop=1;
        restored(m,l);
        CHECK(m->stopSeen && !m->protocolErrors && m->packetWrites==writes && m->packetCommits==commits);
        CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&otherSend)==otherSend);
    }
    // A live transmitter or timing generator forbids retiring an active AVI.
    // Refuse each admission bit with no writes; unrelated active slots are legal.
    for(j=0;j<2;++j)for(i=0;i<2;++i) {
        coldInit(m,l);m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=i?
            DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_SEND_MASK:
            DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_CONT_MASK;
        if(j)m->reg[BC250_REG_DMU_OTG0_OTG_CONTROL/4u]=OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
        else m->reg[R(DIG_BE_EN_CNTL)]=DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
        CHECK(coldProgram(m,l)!=0 && !m->writeOps && !l->Dirty);
        CHECK(l->StreamStage==2 && l->StreamFailure.Valid && l->StreamFailure.Operation==4);
        CHECK(l->StreamFailure.Register==(j?BC250_REG_DMU_OTG0_OTG_CONTROL/4u:R(DIG_BE_EN_CNTL)));
    }
    // A posted data error may have changed RAM. Do not commit a partial image;
    // keep control debt until stop completes, without trying to read the data.
    for(i=0;i<2;++i) {
        coldInit(m,l);m->packetProtocol=1;m->failPacketWrite=1;m->postFailure=i;
        CHECK(coldProgram(m,l)!=0 && l->PacketDirty && l->Dirty && !m->packetCommits);
        CHECK(l->StreamFailure.Operation==3 && !l->StreamFailure.ReadbackValid && !m->packetReads);
        m->failPacketWrite=0;restored(m,l);
    }
    printf("Packet lifecycle: %u checks, %u failures; stale reads, complete writes, inactive admission and stop\n",checks-begin,failures-bad);
}
static void restartPacketTests(MOCK*m,BC250_HDMI_LINK*l)
{
    unsigned begin=checks,bad=failures,i,j,operations,writes,commits;
    uint8_t header[4],payload[32];uint32_t word;
    const uint32_t active=DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_CONT_MASK|
        DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC0_SEND_MASK;
    const uint32_t otherSend=DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC1_CONT_MASK|
        DIG0_HDMI_GENERIC_PACKET_CONTROL0__HDMI_GENERIC1_SEND_MASK;
    const uint32_t line0=DIG0_HDMI_GENERIC_PACKET_CONTROL1__HDMI_GENERIC0_LINE_MASK;
    BC250_ATOM_CALLBACKS io={m,Owned,Allowed,Read,Write,Now,Delay};
    coldInit(m,l);m->packetProtocol=1;
    m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=otherSend;
    m->reg[R(HDMI_GENERIC_PACKET_CONTROL1)]=3u<<DIG0_HDMI_GENERIC_PACKET_CONTROL1__HDMI_GENERIC1_LINE__SHIFT;
    CHECK(coldProgram(m,l)==0);
    for(j=0;j<3;++j) {
        // Model PnP handoff: preserve hardware, discard only software, then
        // the new link owner stops TX/OTG before it programs a different mode.
        m->reg[R(DIG_BE_EN_CNTL)]=DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK|DIG0_DIG_BE_EN_CNTL__DIG_SYMCLK_BE_ON_MASK;
        m->reg[BC250_REG_DMU_OTG0_OTG_CONTROL/4u]=OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
        CHECK(Bc250HdmiStreamMute(l,0)==0);
        CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&active)==active);
        memset(l,0,sizeof(*l));
        m->reg[R(DIG_BE_EN_CNTL)]&=~DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
        m->reg[BC250_REG_DMU_OTG0_OTG_CONTROL/4u]&=~OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
        writes=m->packetWrites;commits=m->packetCommits;m->expectStop=1;m->stopSeen=0;
        CHECK(Bc250HdmiStreamProgram(l,&io,108000,1280,1024,1688,1066)==0);
        CHECK(m->stopSeen && !m->protocolErrors && !m->packetReads);
        CHECK(m->packetWrites==writes+9 && m->packetCommits==commits+1);
        Bc250HdmiLinkAvi(1280,1024,1688,1066,header,payload);
        for(i=0;i<8;++i) {memcpy(&word,payload+4*i,4);CHECK(m->packet[0][i+1]==word);}
        CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&otherSend)==otherSend);
        CHECK(!memcmp(&m->packet[1],&m->oldPacket[1],7*sizeof(m->packet[0])));
    }
    CHECK(Bc250HdmiLinkRestore(l)==0);
    CHECK(!(m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&active));
    CHECK(!(m->reg[R(HDMI_GENERIC_PACKET_CONTROL1)]&line0));
    CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&otherSend)==otherSend);
    CHECK(!m->unknownChanges && !m->commandReplays);
    // An access failure at any point, including a posted write, must not replay
    // active pre-restart controls. Recovery starts from a stopped packet and
    // rebuilds a complete AVI. All operations use the production entry points.
    coldInit(m,l);m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=active|otherSend;
    CHECK(coldProgram(m,l)==0);operations=m->ops;
    for(j=0;j<2;++j)for(i=1;i<=operations;++i) {
        coldInit(m,l);m->packetProtocol=1;m->postFailure=j;
        m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=active|otherSend;
        m->reg[R(HDMI_GENERIC_PACKET_CONTROL1)]=2u|(3u<<DIG0_HDMI_GENERIC_PACKET_CONTROL1__HDMI_GENERIC1_LINE__SHIFT);
        m->failOp=i;
        CHECK(coldProgram(m,l)!=0 && l->StreamFailure.Valid);
        m->failOp=0;
        if(l->Dirty) {
            CHECK(Bc250HdmiLinkRestore(l)==0);
            CHECK(!(m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&active));
        }
        CHECK(coldProgram(m,l)==0);
        CHECK(!m->protocolErrors && !m->packetReads && !m->unknownChanges && !m->commandReplays);
        CHECK((m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&otherSend)==otherSend);
        CHECK(!memcmp(&m->packet[1],&m->oldPacket[1],7*sizeof(m->packet[0])));
        CHECK(Bc250HdmiLinkRestore(l)==0);
        CHECK(!(m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]&active));
    }
    printf("Restart packet lifecycle: %u checks, %u failures; fresh context, retained AVI, stopped takeover and recovery\n",checks-begin,failures-bad);
}
static void streamFailureTests(MOCK*m,BC250_HDMI_LINK*l)
{
    unsigned i,ops,writes,before=checks,bad=failures;BC250_HDMI_RESTORE_DETAIL first;
    // All failure kinds retain their first evidence independently of cleanup.
    coldInit(m,l);CHECK(Bc250HdmiStreamProgram(l,NULL,174500,1920,1080,2080,1119)!=0);
    CHECK(l->StreamStage==1 && l->StreamFailure.Operation==7 && !l->StreamFailure.ReadbackValid);
    CHECK(!m->ops && !m->writeOps);
    coldInit(m,l);CHECK(coldProgram(m,l)==0);ops=m->ops;writes=m->writeOps;
    CHECK(coldProgram(m,l)!=0 && l->StreamStage==1 && l->StreamFailure.Operation==8);
    CHECK(l->StreamFailure.Readback==l->Dirty && m->ops==ops && m->writeOps==writes);restored(m,l);
    for(i=0;i<3;++i) {
        coldInit(m,l);
        if(i==0)m->reg[R(DIG_BE_CNTL)]&=~DIG0_DIG_BE_CNTL__DIG_MODE_MASK;
        if(i==1)m->reg[R(DIG_BE_CNTL)]&=~DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK;
        if(i==2)m->reg[BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL/4u]=DIG1_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
        CHECK(coldProgram(m,l)!=0 && l->StreamStage==2 && l->StreamFailure.Operation==4);
        CHECK(l->StreamFailure.ReadbackValid && ((l->StreamFailure.Readback^l->StreamFailure.Wanted)&l->StreamFailure.Mask));
        CHECK(!m->writeOps);
    }
    coldInit(m,l);m->packetProtocol=1;m->conflictRemaining=3;
    CHECK(coldProgram(m,l)==0 && !l->StreamFailure.Valid);
    CHECK(m->delays==3 && m->packetWrites==9 && !m->protocolErrors);restored(m,l);
    coldInit(m,l);m->packetProtocol=1;m->conflictForever=1;
    CHECK(coldProgram(m,l)!=0 && l->StreamStage==6 && l->StreamFailure.Operation==5);
    CHECK(l->StreamFailure.Register==R(AFMT_VBI_PACKET_CONTROL) && l->StreamFailure.ReadbackValid);
    CHECK(l->StreamFailure.Mask==DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_CONFLICT_MASK);
    CHECK(l->StreamFailure.Wanted==0 && (l->StreamFailure.Readback&l->StreamFailure.Mask));
    CHECK(m->delays==50 && !m->packetWrites && !m->packetCommits);
    first=l->StreamFailure;
    CHECK(coldProgram(m,l)!=0 && !memcmp(&first,&l->StreamFailure,sizeof(first)) && l->StreamStage==6);
    m->conflictForever=0;m->prepareStage=0;restored(m,l);
    CHECK(coldProgram(m,l)==0 && !l->StreamFailure.Valid);restored(m,l);
    // Callback errors remain failures even without a data readback contract.
    coldInit(m,l);m->packetProtocol=1;m->failPacketWrite=1;
    CHECK(coldProgram(m,l)!=0 && l->StreamStage==6 && l->StreamFailure.Operation==3);
    CHECK(l->StreamFailure.Register==R(AFMT_GENERIC_3) && l->StreamFailure.Mask==UINT32_MAX);
    CHECK(!l->StreamFailure.ReadbackValid && !m->packetCommits);
    m->failPacketWrite=0;restored(m,l);
    coldInit(m,l);CHECK(coldProgram(m,l)==0);
    CHECK(Bc250HdmiStreamMute(l,0)!=0 && l->StreamStage==7 && l->StreamFailure.Operation==4);
    CHECK(l->StreamFailure.Register==R(DIG_BE_EN_CNTL) && l->StreamFailure.ReadbackValid);restored(m,l);
    printf("Stream failure controls: %u checks, %u failures; admission, conflicts, payload and first error\n",checks-before,failures-bad);
}
static void w16(uint8_t*b,unsigned p,unsigned v) { b[p]=(uint8_t)v;b[p+1]=(uint8_t)(v>>8); }
static void synthetic(uint8_t*b)
{
    unsigned i,sum=0;
    memset(b,0,1024); w16(b,0,0xaa55); b[2]=2;
    w16(b,0x18,0x120);memcpy(b+0x120,"PCIR",4);w16(b,0x124,0x1002);w16(b,0x126,0x13fe);
    w16(b,0x12a,24);w16(b,0x130,2);w16(b,0x48,0x60);w16(b,0x60,0x24);memcpy(b+0x64,"ATOM",4);
    w16(b,0x7e,0x180);w16(b,0x80,0xa4);w16(b,0xa4,12);b[0xa6]=2;b[0xa7]=1;
    w16(b,0x180,4+77*2);b[0x182]=2;b[0x183]=1;
    w16(b,0x184+4*2,0x240);w16(b,0x240,7);b[0x242]=1;b[0x243]=5;b[0x246]=91;
    w16(b,0x184+76*2,0x250);w16(b,0x250,7);b[0x252]=1;b[0x253]=6;b[0x256]=91;
    for(i=0;i<1024;++i) sum+=b[i]; b[1023]=(uint8_t)(0u-sum);
}
static void w32(uint8_t*b,unsigned p,uint32_t v)
{ unsigned i;for(i=0;i<4;++i)b[p+i]=(uint8_t)(v>>(8*i)); }
static void policyRom(uint8_t* b,const uint32_t* regs,const uint32_t* values,unsigned count)
{
    unsigned i,p=0x266,sum=0;
    synthetic(b);memset(b+1024,0,1024);b[2]=4;w16(b,0x130,4);b[1023]=0;
    w16(b,0x184+76*2,0x220);w16(b,0x220,7);b[0x222]=1;b[0x223]=6;b[0x224]=b[0x225]=0;b[0x226]=91;
    w16(b,0x184+4*2,0x260);b[0x262]=1;b[0x263]=5;b[0x264]=b[0x265]=0;
    for(i=0;i<count;++i) {b[p++]=1;b[p++]=5;w16(b,p,regs[i]);p+=2;w32(b,p,values[i]);p+=4;}
    b[p++]=91;w16(b,0x260,p-0x260);
    for(i=0;i<2048;++i)sum+=b[i];b[2047]=(uint8_t)(0u-sum);
}
static int snapshot(MOCK* m,BC250_HDMI_SNAPSHOT* snap)
{ BC250_ATOM_CALLBACKS io={m,Owned,Allowed,Read,Write,Now,Delay};return Bc250HdmiLinkSnapshot(&io,snap); }
static int preflight(const BC250_HDMI_SNAPSHOT* snap,const BC250_ATOM_IMAGE* im,BC250_ATOM_PLAN* plan,void* w,void* shadow,BC250_HDMI_PREFLIGHT* out)
{ return Bc250HdmiLinkPreflight(snap,im,plan,w,Bc250AtomContextSize(),shadow,Bc250HdmiLinkShadowSize(),174500,1920,1080,2080,1119,out); }
static void measuredInit(MOCK* m)
{
    init(m);
    // Exactly these six startup values were observed in .42. Other mock
    // registers and packet contents remain synthetic, not a complete lab dump.
    m->reg[BC250_REG_DMU_DIO_MEM_PWR_CTRL/4u]=UINT32_C(0x6DB6D800);
    m->reg[R(DIG_FE_CNTL)]=UINT32_C(0x01000000);
    m->reg[R(HDMI_CONTROL)]=UINT32_C(0x00010011);
    m->reg[R(DIG_BE_CNTL)]=UINT32_C(0x00020100);
    m->reg[R(AFMT_CNTL)]=UINT32_C(0x00000100);
    m->reg[R(HDMI_GC)]=UINT32_C(0x00000004);
    memcpy(m->original,m->reg,sizeof(m->reg));
}
static void shadowTests(MOCK* m,BC250_HDMI_LINK* l,const BC250_ATOM_IMAGE* actual,BC250_ATOM_PLAN* plan,void* w,int real)
{
    BC250_HDMI_SNAPSHOT* snap=malloc(sizeof(*snap));
    BC250_HDMI_PREFLIGHT* out=malloc(sizeof(*out));
    void* shadow=malloc(Bc250HdmiLinkShadowSize());uint8_t* rom=malloc(2048);
    BC250_ATOM_IMAGE image;uint32_t regs[70],values[70];unsigned i,bit,ops,delays,n;
    const uint32_t atom_regs[]={R(TMDS_CTL_BITS),R(DIG_FE_CNTL),R(DIG_BE_CNTL),R(HDMI_CONTROL),R(HDMI_VBI_PACKET_CONTROL)};
    CHECK(snap && out && shadow && rom);if(!snap || !out || !shadow || !rom)exit(2);
    CHECK(Bc250HdmiLinkPolicyCount()==26 && !Bc250HdmiLinkPolicyAt(26));
    measuredInit(m);CHECK(snapshot(m,snap)==0 && snap->Complete && snap->Count==22);
    for(i=0,n=0;i<26;++i) {
        const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicyAt(i);
        if(!p->PacketData) {CHECK(snap->Entries[n].Register==p->Register && !snap->Entries[n].Status);++n;}
    }
    CHECK(n==17 && !m->packetReads);
    ops=m->ops;delays=m->delays;
    CHECK(preflight(snap,actual,plan,w,shadow,out)==0);
    CHECK(out->Complete && !out->Fatal && !out->Count && !out->Overflow && out->Writes>0);
    CHECK(m->ops==ops && !m->writeOps && m->delays==delays);
    printf("%s shadow six measured values: complete %u refusals %u events %u fatal %u simulated writes %u reads %u us %llu; hardware writes %u\n",
        real?"Private ROM":"Synthetic ROM",out->Complete,out->Count,out->Events,out->Fatal,out->Writes,out->Reads,
        (unsigned long long)out->ElapsedUs,m->writeOps);
    memset(l,0,sizeof(*l));CHECK(start(m,l,(BC250_ATOM_IMAGE*)actual,plan,w)==0);CHECK(!m->unknownChanges && !m->commandReplays);
    CHECK((m->reg[R(HDMI_GC)]&DIG0_HDMI_GC__HDMI_GC_AVMUTE_CONT_MASK)!=0);restored(m,l);
    // Snapshot failures preserve all per-register statuses and never run writes.
    for(i=1;i<=22;++i) {
        init(m);m->failOp=i;CHECK(snapshot(m,snap)!=0 && !snap->Complete && snap->Count==22);
        CHECK(snap->Entries[i-1].Status!=0 && m->ops==22 && !m->writeOps);
        CHECK(preflight(snap,actual,plan,w,shadow,out)!=0 && out->Fatal && !out->Writes);
    }
    init(m);CHECK(snapshot(m,snap)==0);--snap->Count;
    CHECK(preflight(snap,actual,plan,w,shadow,out)!=0 && out->Fatal && !out->Writes);
    CHECK(snapshot(m,snap)==0);snap->Entries[1].Register=snap->Entries[0].Register;
    CHECK(preflight(snap,actual,plan,w,shadow,out)!=0 && out->Fatal && !out->Writes);
    CHECK(snapshot(m,snap)==0);snap->Entries[0].Status=-1;
    CHECK(preflight(snap,actual,plan,w,shadow,out)!=0 && out->Fatal && !out->Writes);
    CHECK(snapshot(m,snap)==0);
    CHECK(Bc250HdmiLinkPreflight(snap,actual,plan,w,Bc250AtomContextSize(),shadow,Bc250HdmiLinkShadowSize()-1,
        174500,1920,1080,2080,1119,out)!=0 && out->Fatal);
    // A pass does not disable live revalidation. Change GC between the RMW read
    // and the checked write's read; the first hardware write must be refused.
    init(m);CHECK(snapshot(m,snap)==0);CHECK(preflight(snap,actual,plan,w,shadow,out)==0);
    memset(l,0,sizeof(*l));m->gcReads=0;m->flipGcRead=2;
    CHECK(start(m,l,(BC250_ATOM_IMAGE*)actual,plan,w)!=0 && !m->writeOps);
    CHECK(l->FailureOperation==6 && l->FailureRegister==R(HDMI_GC));
    CHECK(((l->FailureWanted^l->FailureReadback)&l->FailureMask)!=0);
    // Each ATOM-writable UNKNOWN bit is an independent rejected intent. The
    // interpreter must continue in shadow so a later refusal is also reported.
    for(n=0;n<sizeof(atom_regs)/sizeof(atom_regs[0]);++n)for(bit=0;bit<32;++bit) {
        const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(atom_regs[n]);uint32_t b=UINT32_C(1)<<bit;
        if(!(p->Unknown&b))continue;
        init(m);regs[0]=atom_regs[n];values[0]=m->reg[regs[0]]^b;
        policyRom(rom,regs,values,1);CHECK(Bc250AtomImageOpen(rom,2048,0x1002,0x13fe,&image)==0);
        CHECK(snapshot(m,snap)==0);ops=m->ops;
        CHECK(preflight(snap,&image,plan,w,shadow,out)!=0 && out->Complete && !out->Fatal);
        CHECK(out->Count>=1 && out->Refusals[0].Register==regs[0] && out->Refusals[0].Operation==6);
        CHECK(((out->Refusals[0].Wanted^out->Refusals[0].Before)&b)!=0 && !m->writeOps && m->ops==ops);
    }
    init(m);regs[0]=R(TMDS_CTL_BITS);values[0]=Bc250HdmiLinkPolicy(regs[0])->Unknown;
    regs[1]=R(HDMI_VBI_PACKET_CONTROL);values[1]=Bc250HdmiLinkPolicy(regs[1])->Unknown;
    policyRom(rom,regs,values,2);CHECK(Bc250AtomImageOpen(rom,2048,0x1002,0x13fe,&image)==0);
    CHECK(snapshot(m,snap)==0);CHECK(preflight(snap,&image,plan,w,shadow,out)!=0);
    CHECK(out->Complete && out->Count==2 && out->Refusals[0].Register==regs[0] && out->Refusals[1].Register==regs[1]);
    CHECK(!m->writeOps && !m->delays);
    for(i=0;i<70;++i) {regs[i]=R(HDMI_VBI_PACKET_CONTROL);values[i]=((i+1)&3u)<<8|((i+1)>>2)<<16;}
    policyRom(rom,regs,values,70);CHECK(Bc250AtomImageOpen(rom,2048,0x1002,0x13fe,&image)==0);
    init(m);CHECK(snapshot(m,snap)==0);CHECK(preflight(snap,&image,plan,w,shadow,out)!=0);
    CHECK(out->Fatal && !out->Complete && out->Overflow && out->Count==64 && out->Events==65 && !m->writeOps);
    for(i=0;i<8;++i) {regs[i]=R(HDMI_VBI_PACKET_CONTROL);values[i]=(i%2+1)<<8;}
    policyRom(rom,regs,values,8);CHECK(Bc250AtomImageOpen(rom,2048,0x1002,0x13fe,&image)==0);
    init(m);CHECK(snapshot(m,snap)==0);CHECK(preflight(snap,&image,plan,w,shadow,out)!=0);
    CHECK(out->Complete && out->Count==3 && out->Events==8 && !out->Overflow && !m->writeOps);
    // Opcode80 is ATOM's millisecond delay. It exceeds this takeover's shared
    // 2ms command delay budget and must refuse before any real delay callback.
    policyRom(rom,regs,values,0);w16(rom,0x260,9);rom[0x266]=80;rom[0x267]=5;rom[0x268]=91;
    { unsigned sum=0;rom[2047]=0;for(i=0;i<2048;++i)sum+=rom[i];rom[2047]=(uint8_t)(0u-sum); }
    CHECK(Bc250AtomImageOpen(rom,2048,0x1002,0x13fe,&image)==0);
    init(m);CHECK(snapshot(m,snap)==0);ops=m->ops;
    CHECK(preflight(snap,&image,plan,w,shadow,out)!=0 && out->Fatal && !out->Complete);
    CHECK(!m->delays && !m->writeOps && m->ops==ops && out->Atom.Status!=0 && out->ElapsedUs<100000);
    free(rom);free(shadow);free(out);free(snap);
}
int main(int argc,char**argv)
{
    MOCK*m=malloc(sizeof(*m));BC250_HDMI_LINK*l=calloc(1,sizeof(*l));
    BC250_ATOM_PLAN*plan=malloc(sizeof(*plan));void*w=malloc(Bc250AtomContextSize());
    uint8_t*rom=malloc(BC250_ATOM_MAX_IMAGE+1u);size_t length=1024;BC250_ATOM_IMAGE im;
    unsigned writes,ops,restoreOps,i,j;uint8_t header[4],payload[32];
    const uint8_t expected[14]={0x1c,0x1e,0x28,0x80,0,0,0,0,0x60,4,0,0,0x21,8};
    if (!m||!l||!plan||!w||!rom) return 2;
    synthetic(rom);
    if(argc>=2) { FILE*f=fopen(argv[1],"rb");if(!f)return 3;length=fread(rom,1,BC250_ATOM_MAX_IMAGE+1u,f);fclose(f); }
    CHECK(Bc250AtomImageOpen(rom,length,0x1002,0x13fe,&im)==0);
    Bc250HdmiLinkAvi(1920,1080,2080,1119,header,payload);
    CHECK(header[0]==0x82&&header[1]==2&&header[2]==13&&header[3]==0);
    CHECK(memcmp(payload,expected,sizeof(expected))==0);
    init(m); CHECK(start(m,l,&im,plan,w)==0); CHECK(l->Active);
    CHECK((m->reg[R(DIG_BE_CNTL)]&DIG0_DIG_BE_CNTL__DIG_MODE_MASK)==(3u<<DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT));
    writes=m->writeOps;ops=m->ops;
    CHECK(m->oldVerifierFailures!=0);
    printf("positive %u writes, %u I/O calls; ATOM status %u table%u offset%u register%x failure%x\n",writes,ops,l->Atom.Status,l->Atom.Table,l->Atom.Offset,l->Atom.Register,l->FailureRegister);
    if(argc>=2) CHECK(m->starts!=0); // private firmware must exercise the command
    if(argc==3 && !strcmp(argv[2],"--positive-only")) {
        printf("Positive-only: %u failures\n",failures);
        return failures?1:0;
    }
    // Real stop sequence: audio mute/disable precedes the display restore.
    m->reg[R(AFMT_CNTL)]&=~DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK;
    restored(m,l);
    restoreOps=m->ops-ops;
    // A failed stop is retried after WDDM drains. The first restore may already
    // have restored some controls, so retry must retain the original journal.
    for(j=0;j<2;++j) for(i=1;i<=restoreOps;++i) {
        init(m);memset(l,0,sizeof(*l));CHECK(start(m,l,&im,plan,w)==0);
        m->reg[R(AFMT_CNTL)]&=~DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK;
        m->failOp=m->ops+i;m->postFailure=j;
        CHECK(Bc250HdmiLinkRestore(l)!=0);CHECK(l->Dirty);
        restored(m,l);
    }
    for(j=0;j<2;++j) for(i=1;i<=writes;++i) {
        init(m);memset(l,0,sizeof(*l));m->failWrite=i;m->postFailure=j;
        CHECK(start(m,l,&im,plan,w)!=0);restored(m,l);
    }
    for(i=1;i<=ops;++i) {
        init(m);memset(l,0,sizeof(*l));m->failOp=i;
        CHECK(start(m,l,&im,plan,w)!=0);restored(m,l);
    }
    init(m);memset(l,0,sizeof(*l));m->reg[R(DIG_BE_EN_CNTL)]=0;
    CHECK(start(m,l,&im,plan,w)!=0);CHECK(!m->writeOps);
    init(m);memset(l,0,sizeof(*l));m->reg[R(DIG_BE_CNTL)]=0;
    CHECK(start(m,l,&im,plan,w)!=0);CHECK(!m->writeOps);
    init(m);memset(l,0,sizeof(*l));m->reg[R(HDMI_GENERIC_PACKET_CONTROL0)]=1;
    CHECK(start(m,l,&im,plan,w)!=0);CHECK(!m->writeOps);
    init(m);memset(l,0,sizeof(*l));m->corruptFe=1;
    CHECK(start(m,l,&im,plan,w)!=0);
    printf("FE corruption: register %x operation %u wanted %x readback %x mask %x\n",
        l->FailureRegister,l->FailureOperation,l->FailureWanted,l->FailureReadback,l->FailureMask);
    CHECK(l->FailureRegister==R(DIG_FE_CNTL)&&l->FailureOperation==4);
    CHECK((l->FailureWanted^l->FailureReadback)&l->FailureMask);
    m->corruptFe=0;restored(m,l);
    // An unchanged inherited UNKNOWN bit is preserved by ordinary RMW.
    init(m);memset(l,0,sizeof(*l));
    m->reg[R(HDMI_CONTROL)]|=DIG0_HDMI_CONTROL__HDMI_ERROR_ACK_MASK;
    m->original[R(HDMI_CONTROL)]=m->reg[R(HDMI_CONTROL)];
    if(argc>=2) {
        // This ROM intentionally replaces HDMI_CONTROL with a full word. That
        // changes ERROR_ACK, unlike the synthetic no-op ROM's later RMW path.
        CHECK(start(m,l,&im,plan,w)!=0);
        CHECK(l->FailureOperation==6 && l->FailureRegister==R(HDMI_CONTROL));
    } else CHECK(start(m,l,&im,plan,w)==0);
    CHECK(m->reg[R(HDMI_CONTROL)]&DIG0_HDMI_CONTROL__HDMI_ERROR_ACK_MASK);
    restored(m,l);
    init(m);memset(l,0,sizeof(*l));CHECK(start(m,l,&im,plan,w)==0);
    m->reg[R(AFMT_VBI_PACKET_CONTROL1)]|=DIG0_AFMT_VBI_PACKET_CONTROL1__AFMT_GENERIC0_FRAME_UPDATE_PENDING_MASK;
    m->original[R(AFMT_VBI_PACKET_CONTROL1)]=m->reg[R(AFMT_VBI_PACKET_CONTROL1)];
    CHECK(Bc250HdmiLinkRestore(l)==0);CHECK(!l->Dirty);
    CHECK(m->reg[R(AFMT_VBI_PACKET_CONTROL1)]&DIG0_AFMT_VBI_PACKET_CONTROL1__AFMT_GENERIC0_FRAME_UPDATE_PENDING_MASK);
    restored(m,l);
    coldTests(m,l);
    streamFailureTests(m,l);
    packetLifecycleTests(m,l);
    restartPacketTests(m,l);
    if(argc>=2)realAtomStreamTests(m,l,&im,plan,w);
    restoreProtocol(m,l,&im,plan,w);
    // Restore must use CURRENT unknown/reserved values, not values captured
    // before takeover. Sampled command/status bits never become replay writes.
    init(m);memset(l,0,sizeof(*l));CHECK(start(m,l,&im,plan,w)==0);
    for(i=0;i<l->Count;++i) {
        uint32_t r=l->Offset[i];const BC250_HDMI_FIELD_POLICY* p=Bc250HdmiLinkPolicy(r);
        uint32_t retained=~(p->Configuration|p->Command|p->Status);
        m->reg[r]^=retained;m->original[r]=(m->original[r]&~retained)|(m->reg[r]&retained);
    }
    m->reg[R(DIG_FE_CNTL)]|=DIG0_DIG_FE_CNTL__DIG_START_MASK;
    i=m->starts;restored(m,l);CHECK(m->starts==i && !m->unknownChanges && !m->commandReplays);
    shadowTests(m,l,&im,plan,w,argc>=2);
    policyModel(m);
    printf("HDMI link: %u checks, %u failures\n",checks,failures);
    free(rom);free(w);free(plan);free(l);free(m);return failures?1:0;
}
