// The kernel backend of driver/shim (bc250_shim.h), shared by every bring-up sequence that runs AMD's imported
// code inside the miniport (gart.c, psp.c). adev->backend points at the BC250_SEQUENCE of whoever runs at the
// moment; the owners serialize through Device->GartLock, so there is one at a time.
//
// Rules, the same for every sequence: registers only through the sequence's own generated table; the first
// refused access stops every further write; a PLAN executes no write at all and records what would be written.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include <ntstrsafe.h>
#include <stdarg.h>
#include "amdgpu.h"

void SequenceBegin(_Out_ BC250_SEQUENCE* Sequence, _In_ BC250_DEVICE* Device, BOOLEAN Plan,
                   _Out_writes_opt_(MaxWrites) BC250_SEQUENCE_WRITE* Writes, ULONG MaxWrites)
{
    Sequence->Device = Device;
    Sequence->Plan = Plan;
    Sequence->Fault = STATUS_SUCCESS;
    Sequence->FaultOffset = 0;
    Sequence->WriteCount = 0;
    Sequence->Writes = Writes;
    Sequence->MaxWrites = (Writes != NULL) ? MaxWrites : 0;
}

static void RecordFault(_Inout_ BC250_SEQUENCE* Sequence, NTSTATUS Status, ULONG Offset)
{
    if (NT_SUCCESS(Sequence->Fault))
    {
        Sequence->Fault = Status;
        Sequence->FaultOffset = Offset;
        GuardLog("%s: register 0x%05X refused (0x%08X), sequence stopped", Sequence->Name, Offset, Status);
    }
}

unsigned int bc250_shim_rreg(struct amdgpu_device* adev, unsigned int dword_index)
{
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)adev->backend;
    ULONG value = 0;
    NTSTATUS status;

    // After a fault nothing is read any more, and all ones ends every bit poll of the sequence at once. No value
    // read here can reach the hardware: writes are stopped as well.
    if (!NT_SUCCESS(sequence->Fault)) return 0xFFFFFFFFu;
    if (sequence->Plan && sequence->PlanAnswers != NULL && sequence->PlanAnswers(sequence, dword_index, &value)) return value;
    status = sequence->Read(sequence->Device, dword_index * 4, &value);
    if (!NT_SUCCESS(status)) RecordFault(sequence, status, dword_index * 4);
    return value;
}

void bc250_shim_wreg(struct amdgpu_device* adev, unsigned int dword_index, unsigned int value)
{
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)adev->backend;
    NTSTATUS status;

    if (!NT_SUCCESS(sequence->Fault))
    {
        // A sequence may name writes that must get through even when it has been stopped (giving a hardware
        // semaphore back, gart.c).
        if (!sequence->Plan && sequence->PassesFault != NULL && sequence->PassesFault(sequence, dword_index, value))
            sequence->Write(sequence->Device, dword_index * 4, value);
        return;
    }
    if (!sequence->Plan)
    {
        status = sequence->Write(sequence->Device, dword_index * 4, value);
        if (!NT_SUCCESS(status)) { RecordFault(sequence, status, dword_index * 4); return; }
    }
    if (sequence->WriteCount < sequence->MaxWrites)
    {
        sequence->Writes[sequence->WriteCount].Offset = dword_index * 4;
        sequence->Writes[sequence->WriteCount].Value = value;
    }
    sequence->WriteCount++;
}

// amdgpu's udelay() and, through the shim's mdelay(), its millisecond waits. Short ones stall; from a millisecond
// on the thread sleeps instead when it may (the owners hold a fast mutex, i.e. run at APC_LEVEL, where a wait
// with a timeout is allowed).
void bc250_shim_udelay(unsigned int usec)
{
    if (usec >= 1000 && KeGetCurrentIrql() <= APC_LEVEL)
    {
        LARGE_INTEGER interval;

        interval.QuadPart = -10ll * (LONGLONG)usec;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        return;
    }
    KeStallExecutionProcessor(usec);
}

void bc250_shim_log(int level, void* dev, const char* fmt, ...)
{
    va_list arguments;
    char line[160];

    UNREFERENCED_PARAMETER(dev);
    va_start(arguments, fmt);
    if (NT_SUCCESS(RtlStringCchVPrintfA(line, sizeof(line), fmt, arguments))) GuardLog("amdgpu[%d]: %s", level, line);
    va_end(arguments);
}
