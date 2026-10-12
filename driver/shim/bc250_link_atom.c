// PROVENANCE: AMD DC (MIT), command_table2.c and DCE112 command helpers, Linux 7d0a66e4.
#include "bc250_link_atom.h"
#include <string.h>
#include "atom_layouts.inc"
#include "link_command_layouts.inc"

#define ROOT(n) ((unsigned)(offsetof(struct atom_master_list_of_command_functions_v2_1,n)/2))
#define CLOCK_ROOT ROOT(setpixelclock)
#define TX_ROOT ROOT(dig1transmittercontrol)
#define STREAM_ROOT ROOT(digxencodercontrol)
#include "bc250_link_clock_scope.inc"

static int protocol(unsigned p) {return p==BC250_LINK_DP || p==BC250_LINK_DVI || p==BC250_LINK_HDMI;}
BC250_ATOM_STATUS Bc250LinkAtomReferenceClock(const BC250_ATOM_IMAGE* image,unsigned* khz)
{
    const unsigned index=(unsigned)(offsetof(struct atom_master_list_of_data_tables_v2_1,dce_info)/2);
    const size_t off=offsetof(struct atom_display_controller_info_v4_1,dce_refclk_10khz);
    BC250_ATOM_TABLE t;BC250_ATOM_STATUS s;uint16_t value;
    if(!khz)return BC250_ATOM_ARGUMENT;
    *khz=0;s=Bc250AtomGetTable(image,index,0,&t);if(s)return s;
    // bios_parser2 get_firmware_info_v3_2 uses this field in 10 kHz units.
    // Unlike its ASIC fallback, a missing clock is an explicit refusal here.
    if(t.Format!=4 || (t.Content!=1 && t.Content!=2))return BC250_ATOM_VERSION;
    if(t.Length<(t.Content==1?sizeof(struct atom_display_controller_info_v4_1):
        sizeof(struct atom_display_controller_info_v4_2)))return BC250_ATOM_BOUNDS;
    memcpy(&value,image->Bytes+t.Offset+off,sizeof(value));
    if(!value)return BC250_ATOM_PARAMETER;
    *khz=(unsigned)value*10u;return BC250_ATOM_OK;
}
static int valid(const BC250_LINK_ATOM* l)
{
    return l && l->Image && l->Plan && l->Workspace && l->WorkspaceSize>=Bc250AtomContextSize() &&
        l->Io.Owned && l->Io.Allowed && l->Io.Read && l->Io.Write && l->Io.NowUs && l->Io.DelayUs;
}
BC250_ATOM_STATUS Bc250LinkAtomValidate(BC250_LINK_ATOM* l)
{
    const unsigned roots[]={CLOCK_ROOT,TX_ROOT,STREAM_ROOT},versions[]={7,6,5};
    BC250_ATOM_TABLE table;BC250_ATOM_STATUS s;unsigned i;
    if(!valid(l))return BC250_ATOM_ARGUMENT;
    l->Ready=0;
    for(i=0;i<3;++i) {
        s=Bc250AtomGetTable(l->Image,roots[i],1,&table);
        if(s)return s;
        if(table.Format!=1 || table.Content!=versions[i])return BC250_ATOM_VERSION;
        s=Bc250AtomPreflight(l->Image,roots[i],l->Plan);if(s)return s;
    }
    l->Ready=1;return BC250_ATOM_OK;
}
static BC250_ATOM_STATUS execute(BC250_LINK_ATOM* l,unsigned root,void* p,size_t n)
{
    // The outer link transition owns a separate wall deadline across commands,
    // training and recovery. This inner bound is shared by each complete call tree.
    const BC250_ATOM_LIMITS limits={16384,100000,200000};BC250_ATOM_STATUS s;
    CLOCK_SCOPE scope={0};BC250_ATOM_CALLBACKS io;
    if(!valid(l) || !l->Ready)return BC250_ATOM_ARGUMENT;
    if(l->ClockScope)return BC250_ATOM_ARGUMENT;
    memset(&l->Last,0,sizeof(l->Last));
    s=Bc250AtomPreflight(l->Image,root,l->Plan);
    if(s) {l->Last.Status=s;l->Last.Table=root;return s;}
    ++l->Commands;
    io=l->Io;
    if(root==CLOCK_ROOT) {
        scope.Link=l;l->ClockScope=1;
        io.Context=&scope;io.Owned=scope_owned;io.Allowed=scope_allowed;
        io.Read=scope_read;io.Write=scope_write;io.NowUs=scope_now;io.DelayUs=scope_delay;
    }
    s=Bc250AtomExecute(l->Workspace,l->WorkspaceSize,l->Image,root,l->Plan,&io,&limits,p,n,&l->Last);
    l->ClockScope=0;
    return s;
}
BC250_ATOM_STATUS Bc250LinkAtomClock(BC250_LINK_ATOM* l,unsigned p,unsigned khz)
{
    struct set_pixel_clock_parameter_v1_7 c={0};
    if(!protocol(p) || khz<25000 || khz>300000)return BC250_ATOM_ARGUMENT;
    c.pixclk_100hz=khz*10u;c.crtc_id=0;
    c.pll_id=(uint8_t)(p==BC250_LINK_DP?ATOM_DP_DTO:ATOM_COMBOPHY_PLL0);
    // command_table_helper: ENCODER_ID_INTERNAL_UNIPHY (object 0x1e).
    // This is an ATOM object identifier, not a register or a PHY ordinal.
    c.encoderobjid=0x1e;c.encoder_mode=(uint8_t)p;
    // dce112_program_pix_clk supplies reference flags only for a real PLL;
    // DP_DTO uses zero misc flags (dce_clock_source.c, Linux 7d0a66e4).
    c.miscinfo=(uint8_t)(p==BC250_LINK_DP?0:PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN);
    return execute(l,CLOCK_ROOT,&c,sizeof(c));
}
BC250_ATOM_STATUS Bc250LinkAtomTransmitter(BC250_LINK_ATOM* l,enum bc250_link_tx_action action,
    unsigned p,unsigned khz,unsigned lanes,unsigned swing,unsigned pre)
{
    struct dig_transmitter_control_ps_allocation_v1_6 t={0};
    if(!protocol(p) || (lanes!=1 && lanes!=2 && lanes!=4) ||
       (p==BC250_LINK_DP?(khz!=162000 && khz!=270000):(khz<25000 || khz>300000 || lanes!=4)) ||
       (action!=BC250_LINK_TX_OFF && action!=BC250_LINK_TX_ON && action!=BC250_LINK_TX_DRIVE) ||
       swing>3 || pre>3 || swing+pre>3 ||
       (action==BC250_LINK_TX_DRIVE && p!=BC250_LINK_DP))return BC250_ATOM_ARGUMENT;
    t.param.phyid=0;t.param.lanenum=(uint8_t)lanes;t.param.hpdsel=1;
    // Physical connector remains DP when a passive DP++ adapter is attached.
    t.param.connobj_id=CONNECTOR_ID_DISPLAY_PORT;
    // DCE112 returns zero for digfe_sel; DC connects FE0 to BE0 itself.
    t.param.symclk_10khz=khz/10;
    t.param.action=action==BC250_LINK_TX_OFF?ATOM_TRANSMITTER_ACTION_DISABLE:
        action==BC250_LINK_TX_ON?ATOM_TRANSMITTER_ACTION_ENABLE:ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH;
    if(action==BC250_LINK_TX_DRIVE) {
        // dpcd_training_lane layout from AMD dc_dp_types.h (not DPCD max-level flags).
        t.param.mode_laneset.dplaneset=(uint8_t)(swing|(pre<<3));
    } else t.param.mode_laneset.digmode=(uint8_t)p;
    return execute(l,TX_ROOT,&t,sizeof(t));
}
BC250_ATOM_STATUS Bc250LinkAtomStream(BC250_LINK_ATOM* l,unsigned p,unsigned khz,unsigned lanes,unsigned rate)
{
    struct dig_encoder_stream_setup_parameters_v1_5 e={0};
    if(!protocol(p) || khz<25000 || khz>300000 || (lanes!=1 && lanes!=2 && lanes!=4) ||
       (p==BC250_LINK_DP?(rate!=6 && rate!=10):(rate!=0 || lanes!=4)))return BC250_ATOM_ARGUMENT;
    e.digid=0;e.action=ATOM_ENCODER_CMD_STREAM_SETUP;e.digmode=(uint8_t)p;
    e.pclk_10khz=khz/10;e.lanenum=(uint8_t)lanes;e.bitpercolor=PANEL_8BIT_PER_COLOR;
    e.dplinkrate_270mhz=(uint8_t)rate;
    return execute(l,STREAM_ROOT,&e,sizeof(e));
}
