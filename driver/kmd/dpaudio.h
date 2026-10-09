// DisplayPort audio as the miniport keeps it (dpaudio.c): steps 0, 1 and 2 of the DP audio work. The register side
// with no Windows in it is dpaudio_seq.c. Included by bc250kmd.h after the WDK headers.
#pragma once

typedef struct _BC250_DPAUDIO {
    // A leaf lock. Held across every indirect access, because the INDEX/DATA pair of an endpoint is two
    // separate register accesses, and across the record below. The holder takes nothing else and logs nothing.
    KSPIN_LOCK Lock;
    ULONG State;                            // BC250_DPAUDIO_STATE_*
    ULONG Reason;                           // enum bc250_dpaudio_reason
    ULONG Notes;                            // BC250_DPAUDIO_NOTE_*
    ULONG Endpoint, Stream;
    ULONG SwitchEnable, SwitchEndpoint, SwitchStream;   // as the last start read them, BC250_DPAUDIO_NO_SWITCH before
    ULONG Starts, Resumes, Stops, Refusals, Failures, PathOn, PathOff;
    ULONG IndirectReads, IndirectWrites, DirectWrites, AccessRefusals;
    ULONG CodecId, ConfigDefault, HotPlugBefore, HotPlugAfter;
    LONG LastStatus;
    // Step 2, the stream half (BC250_ESCAPE_DPAUDIO ABI 2 fields of the same names).
    ULONG StreamState;                      // BC250_DPAUDIO_STREAM_*
    ULONG StreamStep;                       // enum bc250_dpaudio_step
    LONG StreamStatus;
    ULONG RefClockCount, DtoModule, DtoPhase;
    ULONG MismatchOffset, MismatchExpected, MismatchActual;
    ULONG DtoSource, SecCntl, AfmtCntl, PacketControl, PacketControl2;
    ULONG StreamOn, StreamOff, StreamUndos;
    // Step 4, the container ID (BC250_ESCAPE_DPAUDIO ABI 3 fields of the same names). PortId survives a stop: the
    // operating system gives it once per child enumeration, and every later start has to put it in the ELD.
    ULONGLONG PortId, PortIdInEld;
    ULONG OsManufacturer, OsProduct, SwitchContainerId;
    ULONG ContainerCalls, PortIdWrites, PortIdSkips, PortIdCycles;
    LONG PortIdStatus;
    ULONG SinkFromEdid;
    BOOLEAN Written;                        // this start wrote the endpoint: the stop path must clear AUDIO_ENABLED
    BOOLEAN StreamWritten;                  // the stream is on: the stop path must turn it off first
} BC250_DPAUDIO;

struct _BC250_DEVICE;
struct _BC250_ESCAPE_DPAUDIO;
void DpAudioInitialize(struct _BC250_DEVICE* Device);                   // AddDevice
void DpAudioStart(struct _BC250_DEVICE* Device);                        // StartDevice, after the inherited timing
void DpAudioResume(struct _BC250_DEVICE* Device);                       // back in D0
void DpAudioStop(struct _BC250_DEVICE* Device);                         // StopDevice before WddmStop/DcnStop; D3
void DpAudioPathPower(struct _BC250_DEVICE* Device, BOOLEAN On);        // CommitVidPn path power transition
// Step 4's container ID: DxgkDdiGetChildContainerId (pnp.c) hands over the port ID and identity the operating
// system made for the child, after DxgkDdiStartDevice has returned. PASSIVE_LEVEL.
void DpAudioContainerId(struct _BC250_DEVICE* Device, ULONGLONG PortId, USHORT Manufacturer, USHORT Product);
// Size: BC250_DPAUDIO_ABI1_SIZE or sizeof(BC250_ESCAPE_DPAUDIO), checked by display.c; nothing past it is touched.
void DpAudioRequest(struct _BC250_DEVICE* Device, struct _BC250_ESCAPE_DPAUDIO* Data, ULONG Size, BOOLEAN Admin,
                    ULONG EscapeFlags);
