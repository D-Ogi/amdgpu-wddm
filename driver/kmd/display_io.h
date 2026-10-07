// The checked register accessors of the display-mode code (dpaux.c, dcn_scale.c), with no WDK header: modeset.c
// gives them BAR5 and KeStallExecutionProcessor, driver\kmd\test gives them a fake register file and a fake clock.
// Nothing reaches a register past Bc250DispRead/Bc250DispWrite, and they admit only the offsets of gen_regs.py's
// display tables (g_MmioDisplayAllow, g_MmioDisplayWriteAllow).
#pragma once

#define BC250_DISP_STATUS_SUCCESS 0L
#define BC250_DISP_STATUS_ACCESS_DENIED ((long)0xC0000022L)     // offset not on the display table
#define BC250_DISP_STATUS_TIMEOUT ((long)0xC00000B5L)           // STATUS_IO_TIMEOUT: a bounded wait ran out
#define BC250_DISP_STATUS_BUDGET ((long)0xC0000044L)            // STATUS_QUOTA_EXCEEDED: the operation's stall budget
#define BC250_DISP_STATUS_DEVICE ((long)0xC0000185L)            // STATUS_IO_DEVICE_ERROR: the engine said no
#define BC250_DISP_STATUS_BUSY ((long)0x80000011L)              // STATUS_DEVICE_BUSY: another owner holds the engine
#define BC250_DISP_STATUS_NOT_SUPPORTED ((long)0xC00000BBL)     // a request this code does not program
#define BC250_DISP_STATUS_INVALID ((long)0xC000000DL)           // STATUS_INVALID_PARAMETER
#define BC250_DISP_STATUS_MISMATCH ((long)0xC000009CL)          // STATUS_DEVICE_DATA_ERROR: a precondition failed

typedef long (*BC250_DISP_READ_FN)(void* Context, unsigned long Offset, unsigned long* Value);
typedef long (*BC250_DISP_WRITE_FN)(void* Context, unsigned long Offset, unsigned long Value);
typedef void (*BC250_DISP_STALL_FN)(void* Context, unsigned long Microseconds);

typedef struct _BC250_DISP_IO {
    void* Context;
    BC250_DISP_READ_FN Read;
    BC250_DISP_WRITE_FN Write;
    BC250_DISP_STALL_FN Stall;              // a busy wait of the given length; the caller runs at PASSIVE_LEVEL
    unsigned long BudgetUs;                 // the stall time the whole operation may spend; 0 = no stall allowed
    unsigned long StalledUs;                // stall time spent so far
    unsigned long Reads, Writes, Refusals;
} BC250_DISP_IO;

int Bc250DispReadAllowed(unsigned long Offset);
int Bc250DispWriteAllowed(unsigned long Offset);
long Bc250DispRead(BC250_DISP_IO* Io, unsigned long Offset, unsigned long* Value);
long Bc250DispWrite(BC250_DISP_IO* Io, unsigned long Offset, unsigned long Value);
// Read, replace the bits of Mask with Value (already shifted), write. Returns the first failure.
long Bc250DispUpdate(BC250_DISP_IO* Io, unsigned long Offset, unsigned long Mask, unsigned long Value);
// One stall of Microseconds, counted against BudgetUs. BC250_DISP_STATUS_BUDGET (and no stall) when the budget
// would be exceeded.
long Bc250DispStall(BC250_DISP_IO* Io, unsigned long Microseconds);

// A field value placed under its mask (the mask's lowest set bit is the shift).
static __inline unsigned long Bc250DispField(unsigned long Mask, unsigned long Value)
{
    unsigned long shift = 0;
    if (Mask == 0) return 0;
    while (((Mask >> shift) & 1ul) == 0) shift++;
    return (Value << shift) & Mask;
}
static __inline unsigned long Bc250DispGet(unsigned long Register, unsigned long Mask)
{
    unsigned long shift = 0;
    if (Mask == 0) return 0;
    while (((Mask >> shift) & 1ul) == 0) shift++;
    return (Register & Mask) >> shift;
}
