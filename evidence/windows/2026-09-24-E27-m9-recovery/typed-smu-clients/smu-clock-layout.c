#include <stdio.h>
#include <stddef.h>
#include "../../bc250-win/driver/kmd/bc250kmd_escape.h"
int main(void) {
    printf("{\"size\":%zu",sizeof(BC250_ESCAPE_CLOCK));
#define FIELD(x) printf(",\"" #x "\":%zu",offsetof(BC250_ESCAPE_CLOCK,x))
    FIELD(Magic);FIELD(Command);FIELD(Status);FIELD(Version);FIELD(NtStatus);
    FIELD(AbiVersion);FIELD(Op);FIELD(RequestedMHz);FIELD(RequestedMv);
    FIELD(ObservedMHz);FIELD(ObservedVid);FIELD(TemperatureMc);FIELD(InitialMHz);
    FIELD(InitialVid);FIELD(ExpectedVid);FIELD(VoltageStaged);FIELD(Ready);FIELD(Reserved);
    puts("}");return 0;
}
