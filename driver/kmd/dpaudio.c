// DisplayPort audio, steps 0, 1 and 2: the Azalia endpoint of the DP stream the firmware (GOP) left running is made
// present to the HD Audio side, so that Windows' inbox HDA class driver can see a DP sink, and that stream carries
// the audio: the 24 MHz wall clock (DCCG audio DTO1), the AFMT audio path and the DP secondary-data audio packets.
// The register work is dpaudio_seq.c (Linux amdgpu's dce_audio.c and dcn10_stream_encoder.c sequences, checked
// accessors, no Windows); this file is its owner in the miniport: switches, lock, record, log, entry points and the
// escape.
//
//   <service key>\Parameters
//     EnableDpAudio          REG_DWORD  1 (default) = run the start below. 0 = no audio register is read or
//                                       written at any time; the driver behaves as before this feature.
//     EnableDpAudioEndpoint  REG_DWORD  1 (default) = step 1, the endpoint fields and AUDIO_ENABLED. 0 = the same
//                                       as EnableDpAudio 0.
//     EnableDpAudioStream    REG_DWORD  1 (default) = step 2, the wall DTO, the AFMT audio path and the DP_SEC
//                                       audio packets on the inherited stream. 0 = the same as EnableDpAudio 0.
//     EnableDpAudioContainerId  REG_DWORD  1 (default) = step 4's container ID: the ELD carries the port ID the
//                                       operating system made for the child (DxgkDdiGetChildContainerId), so that
//                                       the audio endpoint and the monitor land in one device container. 0 = the
//                                       ELD keeps Linux DC's two constant port-ID halves, as before 0.7.216.25.
//
// All three are read at every start and resume. The endpoint is never shown without its stream: with step 1
// alone, Windows showed an active "Digital Audio (HDMI)" endpoint whose audio clock was not programmed, and it
// played no sound, while Edge slaved its video to that clock and played at about 0.7 times real time. So either
// switch at 0 means no endpoint and no stream, and a refusal or failure of the stream half leaves AUDIO_ENABLED 0.
//
// The start refuses, logs the reason and writes nothing unless every precondition holds (dpaudio_seq.c
// Bc250DpAudioDecide): BAR5 mapped, the codec is unit A's (M819), audio is strapped on, exactly one DP stream
// encoder carries a video stream on a DP SST back end, the endpoint that belongs to it reads the pin configuration
// Linux saw, and the DP reference clock counter reads a plausible clock. Then, in this order: hw_init and
// configure of the endpoint (step 1 without its last write), the stream (step 2, every write a read-modify-write of
// named fields, read back at once), and AUDIO_ENABLED last (dpaudio_seq.c Bc250DpAudioRun says why last). A failure
// or a read-back that differs anywhere runs the stop sequence at once: the stream off, then AUDIO_ENABLED 0. No
// OTG, HUBP or link register is written here.
//
// Entry points (docs/design/dp-audio.md): DpAudioStart from Bc250StartDevice after DisplayPrepareInheritedTiming,
// the seamless-boot point at which Linux adds audio to a stream it did not train (link_dpms.c:2520-2527);
// DpAudioStop from Bc250StopDevice before WddmStop and DcnStop, and on the way to D3, so the next owner (Basic
// Display, the next start) inherits the stream off and AUDIO_ENABLED 0; DpAudioResume on the way back to D0;
// DpAudioPathPower from CommitVidPn's path power transitions, so that a monitor powered off is an unplugged sink
// for audio, as Linux's audio stream disable and az_disable at DPMS off. Every entry point runs at PASSIVE_LEVEL
// (the settings are registry reads).
#include "bc250kmd.h"
#include "dpaudio_seq.h"

#define DPAUDIO_SETTING_ENABLE L"EnableDpAudio"
#define DPAUDIO_SETTING_ENDPOINT L"EnableDpAudioEndpoint"
#define DPAUDIO_SETTING_STREAM L"EnableDpAudioStream"
#define DPAUDIO_SETTING_CONTAINER L"EnableDpAudioContainerId"

// The ABI 1 reply is exactly the fields before the stream record, and the ABI 2 reply the fields before the
// container-ID record. Both prefixes are frozen: an older tool keeps its layout.
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPAUDIO, SwitchStream) == BC250_DPAUDIO_ABI1_SIZE);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPAUDIO, PortId) == BC250_DPAUDIO_ABI2_SIZE);

// The callbacks dpaudio_seq.c performs its checked accesses through. The tables are checked there; checked again
// here so that nothing in this file can reach BAR5 past them either.
static long MmioAzRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    const BC250_DEVICE* device = (const BC250_DEVICE*)Context;

    *Value = 0;
    if (device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    if (!Bc250AzReadAllowed(Offset)) return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

static long MmioAzWrite(void* Context, unsigned long Offset, unsigned long Value)
{
    const BC250_DEVICE* device = (const BC250_DEVICE*)Context;

    if (device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    if (!Bc250AzWriteAllowed(Offset)) return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

static void IoOpen(_In_ BC250_DEVICE* Device, _Out_ BC250_AZ_IO* Io)
{
    RtlZeroMemory(Io, sizeof(*Io));
    Io->Context = Device;
    Io->Read = MmioAzRead;
    Io->Write = MmioAzWrite;
}

// Under the lock.
static void IoClose(_Inout_ BC250_DPAUDIO* Audio, _In_ const BC250_AZ_IO* Io)
{
    Audio->IndirectReads += Io->IndirectReads;
    Audio->IndirectWrites += Io->IndirectWrites;
    Audio->DirectWrites += Io->DirectWrites;
    Audio->AccessRefusals += Io->Refusals;
}

void DpAudioInitialize(_Inout_ BC250_DEVICE* Device)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;

    KeInitializeSpinLock(&audio->Lock);
    audio->State = BC250_DPAUDIO_STATE_IDLE;
    audio->Reason = BC250_DPAUDIO_REASON_NOT_STARTED;
    audio->SwitchEnable = audio->SwitchEndpoint = audio->SwitchStream = BC250_DPAUDIO_NO_SWITCH;
    // The container-ID switch is read at the start too, so before the first start it is "not read yet" and not 0,
    // which the CLI would print as "the switch is off" (b26 review finding F1).
    audio->SwitchContainerId = BC250_DPAUDIO_NO_SWITCH;
    audio->StreamState = BC250_DPAUDIO_STREAM_OFF;
    audio->StreamStep = BC250_DPAUDIO_STEP_NONE;
}

// Under the lock: what a stream sequence left behind, into the record. The step and the mismatch are the first
// failure of that sequence, or NONE and 0.
static void RecordStream(_Inout_ BC250_DPAUDIO* Audio, _In_ const BC250_DPAUDIO_STREAM_RESULT* Failure,
                         _In_ const BC250_DPAUDIO_STREAM_RESULT* Left)
{
    Audio->StreamStep = Failure->Step;
    Audio->StreamStatus = Failure->Status;
    Audio->MismatchOffset = Failure->MismatchOffset;
    Audio->MismatchExpected = Failure->MismatchExpected;
    Audio->MismatchActual = Failure->MismatchActual;
    Audio->DtoSource = Left->DtoSource;
    Audio->SecCntl = Left->SecCntl;
    Audio->AfmtCntl = Left->AfmtCntl;
    Audio->PacketControl = Left->PacketControl;
    Audio->PacketControl2 = Left->PacketControl2;
}

// What a start or resume did, collected under the lock and logged after it.
typedef struct _DPAUDIO_START {
    BC250_DPAUDIO_RUN Seq;                  // dpaudio_seq.c: observation, decision, the write groups
    BC250_DPAUDIO_SINK Sink;                // step 4: the sink from the EDID, or the fixed set
    ULONG Gate;                             // the switches and the mapping, before any register
    ULONG Reason;                           // Bc250DpAudioRun's result when the gate was open
} DPAUDIO_START;

static const char* const g_GroupName[] = { "hw_init", "configure", "stream", "enable" };

static void LogStopSequence(_In_z_ const char* What, ULONG Stream, ULONG Endpoint, _In_ const BC250_DPAUDIO_STOP* Stop,
                            BOOLEAN StreamWritten, BOOLEAN EndpointWritten)
{
    if (StreamWritten) {
        GuardLog("dpaudio: %s: stream off on DP%lu: sec 0x%08lX afmt 0x%08lX pkt 0x%08lX", What, Stream,
                 Stop->Stream.SecCntl, Stop->Stream.AfmtCntl, Stop->Stream.PacketControl);
        if (Stop->StreamStatus < 0)
            GuardLog("dpaudio: %s: stream off failed at step %s (0x%08lX)", What, Bc250DpAudioStepText(Stop->Stream.Step),
                     (ULONG)Stop->StreamStatus);
    }
    if (EndpointWritten)
        GuardLog("dpaudio: %s: AUDIO_ENABLED cleared on endpoint %lu: hpc 0x%08lX -> 0x%08lX (0x%08lX)", What, Endpoint,
                 Stop->Endpoint.HotPlugBefore, Stop->Endpoint.HotPlugAfter, (ULONG)Stop->EndpointStatus);
}

static void LogStart(_In_ const DPAUDIO_START* Start, _In_z_ const char* What)
{
    const BC250_DPAUDIO_RUN* run = &Start->Seq;
    const BC250_DPAUDIO_STREAM_RESULT* s = &run->Stream;
    const ULONG* r = run->Obs.Regs;

    if (Start->Gate != BC250_DPAUDIO_REASON_OK) {
        GuardLog("dpaudio: %s refused: %s; no audio register read or written", What, Bc250DpAudioReasonText(Start->Gate));
        return;
    }
    GuardLog("dpaudio: codec 0x%08lX rev 0x%08lX straps 0x%08lX vid 0x%08lX/0x%08lX be 0x%08lX/0x%08lX hpd 0x%lX/0x%lX",
             r[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE], r[BC250_DPAUDIO_OBS_CODEC_REVISION], r[BC250_DPAUDIO_OBS_DC_PINSTRAPS],
             r[BC250_DPAUDIO_OBS_DP0_VID_STREAM_CNTL], r[BC250_DPAUDIO_OBS_DP1_VID_STREAM_CNTL],
             r[BC250_DPAUDIO_OBS_DIG0_BE_CNTL], r[BC250_DPAUDIO_OBS_DIG1_BE_CNTL],
             r[BC250_DPAUDIO_OBS_HPD0_INT_STATUS], r[BC250_DPAUDIO_OBS_HPD1_INT_STATUS]);
    GuardLog("dpaudio: ep0 cfg 0x%08lX hpc 0x%08lX unsol 0x%lX sense 0x%08lX; ep1 cfg 0x%08lX hpc 0x%08lX; first fail 0x%08lX",
             r[BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT], r[BC250_DPAUDIO_OBS_EP0_HOT_PLUG_CONTROL],
             r[BC250_DPAUDIO_OBS_EP0_UNSOLICITED_RESPONSE], r[BC250_DPAUDIO_OBS_EP0_PIN_SENSE],
             r[BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT], r[BC250_DPAUDIO_OBS_EP1_HOT_PLUG_CONTROL], (ULONG)run->Obs.FirstFailure);
    GuardLog("dpaudio: sec 0x%08lX/0x%08lX dto src 0x%08lX dto1 %lu/%lu afmt 0x%08lX/0x%08lX pkt 0x%08lX/0x%08lX",
             r[BC250_DPAUDIO_OBS_DP0_SEC_CNTL], r[BC250_DPAUDIO_OBS_DP1_SEC_CNTL], r[BC250_DPAUDIO_OBS_DTO_SOURCE],
             r[BC250_DPAUDIO_OBS_DTO1_PHASE], r[BC250_DPAUDIO_OBS_DTO1_MODULE],
             r[BC250_DPAUDIO_OBS_DIG0_AFMT_CNTL], r[BC250_DPAUDIO_OBS_DIG1_AFMT_CNTL],
             r[BC250_DPAUDIO_OBS_DIG0_AFMT_AUDIO_PACKET_CONTROL], r[BC250_DPAUDIO_OBS_DIG1_AFMT_AUDIO_PACKET_CONTROL]);
    GuardLog("dpaudio: DP reference clock count %lu (100 kHz units): DTO1 module %lu, phase %lu, for a 24 MHz wall clock",
             run->Plan.RefClock, run->Plan.DtoModule, (ULONG)BC250_DPAUDIO_DTO1_PHASE);
    if (!run->Wrote) {
        GuardLog("dpaudio: %s refused, nothing written: %s (DP%lu ep %lu notes 0x%lX)", What,
                 Bc250DpAudioReasonText(Start->Reason), run->Plan.Stream, run->Plan.Endpoint, run->Plan.Notes);
        return;
    }
    GuardLog("dpaudio: preconditions hold: stream DP%lu, endpoint %lu, notes 0x%lX", run->Plan.Stream,
             run->Plan.Endpoint, run->Plan.Notes);
    if (run->Groups >= 1)
        GuardLog("dpaudio: hw_init done on endpoint 0: rates 0x%08lX power 0x%08lX hpc 0x%08lX, %lu writes",
                 run->Init.SizeRates, run->Init.PowerStates, run->Init.HotPlugAfter, run->Init.Writes);
    if (run->Groups >= 2)
    {
        GuardLog("dpaudio: configure done on endpoint %lu (%s), %lu writes, speakers 0x%02lX", run->Plan.Endpoint,
                 Start->Sink.FromEdid ? "EDID" : "fixed set", run->Config.Writes, Start->Sink.Speakers);
        GuardLog("dpaudio: LPCM %lu ch rates 0x%02lX sizes 0x%lX, sink 0x%04lX/0x%04lX '%s'", Start->Sink.LpcmChannels,
                 Start->Sink.LpcmRates, Start->Sink.LpcmSizes, Start->Sink.Manufacturer, Start->Sink.Product,
                 Start->Sink.Name);
        GuardLog("dpaudio: ELD port id 0x%016llX (%s)", run->Config.PortId,
                 Start->Sink.HasPortId ? "the container ID of the child" : "Linux's constant: no container ID yet");
    }
    if (run->Groups >= 3) {
        GuardLog("dpaudio: stream on DP%lu: DTO_SOURCE 0x%08lX DTO1 %lu/%lu, AUD_N 0x%08lX timestamp 0x%lX, %lu writes",
                 run->Plan.Stream, s->DtoSource, s->DtoPhase, s->DtoModule, s->AudN, s->Timestamp, s->Writes);
        GuardLog("dpaudio: stream on DP%lu: AFMT_CNTL 0x%08lX SRC 0x%lX PACKET 0x%08lX PACKET2 0x%08lX SEC_CNTL 0x%08lX",
                 run->Plan.Stream, s->AfmtCntl, s->SrcControl, s->PacketControl, s->PacketControl2, s->SecCntl);
    }
    if (run->Groups >= 4) {
        GuardLog("dpaudio: AUDIO_ENABLED set on endpoint %lu: hpc 0x%08lX -> 0x%08lX", run->Plan.Endpoint,
                 run->Enable.HotPlugBefore, run->Enable.HotPlugAfter);
        return;
    }
    GuardLog("dpaudio: write failed in %s (0x%08lX), stream step %s", g_GroupName[run->Groups], (ULONG)run->Status,
             Bc250DpAudioStepText(s->Step));
    if (s->Status == BC250_AZ_STATUS_MISMATCH)
        GuardLog("dpaudio: read-back differs at 0x%05lX: named bits written 0x%08lX, read 0x%08lX", s->MismatchOffset,
                 s->MismatchExpected, s->MismatchActual);
    LogStopSequence("undo", run->Plan.Stream, run->Plan.Endpoint, &run->Undo, (BOOLEAN)(run->StreamWrote != 0), TRUE);
}

static void StartCore(_Inout_ BC250_DEVICE* Device, BOOLEAN Resume)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    DPAUDIO_START* start;
    BC250_AZ_IO io;
    ULONG enable = GuardReadSetting(DPAUDIO_SETTING_ENABLE, 1);
    ULONG endpointSwitch = GuardReadSetting(DPAUDIO_SETTING_ENDPOINT, 1);
    ULONG streamSwitch = GuardReadSetting(DPAUDIO_SETTING_STREAM, 1);
    ULONG containerSwitch = GuardReadSetting(DPAUDIO_SETTING_CONTAINER, 1);
    ULONGLONG portId = 0;
    ULONG osManufacturer = 0, osProduct = 0, identityDiffers = 0;
    KIRQL irql;

    // From pool: the observation alone is 270 bytes, and a start already runs deep in dxgkrnl's stack (M104).
    start = (DPAUDIO_START*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*start), BC250_TAG);
    if (start == NULL) {
        GuardLog("dpaudio: %s skipped: no memory for the record; nothing read or written", Resume ? "resume" : "start");
        return;
    }
    IoOpen(Device, &io);
    // Step 4: the EDID that modeset.c read at this start, or the fixed set of step 1 without one (dpaudio_seq.h).
    (void)Bc250DpAudioSinkFromEdid(ModesetEdidForAudio(Device), &start->Sink);
    // The container ID of a boot's first start is not known yet: the operating system calls
    // DxgkDdiGetChildContainerId only after DxgkDdiStartDevice returns, and DpAudioContainerId then refreshes the
    // ELD. Every later start (a resume, the monitor's path power coming back) has it and writes it at once.
    KeAcquireSpinLock(&audio->Lock, &irql);
    audio->SwitchContainerId = containerSwitch;
    if (containerSwitch == 1) { portId = audio->PortId; osManufacturer = audio->OsManufacturer; osProduct = audio->OsProduct; }
    KeReleaseSpinLock(&audio->Lock, irql);
    identityDiffers = (ULONG)Bc250DpAudioSinkContainer(&start->Sink, portId, osManufacturer, osProduct);
    start->Seq.Sink = &start->Sink;
    start->Gate = Bc250DpAudioGate(Device->Mmio != NULL, enable, endpointSwitch, streamSwitch);
    KeAcquireSpinLock(&audio->Lock, &irql);
    if (Resume) audio->Resumes++; else audio->Starts++;
    audio->SwitchEnable = enable;
    audio->SwitchEndpoint = endpointSwitch;
    audio->SwitchStream = streamSwitch;
    audio->Written = audio->StreamWritten = FALSE;
    audio->Notes = 0;
    audio->LastStatus = 0;
    audio->StreamState = BC250_DPAUDIO_STREAM_OFF;
    audio->StreamStep = BC250_DPAUDIO_STEP_NONE;
    audio->StreamStatus = 0;
    audio->MismatchOffset = audio->MismatchExpected = audio->MismatchActual = 0;
    if (start->Gate != BC250_DPAUDIO_REASON_OK) {
        audio->State = BC250_DPAUDIO_STATE_REFUSED;
        audio->Reason = start->Gate;
        audio->Refusals++;
    } else {
        const BC250_DPAUDIO_RUN* run = &start->Seq;

        start->Reason = Bc250DpAudioRun(&io, &start->Seq);
        audio->Reason = start->Reason;
        audio->Notes = run->Plan.Notes;
        audio->SinkFromEdid = start->Sink.FromEdid;
        if (identityDiffers) audio->Notes |= BC250_DPAUDIO_NOTE_OS_IDENTITY;
        // What the configure group left in the ELD's two port-ID words: the operating system's ID, or, before it
        // has given one, Linux DC's constants. 0 when the group did not run.
        if (run->Groups >= 2) audio->PortIdInEld = run->Config.PortId;
        audio->Endpoint = run->Plan.Endpoint;
        audio->Stream = run->Plan.Stream;
        audio->RefClockCount = run->Plan.RefClock;
        audio->CodecId = run->Obs.Regs[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE];
        audio->ConfigDefault = run->Obs.Regs[run->Plan.Endpoint == 0 ? BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT
                                                                      : BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT];
        // From the first write on, the stop path owes the endpoint an AUDIO_ENABLED 0. A failed start already ran
        // the stop sequence; the stop path runs its endpoint half once more, which changes nothing.
        audio->Written = run->Wrote != 0;
        audio->LastStatus = run->Status;
        if (!run->Wrote) {
            audio->State = BC250_DPAUDIO_STATE_REFUSED;
            audio->Refusals++;
        } else if (start->Reason == BC250_DPAUDIO_REASON_OK) {
            audio->State = BC250_DPAUDIO_STATE_ENABLED;
            audio->StreamState = BC250_DPAUDIO_STREAM_ON;
            audio->StreamWritten = TRUE;
            audio->StreamOn++;
            audio->DtoModule = run->Stream.DtoModule;
            audio->DtoPhase = run->Stream.DtoPhase;
            RecordStream(audio, &run->Stream, &run->Stream);
            audio->HotPlugBefore = run->Enable.HotPlugBefore;
            audio->HotPlugAfter = run->Enable.HotPlugAfter;
        } else {
            audio->State = BC250_DPAUDIO_STATE_FAILED;
            audio->Failures++;
            if (run->StreamWrote) {
                audio->StreamState = BC250_DPAUDIO_STREAM_UNDONE;
                audio->StreamUndos++;
                audio->StreamOff++;
                audio->DtoModule = run->Stream.DtoModule;
                audio->DtoPhase = run->Stream.DtoPhase;
                RecordStream(audio, &run->Stream, &run->Undo.Stream);
            }
            audio->HotPlugBefore = run->Undo.Endpoint.HotPlugBefore;
            audio->HotPlugAfter = run->Undo.Endpoint.HotPlugAfter;
        }
    }
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    LogStart(start, Resume ? "resume" : "start");
    ExFreePoolWithTag(start, BC250_TAG);
}

void DpAudioStart(_Inout_ BC250_DEVICE* Device)
{
    StartCore(Device, FALSE);
}

// Nothing of the endpoint or the stream is assumed to survive D3: the whole start runs again, preconditions included.
void DpAudioResume(_Inout_ BC250_DEVICE* Device)
{
    StartCore(Device, TRUE);
}

// Under the lock: the stop sequence for what this start left on, and the record of it. Returns TRUE when it ran.
// Both owed flags clear only when the sequence succeeded, so that a later stop tries the failed half again.
static BOOLEAN StopLocked(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_AZ_IO* Io, ULONG State, ULONG Reason,
                          _Out_ BC250_DPAUDIO_STOP* Stop, _Out_ BOOLEAN* StreamWritten, _Out_ BOOLEAN* EndpointWritten)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    long status;

    RtlZeroMemory(Stop, sizeof(*Stop));
    *StreamWritten = audio->StreamWritten;
    *EndpointWritten = audio->Written;
    if ((!audio->StreamWritten && !audio->Written) || Device->Mmio == NULL) return FALSE;
    status = Bc250DpAudioStopSequence(Io, audio->Stream, audio->Endpoint, audio->StreamWritten, audio->Written, Stop);
    if (audio->StreamWritten) {
        audio->StreamOff++;
        audio->StreamState = Stop->StreamStatus >= 0 ? BC250_DPAUDIO_STREAM_OFF : BC250_DPAUDIO_STREAM_UNDONE;
        RecordStream(audio, &Stop->Stream, &Stop->Stream);
    }
    if (audio->Written) {
        audio->HotPlugBefore = Stop->Endpoint.HotPlugBefore;
        audio->HotPlugAfter = Stop->Endpoint.HotPlugAfter;
    }
    audio->State = State;
    audio->Reason = Reason;
    audio->LastStatus = status;
    if (status < 0) audio->Failures++;
    else audio->StreamWritten = audio->Written = FALSE;
    return TRUE;
}

void DpAudioStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_STOP stop;
    BC250_AZ_IO io;
    BOOLEAN ran, streamWritten, endpointWritten;
    ULONG stream, endpoint, state;
    KIRQL irql;

    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    audio->Stops++;
    stream = audio->Stream;
    endpoint = audio->Endpoint;
    state = audio->State;
    ran = StopLocked(Device, &io, BC250_DPAUDIO_STATE_STOPPED, BC250_DPAUDIO_REASON_STOPPED, &stop, &streamWritten,
                     &endpointWritten);
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    if (ran) LogStopSequence("stop", stream, endpoint, &stop, streamWritten, endpointWritten);
    else if (state != BC250_DPAUDIO_STATE_IDLE)
        GuardLog("dpaudio: stop: nothing to clear (state %lu, this start left nothing on)", state);
}

void DpAudioPathPower(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_STOP stop;
    BC250_AZ_IO io;
    BOOLEAN ran = FALSE, retry, again, streamWritten = FALSE, endpointWritten = FALSE;
    ULONG stream, endpoint;
    KIRQL irql;

    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    stream = audio->Stream;
    endpoint = audio->Endpoint;
    retry = On && audio->State == BC250_DPAUDIO_STATE_REFUSED && audio->Reason == BC250_DPAUDIO_REASON_NO_STREAM;
    again = On && audio->State == BC250_DPAUDIO_STATE_PATH_OFF;
    if (again) audio->PathOn++;
    // Only what this start turned on follows the monitor's power; a refused or stopped start stays as it is.
    if (!On && audio->State == BC250_DPAUDIO_STATE_ENABLED) {
        audio->PathOff++;
        ran = StopLocked(Device, &io, BC250_DPAUDIO_STATE_PATH_OFF, BC250_DPAUDIO_REASON_PATH_OFF, &stop, &streamWritten,
                         &endpointWritten);
    }
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    if (ran) LogStopSequence("path power off", stream, endpoint, &stop, streamWritten, endpointWritten);
    // The monitor is back: the whole start again, preconditions and all, counted as a resume. The same for a start
    // or resume that found no DP stream (the monitor was off, or D0 came back before the stream did).
    if (again || retry) {
        GuardLog("dpaudio: path power on after %s: start again", again ? "a path power off" : "a refusal for no stream");
        StartCore(Device, TRUE);
    }
}

// Step 4's container ID (docs/design/dp-audio.md). dxgkrnl calls DxgkDdiGetChildContainerId for the one child
// after DxgkDdiStartDevice has returned, so the port ID always arrives after the start that configured the
// endpoint. The port ID is kept for every later start, and the ELD of a running endpoint is refreshed at once:
// read first, written only when it differs, and bracketed by a presence cycle so that the HD Audio class driver
// reads the ELD again (dpaudio_seq.c Bc250DpAudioPortIdRefresh says why). A refusal or a failure never touches
// the endpoint's audio: the stream and AUDIO_ENABLED stay as the start left them.
void DpAudioContainerId(_Inout_ BC250_DEVICE* Device, ULONGLONG PortId, USHORT Manufacturer, USHORT Product)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_PORTID refresh;
    BC250_AZ_IO io;
    ULONG containerSwitch = GuardReadSetting(DPAUDIO_SETTING_CONTAINER, 1);
    ULONG endpoint, calls;
    BOOLEAN ran = FALSE, enabled;
    long status = 0;
    KIRQL irql;

    RtlZeroMemory(&refresh, sizeof(refresh));
    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    audio->ContainerCalls++;
    calls = audio->ContainerCalls;
    audio->SwitchContainerId = containerSwitch;
    audio->PortId = PortId;
    audio->OsManufacturer = Manufacturer;
    audio->OsProduct = Product;
    endpoint = audio->Endpoint;
    enabled = (BOOLEAN)(audio->State == BC250_DPAUDIO_STATE_ENABLED);
    // Only an endpoint this driver configured gets its ELD corrected. Without one the port ID waits for the next
    // start, which writes it in the configure group and needs no presence cycle.
    if (containerSwitch == 1 && PortId != 0 && Device->Mmio != NULL && audio->Written) {
        status = Bc250DpAudioPortIdRefresh(&io, endpoint, PortId, enabled, &refresh);
        audio->PortIdStatus = status;
        if (status < 0) audio->Failures++;
        else audio->PortIdInEld = refresh.After;
        if (refresh.Changed) audio->PortIdWrites++; else audio->PortIdSkips++;
        if (refresh.Cycled) audio->PortIdCycles++;
        ran = TRUE;
    } else if (containerSwitch == 1 && PortId != 0) {
        audio->PortIdSkips++;
    }
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    // One line per call, so that a boot's log says what the operating system gave and what the ELD holds.
    GuardLog("dpaudio: container id call %lu: port 0x%016llX mfg 0x%04X product 0x%04X, switch %lu",
             calls, PortId, (ULONG)Manufacturer, (ULONG)Product, containerSwitch);
    if (ran) {
        GuardLog("dpaudio: ELD port id on endpoint %lu: 0x%016llX -> 0x%016llX (0x%08lX)", endpoint, refresh.Before,
                 refresh.After, (ULONG)status);
        GuardLog("dpaudio: ELD port id: %s, %s, %lu writes", refresh.Changed ? "written" : "already held",
                 refresh.Cycled ? "presence cycled" : "no presence cycle", refresh.Writes);
    }
    else if (containerSwitch != 1)
        GuardLog("dpaudio: container id ignored: EnableDpAudioContainerId is not 1; the ELD keeps Linux's constant");
    else if (PortId == 0)
        GuardLog("dpaudio: container id carries no port ID; the ELD keeps Linux's constant");
    else
        GuardLog("dpaudio: container id kept for the next start: no endpoint of this driver is configured");
}

// BC250_ESCAPE_RUN_DPAUDIO (bc250kmd_escape.h). HardwareAccess only, administrators only, one of three exact sizes
// (display.c): ABI 1 with BC250_DPAUDIO_ABI1_SIZE bytes, ABI 2 with BC250_DPAUDIO_ABI2_SIZE, ABI 3 with the whole
// structure. Nothing past Size is read or written.
void DpAudioRequest(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DPAUDIO* Data, ULONG Size, BOOLEAN Admin,
                    ULONG EscapeFlags)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_OBSERVATION* obs = NULL;
    ULONG op = Data->Op, abi = Data->AbiVersion, i;
    const BOOLEAN abi3 = abi == BC250_DPAUDIO_ABI && Size == sizeof(BC250_ESCAPE_DPAUDIO);
    const BOOLEAN abi2 = abi == BC250_DPAUDIO_ABI_2 && Size == BC250_DPAUDIO_ABI2_SIZE;
    const BOOLEAN abi1 = abi == BC250_DPAUDIO_ABI_1 && Size == BC250_DPAUDIO_ABI1_SIZE;
    BC250_AZ_IO io;
    long status = STATUS_SUCCESS;
    KIRQL irql;

    if (Size != BC250_DPAUDIO_ABI1_SIZE && Size != BC250_DPAUDIO_ABI2_SIZE &&
        Size != sizeof(BC250_ESCAPE_DPAUDIO)) return;                                       // display.c refused it
    RtlZeroMemory(Data, Size);
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_RUN_DPAUDIO;
    Data->Version = BC250_KMD_VERSION;
    Data->AbiVersion = abi1 ? BC250_DPAUDIO_ABI_1 : (abi2 ? BC250_DPAUDIO_ABI_2 : BC250_DPAUDIO_ABI);
    Data->Op = op;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    if (!Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    // HardwareAccess requests dxgkrnl's Level Two exclusion of stop and MMIO unmap, as OBSERVE_DCN.
    if (EscapeFlags != 1u || !(abi1 || abi2 || abi3) ||
        (op != BC250_DPAUDIO_OP_OBSERVE && op != BC250_DPAUDIO_OP_STATE)) {
        Data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
        return;
    }
    Data->Flags = Device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0;
    if (op == BC250_DPAUDIO_OP_OBSERVE) {
        if (Device->Mmio == NULL) status = STATUS_DEVICE_NOT_READY;
        else {
            obs = (BC250_DPAUDIO_OBSERVATION*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*obs), BC250_TAG);
            if (obs == NULL) status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    if (obs != NULL) Bc250DpAudioObserve(&io, obs);
    IoClose(audio, &io);
    Data->State = audio->State; Data->Reason = audio->Reason; Data->Notes = audio->Notes;
    Data->Endpoint = audio->Endpoint; Data->Stream = audio->Stream;
    Data->SwitchEnable = audio->SwitchEnable; Data->SwitchEndpoint = audio->SwitchEndpoint;
    Data->Starts = audio->Starts; Data->Resumes = audio->Resumes; Data->Stops = audio->Stops;
    Data->Refusals = audio->Refusals; Data->Failures = audio->Failures;
    Data->PathOn = audio->PathOn; Data->PathOff = audio->PathOff;
    Data->IndirectReads = audio->IndirectReads; Data->IndirectWrites = audio->IndirectWrites;
    Data->DirectWrites = audio->DirectWrites; Data->AccessRefusals = audio->AccessRefusals;
    Data->CodecId = audio->CodecId; Data->ConfigDefault = audio->ConfigDefault;
    Data->HotPlugBefore = audio->HotPlugBefore; Data->HotPlugAfter = audio->HotPlugAfter;
    Data->LastStatus = (ULONG)audio->LastStatus;
    if (abi2 || abi3) {
        Data->SwitchStream = audio->SwitchStream;
        Data->StreamState = audio->StreamState; Data->StreamStep = audio->StreamStep;
        Data->StreamStatus = (ULONG)audio->StreamStatus; Data->RefClockCount = audio->RefClockCount;
        Data->DtoModule = audio->DtoModule; Data->DtoPhase = audio->DtoPhase;
        Data->MismatchOffset = audio->MismatchOffset;
        Data->MismatchExpected = audio->MismatchExpected; Data->MismatchActual = audio->MismatchActual;
        Data->DtoSource = audio->DtoSource; Data->SecCntl = audio->SecCntl; Data->AfmtCntl = audio->AfmtCntl;
        Data->PacketControl = audio->PacketControl; Data->PacketControl2 = audio->PacketControl2;
        Data->StreamOn = audio->StreamOn; Data->StreamOff = audio->StreamOff; Data->StreamUndos = audio->StreamUndos;
    }
    if (abi3) {
        Data->PortId = audio->PortId; Data->PortIdInEld = audio->PortIdInEld;
        Data->OsManufacturer = audio->OsManufacturer; Data->OsProduct = audio->OsProduct;
        Data->SwitchContainerId = audio->SwitchContainerId;
        Data->ContainerCalls = audio->ContainerCalls; Data->PortIdWrites = audio->PortIdWrites;
        Data->PortIdSkips = audio->PortIdSkips; Data->PortIdCycles = audio->PortIdCycles;
        Data->PortIdStatus = (ULONG)audio->PortIdStatus; Data->SinkFromEdid = audio->SinkFromEdid;
    }
    KeReleaseSpinLock(&audio->Lock, irql);
    if (obs != NULL) {
        BC250_DPAUDIO_PLAN plan;

        for (i = 0; i < BC250_DPAUDIO_OBS_SLOTS; i++) Data->Regs[i] = obs->Regs[i];
        Data->ValidMask = obs->ValidMask;
        Data->ObsReason = Bc250DpAudioDecide(obs, &plan);
        Data->ObsStream = plan.Stream;
        Data->ObsEndpoint = plan.Endpoint;
        Data->ObsNotes = plan.Notes;
        status = obs->FirstFailure;
        ExFreePoolWithTag(obs, BC250_TAG);
    }
    Data->NtStatus = (ULONG)status;
    Data->Status = status >= 0 ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
