// The display-mode code's checked accessors: display_io.h.
#define BC250_REGS_WITH_DISPLAY_TABLES
#include "regs.generated.h"
#include "display_io.h"

static int InTable(const unsigned long* Table, unsigned long Count, unsigned long Offset)
{
    unsigned long lo = 0, hi = Count;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        if (Table[mid] == Offset) return 1;
        if (Table[mid] < Offset) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int Bc250DispReadAllowed(unsigned long Offset)
{
    return InTable(g_MmioDisplayAllow, BC250_MMIO_DISPLAY_ALLOW_COUNT, Offset);
}

int Bc250DispWriteAllowed(unsigned long Offset)
{
    return InTable(g_MmioDisplayWriteAllow, BC250_MMIO_DISPLAY_WRITE_ALLOW_COUNT, Offset);
}

long Bc250DispRead(BC250_DISP_IO* Io, unsigned long Offset, unsigned long* Value)
{
    long status;
    *Value = 0;
    if (!Bc250DispReadAllowed(Offset)) { Io->Refusals++; return BC250_DISP_STATUS_ACCESS_DENIED; }
    status = Io->Read(Io->Context, Offset, Value);
    if (status >= 0) Io->Reads++;
    return status;
}

long Bc250DispWrite(BC250_DISP_IO* Io, unsigned long Offset, unsigned long Value)
{
    long status;
    if (!Bc250DispWriteAllowed(Offset)) { Io->Refusals++; return BC250_DISP_STATUS_ACCESS_DENIED; }
    status = Io->Write(Io->Context, Offset, Value);
    if (status >= 0) Io->Writes++;
    return status;
}

long Bc250DispUpdate(BC250_DISP_IO* Io, unsigned long Offset, unsigned long Mask, unsigned long Value)
{
    unsigned long v;
    long status = Bc250DispRead(Io, Offset, &v);
    if (status < 0) return status;
    return Bc250DispWrite(Io, Offset, (v & ~Mask) | (Value & Mask));
}

long Bc250DispStall(BC250_DISP_IO* Io, unsigned long Microseconds)
{
    if (Microseconds > Io->BudgetUs || Io->StalledUs > Io->BudgetUs - Microseconds) return BC250_DISP_STATUS_BUDGET;
    Io->Stall(Io->Context, Microseconds);
    Io->StalledUs += Microseconds;
    return BC250_DISP_STATUS_SUCCESS;
}
