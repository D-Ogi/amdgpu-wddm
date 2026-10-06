// DisplayPort audio, steps 0 and 1: the Azalia endpoint of the DP stream the firmware (GOP) left running is made
// present to the HD Audio side, so that Windows' inbox HDA class driver can see a DP sink. The register work is
// dpaudio_seq.c (Linux amdgpu's dce_audio.c sequences, checked accessors, no Windows); this file is its owner in
// the miniport: switches, lock, record, log, entry points and the escape.
//
//   <service key>\Parameters
//     EnableDpAudio          REG_DWORD  1 (default) = run the start below. 0 = no audio register is read or
//                                       written at any time; the driver behaves as before this feature.
//     EnableDpAudioEndpoint  REG_DWORD  1 (default) = step 1, the endpoint fields and AUDIO_ENABLED. 0 = the same
//                                       as EnableDpAudio 0 for now (step 2, the stream, is not in this driver).
//
// Both are read at every start and resume. The start refuses, logs the reason and writes nothing unless every
// precondition holds (dpaudio_seq.c Bc250DpAudioDecide): BAR5 mapped, the codec is unit A's (M819), audio is
// strapped on, exactly one DP stream encoder carries a video stream on a DP SST back end, and the endpoint that
// belongs to it reads the pin configuration Linux saw. No DIG, DP, OTG or DTO register is written here: the
// stream half (DTO, AFMT, DP_SEC) is step 2 and lives on another branch.
//
// Entry points (scratch design section 4): DpAudioStart from Bc250StartDevice after DisplayPrepareInheritedTiming,
// the seamless-boot point at which Linux adds audio to a stream it did not train (link_dpms.c:2520-2527);
// DpAudioStop from Bc250StopDevice before WddmStop and DcnStop, and on the way to D3, so the next owner (Basic
// Display, the next start) inherits AUDIO_ENABLED 0; DpAudioResume on the way back to D0; DpAudioPathPower from
// CommitVidPn's path power transitions, so that a monitor powered off is an unplugged sink for audio, as Linux's
// az_disable at DPMS off. Every entry point runs at PASSIVE_LEVEL (the settings are registry reads).
#include "bc250kmd.h"
#include "dpaudio_seq.h"

#define DPAUDIO_SETTING_ENABLE L"EnableDpAudio"
#define DPAUDIO_SETTING_ENDPOINT L"EnableDpAudioEndpoint"

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
    audio->SwitchEnable = audio->SwitchEndpoint = BC250_DPAUDIO_NO_SWITCH;
}

// What a start or resume did, collected under the lock and logged after it.
typedef struct _DPAUDIO_START {
    BC250_DPAUDIO_RUN Seq;                  // dpaudio_seq.c: observation, decision, the write groups
    ULONG Gate;                             // the switches and the mapping, before any register
    ULONG Reason;                           // Bc250DpAudioRun's result when the gate was open
} DPAUDIO_START;

static const char* const g_GroupName[] = { "hw_init", "configure", "enable" };

static void LogStart(_In_ const DPAUDIO_START* Start, _In_z_ const char* What)
{
    const BC250_DPAUDIO_RUN* run = &Start->Seq;
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
        GuardLog("dpaudio: configure done on endpoint %lu: FL/FR, DP, LPCM 2 ch 32/44.1/48 kHz 16 bit, BC-250 DP, %lu writes",
                 run->Plan.Endpoint, run->Config.Writes);
    if (run->Groups >= 3)
        GuardLog("dpaudio: AUDIO_ENABLED set on endpoint %lu: hpc 0x%08lX -> 0x%08lX", run->Plan.Endpoint,
                 run->Enable.HotPlugBefore, run->Enable.HotPlugAfter);
    else
        GuardLog("dpaudio: write failed in %s (0x%08lX); AUDIO_ENABLED cleared: hpc 0x%08lX (0x%08lX)",
                 g_GroupName[run->Groups], (ULONG)run->Status, run->Clear.HotPlugAfter, (ULONG)run->ClearStatus);
}

static void StartCore(_Inout_ BC250_DEVICE* Device, BOOLEAN Resume)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    DPAUDIO_START* start;
    BC250_AZ_IO io;
    ULONG enable = GuardReadSetting(DPAUDIO_SETTING_ENABLE, 1);
    ULONG endpointSwitch = GuardReadSetting(DPAUDIO_SETTING_ENDPOINT, 1);
    KIRQL irql;

    // From pool: the observation alone is 270 bytes, and a start already runs deep in dxgkrnl's stack (M104).
    start = (DPAUDIO_START*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*start), BC250_TAG);
    if (start == NULL) {
        GuardLog("dpaudio: %s skipped: no memory for the record; nothing read or written", Resume ? "resume" : "start");
        return;
    }
    IoOpen(Device, &io);
    start->Gate = Bc250DpAudioGate(Device->Mmio != NULL, enable, endpointSwitch);
    KeAcquireSpinLock(&audio->Lock, &irql);
    if (Resume) audio->Resumes++; else audio->Starts++;
    audio->SwitchEnable = enable;
    audio->SwitchEndpoint = endpointSwitch;
    audio->Written = FALSE;
    audio->Notes = 0;
    audio->LastStatus = 0;
    if (start->Gate != BC250_DPAUDIO_REASON_OK) {
        audio->State = BC250_DPAUDIO_STATE_REFUSED;
        audio->Reason = start->Gate;
        audio->Refusals++;
    } else {
        const BC250_DPAUDIO_RUN* run = &start->Seq;

        start->Reason = Bc250DpAudioRun(&io, &start->Seq);
        audio->Reason = start->Reason;
        audio->Notes = run->Plan.Notes;
        audio->Endpoint = run->Plan.Endpoint;
        audio->Stream = run->Plan.Stream;
        audio->CodecId = run->Obs.Regs[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE];
        audio->ConfigDefault = run->Obs.Regs[run->Plan.Endpoint == 0 ? BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT
                                                                      : BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT];
        // From the first write on, the stop path owes the endpoint an AUDIO_ENABLED 0.
        audio->Written = run->Wrote != 0;
        audio->LastStatus = run->Status;
        if (!run->Wrote) {
            audio->State = BC250_DPAUDIO_STATE_REFUSED;
            audio->Refusals++;
        } else if (start->Reason == BC250_DPAUDIO_REASON_OK) {
            audio->State = BC250_DPAUDIO_STATE_ENABLED;
            audio->HotPlugBefore = run->Enable.HotPlugBefore;
            audio->HotPlugAfter = run->Enable.HotPlugAfter;
        } else {
            audio->State = BC250_DPAUDIO_STATE_FAILED;
            audio->Failures++;
            audio->HotPlugBefore = run->Clear.HotPlugBefore;
            audio->HotPlugAfter = run->Clear.HotPlugAfter;
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

// Nothing of the endpoint is assumed to survive D3: the whole start runs again, preconditions included.
void DpAudioResume(_Inout_ BC250_DEVICE* Device)
{
    StartCore(Device, TRUE);
}

void DpAudioStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_RESULT result;
    BC250_AZ_IO io;
    BOOLEAN cleared = FALSE;
    ULONG endpoint, state;
    long status = 0;
    KIRQL irql;

    RtlZeroMemory(&result, sizeof(result));
    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    audio->Stops++;
    endpoint = audio->Endpoint;
    state = audio->State;
    if (audio->Written && Device->Mmio != NULL) {
        status = Bc250DpAudioSetEnabled(&io, endpoint, 0, &result);
        audio->Written = FALSE;
        audio->State = BC250_DPAUDIO_STATE_STOPPED;
        audio->Reason = BC250_DPAUDIO_REASON_STOPPED;
        audio->HotPlugBefore = result.HotPlugBefore;
        audio->HotPlugAfter = result.HotPlugAfter;
        audio->LastStatus = status;
        if (status < 0) audio->Failures++;
        cleared = TRUE;
    }
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    if (cleared)
        GuardLog("dpaudio: stop: AUDIO_ENABLED cleared on endpoint %lu: hpc 0x%08lX -> 0x%08lX (0x%08lX)", endpoint,
                 result.HotPlugBefore, result.HotPlugAfter, (ULONG)status);
    else if (state != BC250_DPAUDIO_STATE_IDLE)
        GuardLog("dpaudio: stop: nothing to clear (state %lu, this start wrote nothing)", state);
}

void DpAudioPathPower(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_RESULT result;
    BC250_AZ_IO io;
    BOOLEAN changed = FALSE, retry;
    ULONG endpoint;
    long status = 0;
    KIRQL irql;

    RtlZeroMemory(&result, sizeof(result));
    IoOpen(Device, &io);
    KeAcquireSpinLock(&audio->Lock, &irql);
    endpoint = audio->Endpoint;
    retry = On && audio->State == BC250_DPAUDIO_STATE_REFUSED && audio->Reason == BC250_DPAUDIO_REASON_NO_STREAM;
    // Only an endpoint this start enabled follows the monitor's power; a refused or stopped one stays as it is.
    if (audio->Written && Device->Mmio != NULL &&
        ((On && audio->State == BC250_DPAUDIO_STATE_PATH_OFF) || (!On && audio->State == BC250_DPAUDIO_STATE_ENABLED))) {
        status = Bc250DpAudioSetEnabled(&io, endpoint, On ? 1 : 0, &result);
        if (On) audio->PathOn++; else audio->PathOff++;
        audio->State = (status >= 0 && On) ? BC250_DPAUDIO_STATE_ENABLED : BC250_DPAUDIO_STATE_PATH_OFF;
        audio->Reason = audio->State == BC250_DPAUDIO_STATE_ENABLED ? BC250_DPAUDIO_REASON_OK : BC250_DPAUDIO_REASON_PATH_OFF;
        audio->HotPlugBefore = result.HotPlugBefore;
        audio->HotPlugAfter = result.HotPlugAfter;
        audio->LastStatus = status;
        if (status < 0) audio->Failures++;
        changed = TRUE;
    }
    IoClose(audio, &io);
    KeReleaseSpinLock(&audio->Lock, irql);
    if (changed)
        GuardLog("dpaudio: path power %s: AUDIO_ENABLED %lu on endpoint %lu: hpc 0x%08lX -> 0x%08lX (0x%08lX)",
                 On ? "on" : "off", On ? 1ul : 0ul, endpoint, result.HotPlugBefore, result.HotPlugAfter, (ULONG)status);
    // A start or resume that found no DP stream (the monitor was off, or D0 came back before the stream did) gets
    // one more full start when dxgkrnl powers the path on: preconditions and all, counted as a resume.
    if (retry) {
        GuardLog("dpaudio: path power on after a refusal for no stream: start again");
        StartCore(Device, TRUE);
    }
}

// BC250_ESCAPE_RUN_DPAUDIO (bc250kmd_escape.h). HardwareAccess only, administrators only, one exact size (display.c).
void DpAudioRequest(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DPAUDIO* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_DPAUDIO* audio = &Device->DpAudio;
    BC250_DPAUDIO_OBSERVATION* obs = NULL;
    ULONG op = Data->Op, abi = Data->AbiVersion, i;
    BC250_AZ_IO io;
    long status = STATUS_SUCCESS;
    KIRQL irql;

    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_RUN_DPAUDIO;
    Data->Version = BC250_KMD_VERSION;
    Data->AbiVersion = BC250_DPAUDIO_ABI;
    Data->Op = op;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    if (!Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    // HardwareAccess requests dxgkrnl's Level Two exclusion of stop and MMIO unmap, as OBSERVE_DCN.
    if (EscapeFlags != 1u || abi != BC250_DPAUDIO_ABI || (op != BC250_DPAUDIO_OP_OBSERVE && op != BC250_DPAUDIO_OP_STATE)) {
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
