/* Host-only parameter contract tests. No firmware image or hardware access.
 * Oracle: AMD Linux 7d0a66e4 command_table2.c, atomfirmware.h, grph_object_id.h (MIT).
 */
#include <stdio.h>
#include <string.h>
#include "../../shim/bc250_link_atom.h"
#include "../../shim/atom_layouts.inc"
#include "../../shim/link_command_layouts.inc"
#include "../../shim/link_clock_ports.inc"

static unsigned failures, checks, gets, pres, runs, selected, badroot;
static BC250_ATOM_STATUS get_status, pre_status, run_status;
static unsigned badformat, badcontent, scope_test, transport_writes, transport_fail;
static uint32_t transport_value;
static unsigned char captured[64], workspace[64];
static size_t captured_size;
static BC250_ATOM_IMAGE image;
static BC250_ATOM_PLAN plan;
static BC250_LINK_ATOM link;
static BC250_ATOM_TABLE data_table;
static BC250_ATOM_STATUS data_status;
static unsigned char data_bytes[512];
static void check(int yes,unsigned line,const char* expression)
{ ++checks;if(!yes) {++failures;printf("FAIL line %u: %s\n",line,expression);} }
#define CHECK(x) check(!!(x),(unsigned)__LINE__,#x)
static int owned(void* p) { (void)p; return 1; }
static int allowed(void* p,uint32_t r,int w) { (void)p;(void)r;(void)w;return 1; }
static int readreg(void* p,uint32_t r,uint32_t* v) { (void)p;(void)r;*v=transport_value;return (int)transport_fail; }
static int writereg(void* p,uint32_t r,uint32_t v) { (void)p;(void)r;(void)v;++transport_writes;return (int)transport_fail; }
static uint64_t now(void* p) { (void)p;return 0; }
static int delay(void* p,uint32_t u) { (void)p;(void)u;return 1; }
size_t Bc250AtomContextSize(void) { return sizeof(workspace); }
BC250_ATOM_STATUS Bc250AtomGetTable(const BC250_ATOM_IMAGE* im,unsigned root,int cmd,BC250_ATOM_TABLE* out)
{
    ++gets;
    if(!cmd) {
        CHECK(root==offsetof(struct atom_master_list_of_data_tables_v2_1,dce_info)/2);
        if(!im)return BC250_ATOM_ARGUMENT;
        CHECK(im==&image);*out=data_table;return data_status;
    }
    CHECK(im==&image && cmd==1);CHECK(root==12 || root==76 || root==4);
    memset(out,0,sizeof(*out));out->Format=1;out->Content=(uint8_t)(root==12?7:root==76?6:5);
    if(root==badroot) {out->Format=(uint8_t)badformat;out->Content=(uint8_t)badcontent;return get_status;}
    return BC250_ATOM_OK;
}
BC250_ATOM_STATUS Bc250AtomPreflight(const BC250_ATOM_IMAGE* im,unsigned root,BC250_ATOM_PLAN* p)
{ ++pres;CHECK(im==&image && p==&plan);return root==badroot?pre_status:BC250_ATOM_OK; }
BC250_ATOM_STATUS Bc250AtomExecute(void* w,size_t n,const BC250_ATOM_IMAGE* im,unsigned root,
    const BC250_ATOM_PLAN* p,const BC250_ATOM_CALLBACKS* io,const BC250_ATOM_LIMITS* limits,
    void* params,size_t bytes,BC250_ATOM_RESULT* result)
{
    /* SetPixelClock and DIG1TransmitterControl ENABLE may reach the PHY power-up table (BD-195). */
    const int scoped=root==12 || (root==76 && bytes==sizeof(struct dig_transmitter_control_ps_allocation_v1_6) &&
        ((const struct dig_transmitter_control_ps_allocation_v1_6*)params)->param.action==ATOM_TRANSMITTER_ACTION_ENABLE);
    ++runs;selected=root;CHECK(w==workspace && n==sizeof(workspace));CHECK(im==&image && p==&plan);
    CHECK(scoped ? io->Context!=link.Io.Context : io->Context==link.Io.Context);
    CHECK(Bc250LinkAtomScopedRegister(&link,CLOCK_CR_ADDR,1)==scoped);
    if(scope_test==2 && root==76 && scoped) {
        uint32_t value;unsigned before=transport_writes;
        CHECK(io->Write(io->Context,CLOCK_PSP34,0x6978)!=0);    // mailbox not seen ready
        transport_value=0x80000000;CHECK(io->Read(io->Context,CLOCK_PSP33,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0)!=0);         // SetPixelClock's argument, not this call's
        CHECK(io->Write(io->Context,CLOCK_PSP34,0x3f48)!=0);    // another link rate
        CHECK(transport_writes==before);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0x6978)==0);    // symclk_10khz of 270000 kHz
        CHECK(io->Write(io->Context,CLOCK_PSP33,0x50000)==0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0x6978)!=0);    // one message per call tree
        transport_value=0x0040f000;CHECK(io->Read(io->Context,CLOCK_CNTL,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_CNTL,0x0041f000)==0); // RDPCS_TX_FIFO_EN
        CHECK(io->Write(io->Context,CLOCK_CNTL,0x0042f000)!=0); // RDPCS_TX_FIFO_START is not in it
    }
    if(scope_test && root==12) {
        uint32_t value;unsigned before;
        before=transport_writes;
        CHECK(Bc250LinkAtomClock(&link,BC250_LINK_HDMI,148500)==BC250_ATOM_ARGUMENT);
        CHECK(link.ClockScope && transport_writes==before);
        CHECK(io->Write(io->Context,CLOCK_CR_DATA,0)!=0);
        CHECK(io->Write(io->Context,CLOCK_CR_ADDR,0xdead)!=0);
        CHECK(io->Write(io->Context,CLOCK_CR_ADDR,0x3007)==0);
        CHECK(io->Write(io->Context,CLOCK_CR_DATA,0x2546)!=0);
        transport_value=0x8000a000;CHECK(io->Read(io->Context,CLOCK_CR_DATA,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_CR_DATA,0x8000a546)==0);
        before=transport_writes;CHECK(io->Write(io->Context,CLOCK_CR_DATA,0x2546)!=0);
        CHECK(transport_writes==before);
        CHECK(io->Write(io->Context,CLOCK_PSP33,0x50000)!=0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0)!=0);
        transport_value=0x80000000;CHECK(io->Read(io->Context,CLOCK_PSP33,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,1)!=0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0)==0);
        CHECK(io->Write(io->Context,CLOCK_PSP33,0x60000)!=0);
        CHECK(io->Write(io->Context,CLOCK_PSP33,0x50000)==0);
        CHECK(io->Write(io->Context,CLOCK_PSP33,0x50000)!=0);
        CHECK(io->Write(io->Context,CLOCK_PSP34,0)!=0);
        transport_value=0x80000010;CHECK(io->Read(io->Context,CLOCK_PHY2,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_PHY2,0x80000018)==0);
        CHECK(io->Write(io->Context,CLOCK_PHY2,0x8)!=0);
        transport_value=0x80000010;CHECK(io->Read(io->Context,CLOCK_PLL_UPDATE,&value)==0);
        CHECK(io->Write(io->Context,CLOCK_PLL_UPDATE,0x80000011)==0);
        CHECK(io->Write(io->Context,CLOCK_PLL_UPDATE,1)!=0);
        transport_fail=1;CHECK(io->Write(io->Context,CLOCK_CR_ADDR,0xc000)!=0);
        transport_fail=0;CHECK(io->Write(io->Context,CLOCK_CR_DATA,0)!=0);
    }CHECK(limits->Instructions==16384 && limits->DelayUs==100000 && limits->ElapsedUs==200000);
    CHECK(bytes<=sizeof(captured));memset(captured,0xcc,sizeof(captured));memcpy(captured,params,bytes);captured_size=bytes;
    CHECK(result->Status==0 && result->Writes==0);result->Status=run_status;result->Table=root;return run_status;
}
static void reset(void)
{
    memset(&link,0,sizeof(link));link.Image=&image;link.Plan=&plan;link.Workspace=workspace;link.WorkspaceSize=sizeof(workspace);
    link.Io.Owned=owned;link.Io.Allowed=allowed;link.Io.Read=readreg;link.Io.Write=writereg;link.Io.NowUs=now;link.Io.DelayUs=delay;
    gets=pres=runs=0;badroot=999;get_status=pre_status=run_status=BC250_ATOM_OK;badformat=1;badcontent=0;
}
static void ready(void) { reset();CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_OK);CHECK(link.Ready && gets==3 && pres==3 && !runs); }
static void validation(void)
{
    unsigned i;
    const unsigned roots[]={12,76,4};
    CHECK(offsetof(struct atom_master_list_of_command_functions_v2_1,setpixelclock)/2==12);
    CHECK(offsetof(struct atom_master_list_of_command_functions_v2_1,dig1transmittercontrol)/2==76);
    CHECK(offsetof(struct atom_master_list_of_command_functions_v2_1,digxencodercontrol)/2==4);
    CHECK(sizeof(struct set_pixel_clock_parameter_v1_7)==16);
    CHECK(sizeof(struct dig_transmitter_control_ps_allocation_v1_6)==32);
    CHECK(sizeof(struct dig_encoder_stream_setup_parameters_v1_5)==12);
    CHECK(Bc250LinkAtomValidate(NULL)==BC250_ATOM_ARGUMENT);
    for(i=0;i<3;++i) {
        reset();link.Ready=1;badroot=roots[i];badformat=2;
        CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_VERSION);CHECK(!link.Ready && !runs);
        reset();badroot=roots[i];CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_VERSION);
        reset();badroot=roots[i];get_status=BC250_ATOM_NOT_FOUND;
        CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_NOT_FOUND);CHECK(!link.Ready && !runs);
        reset();badroot=roots[i];badcontent=i==0?7:i==1?6:5;pre_status=BC250_ATOM_OPCODE;
        CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_OPCODE);CHECK(!link.Ready && !runs);
    }
    reset();link.WorkspaceSize--;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.Write=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();CHECK(Bc250LinkAtomClock(&link,BC250_LINK_DP,148500)==BC250_ATOM_ARGUMENT);CHECK(!runs);
}
static void marshaling(void)
{
    const unsigned modes[]={BC250_LINK_DP,BC250_LINK_DVI,BC250_LINK_HDMI};unsigned i,s,p;
    for(i=0;i<3;++i) {
        struct set_pixel_clock_parameter_v1_7 c={0};
        struct dig_transmitter_control_ps_allocation_v1_6 t={0};
        struct dig_encoder_stream_setup_parameters_v1_5 e={0};
        ready();c.pixclk_100hz=1485000;c.pll_id=(uint8_t)(i==0?ATOM_DP_DTO:ATOM_COMBOPHY_PLL0);
        c.encoderobjid=0x1e;c.encoder_mode=(uint8_t)modes[i];
        c.miscinfo=(uint8_t)(i==0?0:PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN);
        CHECK(Bc250LinkAtomClock(&link,modes[i],148500)==BC250_ATOM_OK);
        CHECK(selected==12 && captured_size==sizeof(c) && !memcmp(captured,&c,sizeof(c)));
        t.param.action=ATOM_TRANSMITTER_ACTION_ENABLE;t.param.mode_laneset.digmode=(uint8_t)modes[i];
        t.param.lanenum=4;t.param.symclk_10khz=i==0?27000:14850;t.param.hpdsel=1;
        /* Physical connector stays DP for a passive DP++ adapter, AMD grph_object_id.h. */
        t.param.connobj_id=19;
        CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,modes[i],i==0?270000:148500,4,0,0)==BC250_ATOM_OK);
        CHECK(selected==76 && captured_size==sizeof(t) && !memcmp(captured,&t,sizeof(t)));
        t.param.action=ATOM_TRANSMITTER_ACTION_DISABLE;
        CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_OFF,modes[i],i==0?270000:148500,4,0,0)==BC250_ATOM_OK);
        CHECK(!memcmp(captured,&t,sizeof(t)));
        e.action=ATOM_ENCODER_CMD_STREAM_SETUP;e.digmode=(uint8_t)modes[i];e.lanenum=4;e.pclk_10khz=14850;
        e.bitpercolor=2; /* PANEL_8BIT_PER_COLOR, AMD atomfirmware.h:148. */
        e.dplinkrate_270mhz=(uint8_t)(i==0?10:0);
        CHECK(Bc250LinkAtomStream(&link,modes[i],148500,4,i==0?10:0)==BC250_ATOM_OK);
        CHECK(selected==4 && captured_size==sizeof(e) && !memcmp(captured,&e,sizeof(e)));
    }
    for(s=0;s<=3;++s)for(p=0;p+s<=3;++p) {
        struct dig_transmitter_control_ps_allocation_v1_6 t={0};ready();
        t.param.action=ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH;t.param.mode_laneset.dplaneset=(uint8_t)(s|(p<<3));
        t.param.lanenum=2;t.param.symclk_10khz=16200;t.param.hpdsel=1;t.param.connobj_id=19;
        CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_DRIVE,BC250_LINK_DP,162000,2,s,p)==BC250_ATOM_OK);
        CHECK(captured_size==sizeof(t) && !memcmp(captured,&t,sizeof(t)));
    }
}
static void failures_test(void)
{
    unsigned before;ready();before=runs;
    CHECK(Bc250LinkAtomClock(&link,1,148500)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomClock(&link,0,24999)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomClock(&link,0,300001)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_DRIVE,3,148500,4,0,0)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,0,540000,4,0,0)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,0,270000,3,0,0)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_DRIVE,0,270000,4,3,1)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomTransmitter(&link,(enum bc250_link_tx_action)99,0,270000,4,0,0)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomStream(&link,3,148500,2,0)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomStream(&link,0,148500,4,20)==BC250_ATOM_ARGUMENT);
    CHECK(Bc250LinkAtomStream(&link,3,148500,4,6)==BC250_ATOM_ARGUMENT);CHECK(runs==before);
    badroot=12;pre_status=BC250_ATOM_OPCODE;
    CHECK(Bc250LinkAtomClock(&link,0,148500)==BC250_ATOM_OPCODE);
    CHECK(runs==before && link.Commands==0 && link.Last.Table==12 && link.Last.Status==BC250_ATOM_OPCODE);
    pre_status=BC250_ATOM_OK;run_status=BC250_ATOM_IO;link.Last.Writes=99;
    CHECK(Bc250LinkAtomClock(&link,0,148500)==BC250_ATOM_IO);
    CHECK(runs==before+1 && link.Commands==1 && link.Last.Status==BC250_ATOM_IO);
}
static void scope_contract(void)
{
    ready();scope_test=1;
    CHECK(!Bc250LinkAtomScopedRegister(&link,CLOCK_CR_ADDR,1));
    CHECK(Bc250LinkAtomClock(&link,BC250_LINK_HDMI,148500)==BC250_ATOM_OK);
    CHECK(!link.ClockScope);
    /* Repeat proves state is not retained after successful or failed commands. */
    run_status=BC250_ATOM_IO;
    CHECK(Bc250LinkAtomClock(&link,BC250_LINK_HDMI,174500)==BC250_ATOM_IO);
    CHECK(!link.ClockScope);
    run_status=BC250_ATOM_BUDGET;
    CHECK(Bc250LinkAtomClock(&link,BC250_LINK_HDMI,74250)==BC250_ATOM_BUDGET);
    CHECK(!link.ClockScope);scope_test=0;
    CHECK(!Bc250LinkAtomScopedRegister(&link,CLOCK_PSP33,1));
    CHECK(Bc250LinkAtomStream(&link,BC250_LINK_HDMI,148500,4,0)==BC250_ATOM_BUDGET);
    CHECK(!link.ClockScope);
    /* BD-195: DP transmitter enable after a re-plug runs the same PHY power-up. */
    run_status=BC250_ATOM_OK;scope_test=2;
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_ON,BC250_LINK_DP,270000,4,0,0)==BC250_ATOM_OK);
    CHECK(!link.ClockScope);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_OFF,BC250_LINK_DP,270000,4,0,0)==BC250_ATOM_OK);
    CHECK(Bc250LinkAtomTransmitter(&link,BC250_LINK_TX_DRIVE,BC250_LINK_DP,270000,4,1,1)==BC250_ATOM_OK);
    CHECK(!link.ClockScope);scope_test=0;
}
static void boundaries(void)
{
    unsigned lane,rate;ready();
    CHECK(Bc250LinkAtomClock(&link,BC250_LINK_HDMI,25000)==BC250_ATOM_OK);
    CHECK(Bc250LinkAtomClock(&link,BC250_LINK_DVI,300000)==BC250_ATOM_OK);
    for(lane=1;lane<=4;lane*=2)for(rate=6;rate<=10;rate+=4) {
        struct dig_encoder_stream_setup_parameters_v1_5 e={0};
        e.action=ATOM_ENCODER_CMD_STREAM_SETUP;e.lanenum=(uint8_t)lane;e.pclk_10khz=2500;
        e.bitpercolor=2;e.dplinkrate_270mhz=(uint8_t)rate;
        CHECK(Bc250LinkAtomStream(&link,BC250_LINK_DP,25000,lane,rate)==BC250_ATOM_OK);
        CHECK(captured_size==sizeof(e) && !memcmp(captured,&e,sizeof(e)));
    }
    reset();link.Image=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Plan=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Workspace=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.Owned=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.Allowed=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.Read=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.NowUs=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
    reset();link.Io.DelayUs=NULL;CHECK(Bc250LinkAtomValidate(&link)==BC250_ATOM_ARGUMENT);
}
static void reference_clock(void)
{
    const size_t off=offsetof(struct atom_display_controller_info_v4_1,dce_refclk_10khz);
    unsigned khz,i,before;
    CHECK(off==offsetof(struct atom_display_controller_info_v4_2,dce_refclk_10khz));
    memset(data_bytes,0,sizeof(data_bytes));image.Bytes=data_bytes;
    data_status=BC250_ATOM_OK;memset(&data_table,0,sizeof(data_table));
    data_table.Format=4;data_table.Offset=1; // deliberately unaligned field load
    for(i=1;i<=2;++i) {
        unsigned value;
        data_table.Content=(uint8_t)i;
        data_table.Length=(uint16_t)(i==1?sizeof(struct atom_display_controller_info_v4_1):
            sizeof(struct atom_display_controller_info_v4_2));
        for(value=1;value<=65535;value=value==1?2700:value==2700?65535:65536) {
            data_bytes[1+off]=(uint8_t)value;data_bytes[2+off]=(uint8_t)(value>>8);
            khz=0xdead;CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_OK);
            CHECK(khz==value*10u);
        }
        --data_table.Length;khz=99;
        CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_BOUNDS);CHECK(khz==0);
        ++data_table.Length;data_bytes[1+off]=data_bytes[2+off]=0;khz=99;
        CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_PARAMETER);CHECK(khz==0);
    }
    data_table.Format=3;khz=99;
    CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_VERSION);CHECK(khz==0);
    data_table.Format=4;data_table.Content=3;
    CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_VERSION);
    data_status=BC250_ATOM_BOUNDS;khz=99;
    CHECK(Bc250LinkAtomReferenceClock(&image,&khz)==BC250_ATOM_BOUNDS);CHECK(khz==0);
    before=gets;CHECK(Bc250LinkAtomReferenceClock(&image,NULL)==BC250_ATOM_ARGUMENT);CHECK(gets==before);
    khz=99;CHECK(Bc250LinkAtomReferenceClock(NULL,&khz)==BC250_ATOM_ARGUMENT);CHECK(khz==0);
}
int main(void) {scope_contract();validation();marshaling();failures_test();boundaries();reference_clock();printf("link ATOM: %u checks, %u failures\n",checks,failures);return failures?1:0;}
