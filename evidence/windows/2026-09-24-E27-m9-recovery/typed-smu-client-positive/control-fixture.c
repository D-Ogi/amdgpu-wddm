#include "../../../bc250-win/driver/kmd/bc250kmd_escape.h"
__declspec(dllexport) long __stdcall Bc250ClockControl(unsigned long op,unsigned long mhz,unsigned long mv,BC250_ESCAPE_CLOCK* data,unsigned long bytes) {
    BC250_ESCAPE_CLOCK r={0};
    if(!data || bytes!=sizeof(r) || op>1)return (long)0xC000000D;
    if(op==0 && (mhz || mv))return (long)0xC000000D;
    if(op==1 && (mhz!=1000 || mv!=820))return (long)0xC000000D;
    r.Magic=BC250_ESCAPE_MAGIC;r.Command=BC250_ESCAPE_RUN_CLOCK;r.AbiVersion=BC250_CLOCK_ABI;
    r.Op=op;r.Ready=1;r.ObservedMHz=1000;r.ObservedVid=116;r.TemperatureMc=67500;
    *data=r;return 0;
}
