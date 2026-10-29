// Reserve a linear handoff surface before QUERYSEGMENT. The surface remains in
// the carved-out VRAM after unmapping: Basic Display can adopt it on driver stop.
#include "bc250kmd.h"

#define CONNECTOR_BUFFER_BYTES (4096ull * 2160ull * 4ull)

NTSTATUS ConnectorBufferPrepare(BC250_DEVICE* Device)
{
    ULONGLONG offset, bytes, end, limit;
    PHYSICAL_ADDRESS physical;
    if (Device->DisplayReservationEnd || Device->RuntimeFramebuffer)
        return STATUS_INVALID_DEVICE_STATE;
    if (!WddmFullTableSelected() || !Device->DcnWriteEnabled || !Device->VramEnabled ||
        !Device->VramWriteEnabled || !VramFramebufferOffset(Device,&offset) ||
        Device->VramLength<=BC250_VRAM_TOP_RESERVED) return STATUS_NOT_SUPPORTED;
    limit=Device->VramLength-BC250_VRAM_TOP_RESERVED;
    bytes=(ULONGLONG)Device->Post.Pitch*Device->Post.Height;
    if (!bytes || offset>limit || bytes>limit-offset) return STATUS_INVALID_PARAMETER;
    end=offset+bytes;
    if (end>MAXULONGLONG-65535ull) return STATUS_INVALID_PARAMETER;
    offset=(end+65535ull)&~65535ull;
    if (offset>limit || CONNECTOR_BUFFER_BYTES>limit-offset) return STATUS_NO_MEMORY;
    // PHYSICAL_ADDRESS is signed. Check the entire half-open mapping range
    // before either the unsigned-to-signed conversion or the physical addition.
    if (Device->VramPhysical.QuadPart<0 ||
        offset>(ULONGLONG)MAXLONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart ||
        CONNECTOR_BUFFER_BYTES>(ULONGLONG)MAXLONGLONG-
            (ULONGLONG)Device->VramPhysical.QuadPart-offset) return STATUS_INVALID_PARAMETER;
    physical.QuadPart=Device->VramPhysical.QuadPart+(LONGLONG)offset;
    Device->RuntimeFramebuffer=VramMapCpuRange(Device,physical,
        (SIZE_T)CONNECTOR_BUFFER_BYTES,PAGE_READWRITE);
    if (!Device->RuntimeFramebuffer) return STATUS_INSUFFICIENT_RESOURCES;
    Device->RuntimeFramebufferPhysical=physical;
    Device->RuntimeFramebufferLength=(SIZE_T)CONNECTOR_BUFFER_BYTES;
    Device->DisplayReservationEnd=offset+CONNECTOR_BUFFER_BYTES;
    RtlZeroMemory(Device->RuntimeFramebuffer,Device->RuntimeFramebufferLength);
    KeMemoryBarrier();
    GuardLog("connector: reserved linear handoff %llu bytes before the application segment",
        CONNECTOR_BUFFER_BYTES);
    return STATUS_SUCCESS;
}

void ConnectorBufferRelease(BC250_DEVICE* Device)
{
    // The hardware pointer's surfaces lie after this buffer in the same reservation (dcn_cursor.c).
    DcnCursorRelease(Device);
    if (Device->RuntimeFramebuffer) {
        MmUnmapIoSpace(Device->RuntimeFramebuffer,Device->RuntimeFramebufferLength);
        Device->RuntimeFramebuffer=NULL;
    }
    // Keep RuntimePost for StopDeviceAndReleasePostDisplayOwnership's reply.
    // A new StartDevice resets this reservation before its first query.
}
