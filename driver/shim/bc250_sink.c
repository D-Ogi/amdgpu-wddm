#include "bc250_sink.h"
#include "bc250_dp_train.h"
#include "../kmd/dpaux.h"
#include <string.h>

typedef struct GUARDED_IO {
    BC250_DISP_IO* Raw;
    const BC250_SINK_GUARD* Guard;
    int Cleanup;
} GUARDED_IO;
static long readIo(void* context,unsigned long reg,unsigned long* value)
{
    GUARDED_IO* g=context;
    if(!g->Cleanup && !g->Guard->Current(g->Guard->Context))return BC250_DISP_STATUS_BUSY;
    return g->Raw->Read(g->Raw->Context,reg,value);
}
static long writeIo(void* context,unsigned long reg,unsigned long value)
{
    GUARDED_IO* g=context;
    if(!g->Cleanup && !g->Guard->Current(g->Guard->Context))return BC250_DISP_STATUS_BUSY;
    return g->Raw->Write(g->Raw->Context,reg,value);
}
static void stallIo(void* context,unsigned long us)
{
    GUARDED_IO* g=context;
    g->Raw->Stall(g->Raw->Context,us);
}

long Bc250SinkDiscover(BC250_DISP_IO* raw,const BC250_SINK_GUARD* guard,
    unsigned long maxClock,unsigned long maxWidth,unsigned long maxHeight,BC250_SINK* r)
{
    BC250_AUX_SESSION aux={0};BC250_DDC_SESSION ddc={0};BC250_DISP_IO io;
    GUARDED_IO g={raw,guard,0};long status,close;unsigned long reason;
    if(!r)return BC250_DISP_STATUS_INVALID;
    memset(r,0,sizeof(*r));r->Status=BC250_DISP_STATUS_INVALID;
    if(!raw || !guard || !guard->Current || !raw->Read || !raw->Write || !raw->Stall ||
       !maxClock || !maxWidth || !maxHeight)return r->Status;
    io=*raw;io.Context=&g;io.Read=readIo;io.Write=writeIo;io.Stall=stallIo;
    r->AuxStatus=r->DdcStatus=BC250_DISP_STATUS_NOT_SUPPORTED;
    if(!guard->Current(guard->Context)) {status=BC250_DISP_STATUS_BUSY;goto done;}
    r->Phase=BC250_SINK_PHASE_AUX_OPEN;
    status=Bc250AuxOpenForDetection(&aux,&io);
    if(status>=0) {
        r->Phase=BC250_SINK_PHASE_DPCD;
        status=Bc250AuxDpcdRead(&aux,0,r->Dpcd,sizeof(r->Dpcd));
        if(status>=0) {
            // DPCD success fixes the protocol. An unsupported DP receiver is
            // not reclassified as TMDS just because its mode or EDID fails.
            r->Kind=BC250_SINK_DP;
            r->MaxPixelClockKhz=Bc250DpMaxPixelClock(r->Dpcd);
            if(!r->MaxPixelClockKhz) status=BC250_DISP_STATUS_NOT_SUPPORTED;
            else {
                r->Phase=BC250_SINK_PHASE_AUX_EDID;
                status=Bc250AuxReadEdid(&aux,r->Edid,sizeof(r->Edid),&r->EdidLength);
            }
        }
    }
    g.Cleanup=1;close=Bc250AuxClose(&aux);g.Cleanup=0;
    r->AuxStatus=status;
    if(close<0) {r->CleanupStatus=close;status=close;goto done;}
    if(!guard->Current(guard->Context)) {status=BC250_DISP_STATUS_BUSY;goto done;}
    if(r->Kind==BC250_SINK_DP) {if(status<0)goto done;}
    else {
        // A failed AUX packet is insufficient evidence of a passive adapter.
        // Require its positive ID before choosing TMDS or writing a link mode.
        r->Phase=BC250_SINK_PHASE_DDC_OPEN;
        status=bc250_ddc_open(&ddc,&io);
        if(status>=0) {
            r->Phase=BC250_SINK_PHASE_ADAPTER;
            status=bc250_ddc_read_adapter(&ddc,&r->Adapter);
            if(status>=0 && (!r->Adapter.type || !r->Adapter.max_tmds_khz)) status=BC250_DISP_STATUS_NOT_SUPPORTED;
            if(status>=0) {
                r->MaxPixelClockKhz=r->Adapter.max_tmds_khz;
                r->Phase=BC250_SINK_PHASE_DDC_EDID;
                status=bc250_ddc_read_edid(&ddc,r->Edid,sizeof(r->Edid),&r->EdidLength);
            }
        }
        g.Cleanup=1;close=bc250_ddc_close(&ddc);g.Cleanup=0;
        r->DdcStatus=status;
        if(close<0) {r->CleanupStatus=close;status=close;goto done;}
        if(status<0)goto done;
    }
    if(!guard->Current(guard->Context)) {status=BC250_DISP_STATUS_BUSY;goto done;}
    r->Phase=BC250_SINK_PHASE_PARSE;
    reason=Bc250EdidParse(r->Edid,r->EdidLength,&r->Info);
    if(reason!=BC250_EDID_OK && reason!=BC250_EDID_EXTENSION_DROPPED) {
        status=BC250_DISP_STATUS_MISMATCH;goto done;
    }
    if(r->Kind!=BC250_SINK_DP)r->Kind=r->Info.Hdmi?BC250_SINK_HDMI:BC250_SINK_DVI;
    if(maxClock<r->MaxPixelClockKhz)r->MaxPixelClockKhz=maxClock;
    r->TargetCount=Bc250EdidTargetList(&r->Info,r->MaxPixelClockKhz,maxWidth,maxHeight,r->Targets,BC250_EDID_MAX_MODES);
    if(!r->TargetCount) {status=BC250_DISP_STATUS_NOT_SUPPORTED;goto done;}
    r->Phase=BC250_SINK_PHASE_READY;status=0;
done:
    raw->Reads=io.Reads;raw->Writes=io.Writes;raw->Refusals=io.Refusals;raw->StalledUs=io.StalledUs;
    if(!guard->Current(guard->Context) && !r->CleanupStatus)status=BC250_DISP_STATUS_BUSY;
    r->Status=status;return status;
}
