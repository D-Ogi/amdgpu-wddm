// DisplayPort audio, steps 0, 1 and 2, the part with no Windows in it (dpaudio_seq.h says what is here and why).
#define BC250_REGS_WITH_AUDIO_TABLES
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"      // field masks only; every offset and index comes from regs.generated.h
#include "dpaudio_seq.h"

// The two endpoints share one register layout. The driver takes every field from the ENDPOINT0 names; these checks
// make the build fail if the ENDPOINT1 names of the fields it touches ever say otherwise.
#define AZ0(reg, field) AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_##reg##__##field
#define AZ1(reg, field) AZF0ENDPOINT1_AZALIA_F0_CODEC_PIN_CONTROL_##reg##__##field
#define AZ_SAME(reg, field) typedef char Bc250AzSame_##reg##_##field[(AZ0(reg, field##_MASK) == AZ1(reg, field##_MASK) && \
                                                                    AZ0(reg, field##__SHIFT) == AZ1(reg, field##__SHIFT)) ? 1 : -1]
AZ_SAME(HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE);
AZ_SAME(HOT_PLUG_CONTROL, AUDIO_ENABLED);
AZ_SAME(CHANNEL_SPEAKER, SPEAKER_ALLOCATION);
AZ_SAME(CHANNEL_SPEAKER, HDMI_CONNECTION);
AZ_SAME(CHANNEL_SPEAKER, DP_CONNECTION);
AZ_SAME(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO);
AZ_SAME(CHANNEL_SPEAKER, LFE_PLAYBACK_LEVEL);
AZ_SAME(RESPONSE_HBR, HBR_CAPABLE);
AZ_SAME(RESPONSE_LIPSYNC, VIDEO_LIPSYNC);
AZ_SAME(RESPONSE_LIPSYNC, AUDIO_LIPSYNC);
AZ_SAME(UNSOLICITED_RESPONSE, ENABLE);

#define HPC_CLOCK_GATING_DISABLE AZ0(HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE_MASK)
#define HPC_AUDIO_ENABLED AZ0(HOT_PLUG_CONTROL, AUDIO_ENABLED_MASK)

static unsigned long SetField(unsigned long Value, unsigned long Mask, unsigned long Shift, unsigned long Field)
{
    return (Value & ~Mask) | ((Field << Shift) & Mask);
}
#define SET(v, reg, field, x) SetField((v), AZ0(reg, field##_MASK), AZ0(reg, field##__SHIFT), (x))

static int InTable(const unsigned long* Table, unsigned long Count, unsigned long Value)
{
    unsigned long low = 0, high = Count;

    while (low < high) {
        unsigned long middle = low + (high - low) / 2;
        if (Table[middle] == Value) return 1;
        if (Table[middle] < Value) low = middle + 1; else high = middle;
    }
    return 0;
}

// ---- the checked accessors ---------------------------------------------------------------------------------------

static const unsigned long g_EndpointIndex[BC250_DPAUDIO_ENDPOINTS] = {
    BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX };
static const unsigned long g_EndpointData[BC250_DPAUDIO_ENDPOINTS] = {
    BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA, BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_DATA };

int Bc250AzReadAllowed(unsigned long Offset)
{
    return (Offset & 3) == 0 && InTable(g_MmioAudioAllow, BC250_MMIO_AUDIO_ALLOW_COUNT, Offset);
}

int Bc250AzWriteAllowed(unsigned long Offset)
{
    return (Offset & 3) == 0 && InTable(g_MmioAudioWriteAllow, BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT, Offset);
}

long Bc250AzRead(BC250_AZ_IO* Io, unsigned long Offset, unsigned long* Value)
{
    *Value = 0;
    if (!Bc250AzReadAllowed(Offset)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    return Io->Read(Io->Context, Offset, Value);
}

// The one place a register write leaves this file. Counted by the caller, which knows what the write was for.
static long CheckedWrite(BC250_AZ_IO* Io, unsigned long Offset, unsigned long Value)
{
    if (!Bc250AzWriteAllowed(Offset)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    return Io->Write(Io->Context, Offset, Value);
}

long Bc250AzWrite(BC250_AZ_IO* Io, unsigned long Offset, unsigned long Value)
{
    long status = CheckedWrite(Io, Offset, Value);

    if (status >= 0) Io->DirectWrites++;
    return status;
}

// write_indirect_azalia_reg / read_indirect_azalia_reg (dce_audio.c:55-84): the index into the endpoint's INDEX
// register (AZALIA_ENDPOINT_REG_INDEX, mask 0x3FFF; every index on the tables fits), then the DATA register. The
// pair is not atomic in hardware: dpaudio.c holds its spin lock across every call of this file.
long Bc250AzIndirectRead(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long* Value)
{
    long status;

    *Value = 0;
    if (Endpoint >= BC250_DPAUDIO_ENDPOINTS) { Io->Refusals++; return BC250_AZ_STATUS_INVALID_PARAMETER; }
    if (!InTable(g_AzIxReadAllow, BC250_AZ_IX_READ_ALLOW_COUNT, Index)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    status = CheckedWrite(Io, g_EndpointIndex[Endpoint],
                          Index & AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX__AZALIA_ENDPOINT_REG_INDEX_MASK);
    if (status < 0) return status;
    status = Bc250AzRead(Io, g_EndpointData[Endpoint], Value);
    if (status >= 0) Io->IndirectReads++;
    return status;
}

long Bc250AzIndirectWrite(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long Value)
{
    long status;

    if (Endpoint >= BC250_DPAUDIO_ENDPOINTS) { Io->Refusals++; return BC250_AZ_STATUS_INVALID_PARAMETER; }
    if (!InTable(g_AzIxWriteAllow, BC250_AZ_IX_WRITE_ALLOW_COUNT, Index)) { Io->Refusals++; return BC250_AZ_STATUS_ACCESS_DENIED; }
    status = CheckedWrite(Io, g_EndpointIndex[Endpoint],
                          Index & AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX__AZALIA_ENDPOINT_REG_INDEX_MASK);
    if (status < 0) return status;
    status = CheckedWrite(Io, g_EndpointData[Endpoint], Value);
    if (status >= 0) Io->IndirectWrites++;
    return status;
}

// ---- step 0: the observation -------------------------------------------------------------------------------------

#define SLOT_DIRECT 1u
#define SLOT_INDIRECT 2u
typedef struct _OBS_SLOT {
    unsigned long Kind;                     // 0 = not bound: a slot the table below forgot, refused at run time
    unsigned long Endpoint;                 // SLOT_INDIRECT only
    unsigned long Where;                    // BAR5 offset or indirect index
} OBS_SLOT;
#define D(slot, reg) [BC250_DPAUDIO_OBS_##slot] = { SLOT_DIRECT, 0, BC250_REG_DMU_##reg }
#define I(slot, e, ix) [BC250_DPAUDIO_OBS_##slot] = { SLOT_INDIRECT, e, BC250_AZ_IX_##ix }
static const OBS_SLOT g_ObsSlots[BC250_DPAUDIO_OBS_COUNT] = {
    D(CODEC_VENDOR_DEVICE, AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID),
    D(CODEC_REVISION, AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID),
    D(SUPPORTED_SIZE_RATES, AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES),
    D(STREAM_FORMATS, AZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS),
    D(POWER_STATES, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES),
    D(DC_PINSTRAPS, DC_PINSTRAPS),
    D(DTO_SOURCE, DCCG_AUDIO_DTO_SOURCE), D(DTO0_PHASE, DCCG_AUDIO_DTO0_PHASE), D(DTO0_MODULE, DCCG_AUDIO_DTO0_MODULE),
    D(DTO1_PHASE, DCCG_AUDIO_DTO1_PHASE), D(DTO1_MODULE, DCCG_AUDIO_DTO1_MODULE),
    D(DIG0_FE_CNTL, DIG0_DIG_FE_CNTL), D(DIG0_BE_CNTL, DIG0_DIG_BE_CNTL), D(DP0_VID_STREAM_CNTL, DP0_DP_VID_STREAM_CNTL),
    D(DP0_SEC_CNTL, DP0_DP_SEC_CNTL), D(DP0_SEC_AUD_N, DP0_DP_SEC_AUD_N), D(DP0_SEC_AUD_M_READBACK, DP0_DP_SEC_AUD_M_READBACK),
    D(DP0_SEC_TIMESTAMP, DP0_DP_SEC_TIMESTAMP), D(DIG0_AFMT_CNTL, DIG0_AFMT_CNTL),
    D(DIG0_AFMT_AUDIO_SRC_CONTROL, DIG0_AFMT_AUDIO_SRC_CONTROL), D(DIG0_AFMT_AUDIO_PACKET_CONTROL, DIG0_AFMT_AUDIO_PACKET_CONTROL),
    D(DIG0_AFMT_AUDIO_PACKET_CONTROL2, DIG0_AFMT_AUDIO_PACKET_CONTROL2), D(DIG0_AFMT_STATUS, DIG0_AFMT_STATUS),
    D(HPD0_INT_STATUS, HPD0_DC_HPD_INT_STATUS),
    D(DIG1_FE_CNTL, DIG1_DIG_FE_CNTL), D(DIG1_BE_CNTL, DIG1_DIG_BE_CNTL), D(DP1_VID_STREAM_CNTL, DP1_DP_VID_STREAM_CNTL),
    D(DP1_SEC_CNTL, DP1_DP_SEC_CNTL), D(DP1_SEC_AUD_N, DP1_DP_SEC_AUD_N), D(DP1_SEC_AUD_M_READBACK, DP1_DP_SEC_AUD_M_READBACK),
    D(DP1_SEC_TIMESTAMP, DP1_DP_SEC_TIMESTAMP), D(DIG1_AFMT_CNTL, DIG1_AFMT_CNTL),
    D(DIG1_AFMT_AUDIO_SRC_CONTROL, DIG1_AFMT_AUDIO_SRC_CONTROL), D(DIG1_AFMT_AUDIO_PACKET_CONTROL, DIG1_AFMT_AUDIO_PACKET_CONTROL),
    D(DIG1_AFMT_AUDIO_PACKET_CONTROL2, DIG1_AFMT_AUDIO_PACKET_CONTROL2), D(DIG1_AFMT_STATUS, DIG1_AFMT_STATUS),
    D(HPD1_INT_STATUS, HPD1_DC_HPD_INT_STATUS),
    I(EP0_CONFIG_DEFAULT, 0, RESPONSE_CONFIGURATION_DEFAULT), I(EP0_HOT_PLUG_CONTROL, 0, HOT_PLUG_CONTROL),
    I(EP0_PIN_SENSE, 0, RESPONSE_PIN_SENSE), I(EP0_UNSOLICITED_RESPONSE, 0, UNSOLICITED_RESPONSE),
    I(EP0_WIDGET_CONTROL, 0, WIDGET_CONTROL), I(EP0_CHANNEL_SPEAKER, 0, CHANNEL_SPEAKER),
    I(EP0_AUDIO_DESCRIPTOR0, 0, AUDIO_DESCRIPTOR0), I(EP0_SINK_INFO1, 0, SINK_INFO1),
    I(EP1_CONFIG_DEFAULT, 1, RESPONSE_CONFIGURATION_DEFAULT), I(EP1_HOT_PLUG_CONTROL, 1, HOT_PLUG_CONTROL),
    I(EP1_PIN_SENSE, 1, RESPONSE_PIN_SENSE), I(EP1_UNSOLICITED_RESPONSE, 1, UNSOLICITED_RESPONSE),
    I(EP1_WIDGET_CONTROL, 1, WIDGET_CONTROL), I(EP1_CHANNEL_SPEAKER, 1, CHANNEL_SPEAKER),
    I(EP1_AUDIO_DESCRIPTOR0, 1, AUDIO_DESCRIPTOR0), I(EP1_SINK_INFO1, 1, SINK_INFO1),
    [BC250_DPAUDIO_OBS_REFCLK_COUNT] = { SLOT_DIRECT, 0, BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT },
    D(DIG0_AFMT_INFOFRAME_CONTROL0, DIG0_AFMT_INFOFRAME_CONTROL0), D(DIG0_AFMT_60958_0, DIG0_AFMT_60958_0),
    D(DIG1_AFMT_INFOFRAME_CONTROL0, DIG1_AFMT_INFOFRAME_CONTROL0), D(DIG1_AFMT_60958_0, DIG1_AFMT_60958_0),
};
#undef D
#undef I

void Bc250DpAudioObserve(BC250_AZ_IO* Io, BC250_DPAUDIO_OBSERVATION* Obs)
{
    unsigned long i;

    for (i = 0; i < BC250_DPAUDIO_OBS_SLOTS; i++) Obs->Regs[i] = 0;
    Obs->ValidMask = 0;
    Obs->FirstFailure = BC250_AZ_STATUS_SUCCESS;
    for (i = 0; i < BC250_DPAUDIO_OBS_COUNT; i++) {
        const OBS_SLOT* slot = &g_ObsSlots[i];
        unsigned long value = 0;
        long status;

        if (slot->Kind == SLOT_DIRECT) status = Bc250AzRead(Io, slot->Where, &value);
        else if (slot->Kind == SLOT_INDIRECT) status = Bc250AzIndirectRead(Io, slot->Endpoint, slot->Where, &value);
        else status = BC250_AZ_STATUS_INVALID_PARAMETER;
        if (status >= 0) {
            Obs->Regs[i] = value;
            Obs->ValidMask |= 1ull << i;
        } else if (Obs->FirstFailure >= 0) {
            Obs->FirstFailure = status;
        }
    }
}

// ---- steps 1 and 2: the decision ---------------------------------------------------------------------------------

unsigned long Bc250DpAudioGate(int MmioMapped, unsigned long EnableDpAudio, unsigned long EnableDpAudioEndpoint,
                               unsigned long EnableDpAudioStream)
{
    if (EnableDpAudio != 1) return BC250_DPAUDIO_REASON_SWITCH_OFF;
    if (EnableDpAudioEndpoint != 1) return BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF;
    if (EnableDpAudioStream != 1) return BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF;
    if (!MmioMapped) return BC250_DPAUDIO_REASON_NO_MMIO;
    return BC250_DPAUDIO_REASON_OK;
}

#define BIT(slot) (1ull << BC250_DPAUDIO_OBS_##slot)
#define VALID(o, slot) (((o)->ValidMask & BIT(slot)) != 0)

static const unsigned long g_VidStreamSlot[2] = { BC250_DPAUDIO_OBS_DP0_VID_STREAM_CNTL, BC250_DPAUDIO_OBS_DP1_VID_STREAM_CNTL };
static const unsigned long g_VidStreamEnable[2] = { DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
                                                    DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK };
static const unsigned long g_BeSlot[2] = { BC250_DPAUDIO_OBS_DIG0_BE_CNTL, BC250_DPAUDIO_OBS_DIG1_BE_CNTL };
static const unsigned long g_BeModeMask[2] = { DIG0_DIG_BE_CNTL__DIG_MODE_MASK, DIG1_DIG_BE_CNTL__DIG_MODE_MASK };
static const unsigned long g_BeModeShift[2] = { DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT, DIG1_DIG_BE_CNTL__DIG_MODE__SHIFT };
static const unsigned long g_BeFeMask[2] = { DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK };
static const unsigned long g_BeFeShift[2] = { DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT, DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT };
static const unsigned long g_HpdSlot[2] = { BC250_DPAUDIO_OBS_HPD0_INT_STATUS, BC250_DPAUDIO_OBS_HPD1_INT_STATUS };
static const unsigned long g_HpdSense[2] = { HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK, HPD1_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK };
static const unsigned long g_ConfigSlot[2] = { BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT, BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT };
static const unsigned long g_HotPlugSlot[2] = { BC250_DPAUDIO_OBS_EP0_HOT_PLUG_CONTROL, BC250_DPAUDIO_OBS_EP1_HOT_PLUG_CONTROL };
static const unsigned long g_UnsolSlot[2] = { BC250_DPAUDIO_OBS_EP0_UNSOLICITED_RESPONSE, BC250_DPAUDIO_OBS_EP1_UNSOLICITED_RESPONSE };
// DIG_MODE 0 is DP SST, 5 DP MST (dcn10_link_encoder.c:880-907 dcn10_link_encoder_setup). Endpoint presence is
// written for an SST stream only: MST would need one endpoint per stream and a different speaker allocation path.
#define DIG_MODE_DP_SST 0u

unsigned long Bc250DpAudioDecide(const BC250_DPAUDIO_OBSERVATION* Obs, BC250_DPAUDIO_PLAN* Plan)
{
    const unsigned long long required = BIT(CODEC_VENDOR_DEVICE) | BIT(DC_PINSTRAPS) | BIT(DP0_VID_STREAM_CNTL) |
        BIT(DP1_VID_STREAM_CNTL) | BIT(DIG0_BE_CNTL) | BIT(DIG1_BE_CNTL) | BIT(EP0_CONFIG_DEFAULT) |
        BIT(EP1_CONFIG_DEFAULT) | BIT(EP0_HOT_PLUG_CONTROL) | BIT(EP1_HOT_PLUG_CONTROL) | BIT(REFCLK_COUNT);
    unsigned long n, m, streams = 0, stream = 0, e, refclk;
    int sst = 0;

    Plan->Reason = BC250_DPAUDIO_REASON_OK;
    Plan->Stream = Plan->Endpoint = 0;
    Plan->Notes = 0;
    Plan->RefClock = Plan->DtoModule = 0;
    if ((Obs->ValidMask & required) != required) return Plan->Reason = BC250_DPAUDIO_REASON_READ_FAILED;
    if (Obs->Regs[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE] != BC250_DPAUDIO_CODEC_ID)
        return Plan->Reason = BC250_DPAUDIO_REASON_CODEC_ID;
    // dcn201_resource.c:840-845: DC_PINSTRAPS_AUDIO 0 means no audio endpoint at all (audio_support.bits.DP_AUDIO).
    if ((Obs->Regs[BC250_DPAUDIO_OBS_DC_PINSTRAPS] & DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK) == 0)
        return Plan->Reason = BC250_DPAUDIO_REASON_STRAPS;
    // Which stream encoder drives the monitor. The firmware enabled exactly one DP stream; the audio endpoint that
    // belongs to it is the one with the encoder's engine id (dc_resource.c:3530-3563, audio instance = engine id;
    // Linux on unit A: DIG0, DP0, endpoint 0, M820).
    for (n = 0; n < 2; n++)
        if (Obs->Regs[g_VidStreamSlot[n]] & g_VidStreamEnable[n]) { streams++; stream = n; }
    if (streams == 0) return Plan->Reason = BC250_DPAUDIO_REASON_NO_STREAM;
    if (streams > 1) return Plan->Reason = BC250_DPAUDIO_REASON_TWO_STREAMS;
    Plan->Stream = stream;
    Plan->Endpoint = e = stream;
    // The back end that carries that stream encoder, in DP SST mode. DIG_FE_SOURCE_SELECT is a bit mask of front
    // ends, DIGA = 0x1 for engine 0 (dcn10_link_encoder.c:54-61 and get_frontend_source :425-440).
    for (m = 0; m < 2; m++) {
        unsigned long be = Obs->Regs[g_BeSlot[m]];
        if ((((be & g_BeFeMask[m]) >> g_BeFeShift[m]) & (1ul << stream)) != 0 &&
            ((be & g_BeModeMask[m]) >> g_BeModeShift[m]) == DIG_MODE_DP_SST) sst = 1;
    }
    if (!sst) return Plan->Reason = BC250_DPAUDIO_REASON_NOT_DP_SST;
    // The endpoint must be the pin Linux saw (M819: both pins report 0x185600F0, port connectivity 0 = jack).
    // dce_aud_endpoint_valid (dce_audio.c:1244-1256) only asks PORT_CONNECTIVITY != 1; this asks for the whole
    // value, because a different value means a different endpoint layout or a wrong index table, and the first
    // write must not land on either. Endpoint 0 is checked as well: hw_init writes it whatever e is.
    if (Obs->Regs[g_ConfigSlot[e]] != BC250_DPAUDIO_CONFIG_DEFAULT ||
        Obs->Regs[g_ConfigSlot[0]] != BC250_DPAUDIO_CONFIG_DEFAULT)
        return Plan->Reason = BC250_DPAUDIO_REASON_CONFIG_DEFAULT;
    // Step 2's own precondition: the DP reference clock the wall DTO divides. Linux takes the DTO source clock from
    // its clock manager (dp_dto_source_clock_in_khz, adjusted for the VBIOS spread-spectrum figure); on unit A that
    // gives 598 874 kHz and a DTO1 module of 5 988 740 (M820). This driver takes the clock the hardware counts
    // instead: 600.000 MHz under Windows (M788), and the board does not apply that spread-spectrum reduction (M788,
    // the community board with the same count). With Linux's module the wall clock would be 600 000 x 240 000 /
    // 5 988 740 = 24.045 MHz, 0.19 % fast, a pitch and drift error; with the counted clock it is 24.000 MHz.
    // Module = count (100 kHz) x 100 = kHz, x 10 as get_azalia_clock_info_dp: 6000 -> 6 000 000.
    refclk = Obs->Regs[BC250_DPAUDIO_OBS_REFCLK_COUNT];
    Plan->RefClock = refclk;
    if (refclk < BC250_DPAUDIO_REFCLK_MIN || refclk > BC250_DPAUDIO_REFCLK_MAX)
        return Plan->Reason = BC250_DPAUDIO_REASON_REFCLK;
    Plan->DtoModule = refclk * 1000ul;
    if (VALID(Obs, HPD0_INT_STATUS) && stream == 0 && !(Obs->Regs[g_HpdSlot[0]] & g_HpdSense[0]))
        Plan->Notes |= BC250_DPAUDIO_NOTE_HPD_LOW;
    if (VALID(Obs, HPD1_INT_STATUS) && stream == 1 && !(Obs->Regs[g_HpdSlot[1]] & g_HpdSense[1]))
        Plan->Notes |= BC250_DPAUDIO_NOTE_HPD_LOW;
    if (Obs->Regs[g_HotPlugSlot[e]] & HPC_AUDIO_ENABLED) Plan->Notes |= BC250_DPAUDIO_NOTE_INHERITED;
    if (VALID(Obs, CODEC_REVISION) && Obs->Regs[BC250_DPAUDIO_OBS_CODEC_REVISION] != BC250_DPAUDIO_CODEC_REVISION)
        Plan->Notes |= BC250_DPAUDIO_NOTE_REVISION;
    if ((Obs->ValidMask & (1ull << g_UnsolSlot[e])) && (Obs->Regs[g_UnsolSlot[e]] & AZ0(UNSOLICITED_RESPONSE, ENABLE_MASK)))
        Plan->Notes |= BC250_DPAUDIO_NOTE_UNSOLICITED;
    return Plan->Reason;
}

// ---- step 1: the write groups ------------------------------------------------------------------------------------

#define TRY(x) do { status = (x); if (status < 0) return status; } while (0)
#define IR(ix, v) TRY(Bc250AzIndirectRead(Io, Endpoint, BC250_AZ_IX_##ix, (v)))
#define IW(ix, v) do { TRY(Bc250AzIndirectWrite(Io, Endpoint, BC250_AZ_IX_##ix, (v))); Result->Writes++; } while (0)

// dce_aud_hw_init (dce_audio.c:1260-1295), endpoint 0 only, as Linux (`if (audio->inst != 0) return`):
// rates 32/44.1/48 kHz into the function group, CLKSTOP and EPSS on, inside a CLOCK_GATING_DISABLE bracket.
long Bc250DpAudioHwInit(BC250_AZ_IO* Io, BC250_DPAUDIO_RESULT* Result)
{
    const unsigned long Endpoint = 0;
    unsigned long hpc, value;
    long status;

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    hpc |= HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    // R5, R6, R7 (Linux's comment): 32, 44.1 and 48 kHz.
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, &value));
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES__AUDIO_RATE_CAPABILITIES_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES__AUDIO_RATE_CAPABILITIES__SHIFT, 0x70);
    TRY(Bc250AzWrite(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, value));
    Result->Writes++;
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, &value));
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__CLKSTOP_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__CLKSTOP__SHIFT, 1);
    value = SetField(value, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__EPSS_MASK,
                     AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__EPSS__SHIFT, 1);
    TRY(Bc250AzWrite(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, value));
    Result->Writes++;
    hpc &= ~HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    Result->HotPlugAfter = hpc;
    // Not in Linux: the two read-backs the log line and the escape report.
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, &Result->SizeRates));
    TRY(Bc250AzRead(Io, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, &Result->PowerStates));
    return BC250_AZ_STATUS_SUCCESS;
}

// The fixed "basic audio" set of step 1, for a start with no usable EDID (step 4 below): what every DP sink with
// audio must accept (2-channel LPCM at 32, 44.1 and 48 kHz, 16 bit). Linux's values for unit A's monitor (M810: 32 to
// 192 kHz, 16/20/24 bit) are wider; this set is a subset of them.
#define LPCM_CHANNELS 2u
#define LPCM_RATES 0x07u                    // union audio_sample_rates (dc_types.h:437-449): RATE_32, RATE_44_1, RATE_48
#define LPCM_SIZES 0x01u                    // the CTA-861 SAD byte 3 of LPCM: bit 0 = 16 bit (amdgpu_dm copies it as is)
#define SPEAKERS_FL_FR 0x01u                // struct audio_speaker_flags (dc_types.h:451-): FL_FR is bit 0
// The two halves of the port ID. Linux DC writes these constants when no Windows DM supplies a container ID
// (dc_stream.c:98-109); step 4 replaces them with DXGK_CHILD_CONTAINER_ID.EldInfo.PortId.
#define SINK_PORT_ID0 0x5558859eul
#define SINK_PORT_ID1 0x0d989449ul
static const char g_SinkName[] = "BC-250 DP";
#define SINK_NAME_MAX 18u                   // MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS (audio_types.h:34)

static const unsigned long g_Descriptor[14] = {
    BC250_AZ_IX_AUDIO_DESCRIPTOR0, BC250_AZ_IX_AUDIO_DESCRIPTOR1, BC250_AZ_IX_AUDIO_DESCRIPTOR2, BC250_AZ_IX_AUDIO_DESCRIPTOR3,
    BC250_AZ_IX_AUDIO_DESCRIPTOR4, BC250_AZ_IX_AUDIO_DESCRIPTOR5, BC250_AZ_IX_AUDIO_DESCRIPTOR6, BC250_AZ_IX_AUDIO_DESCRIPTOR7,
    BC250_AZ_IX_AUDIO_DESCRIPTOR8, BC250_AZ_IX_AUDIO_DESCRIPTOR9, BC250_AZ_IX_AUDIO_DESCRIPTOR10, BC250_AZ_IX_AUDIO_DESCRIPTOR11,
    BC250_AZ_IX_AUDIO_DESCRIPTOR12, BC250_AZ_IX_AUDIO_DESCRIPTOR13 };
// enum audio_format_code (dc_types.h:479-499): descriptor i is format code i + 1. 1BITAUDIO (9) and DST (13) are
// skipped, not zeroed, as dce_aud_az_configure skips them.
#define FORMAT_LPCM 1u
#define FORMAT_1BITAUDIO 9u
#define FORMAT_DST 13u

void Bc250DpAudioSinkDefault(BC250_DPAUDIO_SINK* Sink)
{
    unsigned long i;

    Sink->Manufacturer = Sink->Product = 0;     // unknown without an EDID
    for (i = 0; i < sizeof(Sink->Name); i++) Sink->Name[i] = i < sizeof(g_SinkName) ? g_SinkName[i] : '\0';
    Sink->Name[BC250_DPAUDIO_SINK_NAME_MAX] = '\0';
    Sink->LpcmChannels = LPCM_CHANNELS;
    Sink->LpcmRates = LPCM_RATES;
    Sink->LpcmSizes = LPCM_SIZES;
    Sink->Speakers = SPEAKERS_FL_FR;
    Sink->FromEdid = 0;
}

// dm_helpers_parse_edid_caps (amdgpu_dm_helpers.c:104-173) and is_audio_format_supported (dce_audio.c): the identity
// in Linux's byte order, the monitor name, the LPCM descriptor with the most channels, the first speaker allocation
// byte or DEFAULT_SPEAKER_LOCATION. HBR stays off whatever the descriptors say (Bc250DpAudioConfigureSink).
int Bc250DpAudioSinkFromEdid(const BC250_EDID_INFO* Info, BC250_DPAUDIO_SINK* Sink)
{
    unsigned long i, best = BC250_EDID_MAX_SADS;

    Bc250DpAudioSinkDefault(Sink);
    if (Info == 0 || (Info->Reason != BC250_EDID_OK && Info->Reason != BC250_EDID_EXTENSION_DROPPED)) return 0;
    for (i = 0; i < Info->SadCount && i < BC250_EDID_MAX_SADS; i++) {
        if (Info->Sads[i].Format != FORMAT_LPCM || Info->Sads[i].Channels == 0) continue;
        if (best == BC250_EDID_MAX_SADS || Info->Sads[i].Channels > Info->Sads[best].Channels) best = i;
    }
    if (best == BC250_EDID_MAX_SADS) return 0;
    Sink->Manufacturer = (unsigned long)Info->ManufacturerId[0] | ((unsigned long)Info->ManufacturerId[1] << 8);
    Sink->Product = Info->ProductCode & 0xFFFFul;
    for (i = 0; i < sizeof(Sink->Name); i++) Sink->Name[i] = '\0';
    for (i = 0; Info->HasName && i < BC250_EDID_NAME_CHARS && i < BC250_DPAUDIO_SINK_NAME_MAX && Info->Name[i]; i++)
        Sink->Name[i] = Info->Name[i];
    Sink->LpcmChannels = Info->Sads[best].Channels > BC250_DPAUDIO_MAX_CHANNELS ? BC250_DPAUDIO_MAX_CHANNELS
                                                                                 : Info->Sads[best].Channels;
    Sink->LpcmRates = Info->Sads[best].Rates & 0x7Ful;
    Sink->LpcmSizes = Info->Sads[best].Byte2 & 0x07ul;
    Sink->Speakers = Info->HasSpeaker ? Info->Speaker : BC250_DPAUDIO_DEFAULT_SPEAKERS;
    Sink->FromEdid = 1;
    return 1;
}

static unsigned long NameChar(const BC250_DPAUDIO_SINK* Sink, unsigned long i)
{
    return i < BC250_DPAUDIO_SINK_NAME_MAX ? (unsigned long)(unsigned char)Sink->Name[i] : 0;
}

// dce_aud_az_configure (dce_audio.c:663-1022) for SIGNAL_TYPE_DISPLAY_PORT with a sink (step 4) or the fixed set.
// Deviations from Linux, each for the reason given:
//   - ACP_DATA (SUPPORTS_AI) is not written: the DCN 2.0.1 header has no index for it (Linux reaches it through
//     the DCE 11 table, index 0x27). Its value would be 0 here anyway: amdgpu_dm sets SUPPORT_AI only for HDMI.
//   - check_audio_bandwidth is not run. It removes no rate here: for SST with 8b/10b coding, which is the only DP
//     link of unit A, check_audio_bandwidth_dp returns at once (dce_audio.c:480-482), so the descriptor carries
//     the sink's rates as they are. Linux therefore writes HBR_CAPABLE 1 on this link; this driver writes 0, the
//     value Linux writes when the 192 kHz 8-channel check fails, because the stream half (step 2) programs no
//     high-bit-rate packets.
//   - Only LPCM is described; the other formats' descriptors are written 0 (no compressed audio over this path).
long Bc250DpAudioConfigure(BC250_AZ_IO* Io, unsigned long Endpoint, BC250_DPAUDIO_RESULT* Result)
{
    return Bc250DpAudioConfigureSink(Io, Endpoint, 0, Result);
}

long Bc250DpAudioConfigureSink(BC250_AZ_IO* Io, unsigned long Endpoint, const BC250_DPAUDIO_SINK* Sink,
                               BC250_DPAUDIO_RESULT* Result)
{
    BC250_DPAUDIO_SINK fixed;
    unsigned long hpc, value, i, length;
    long status;

    if (Sink == 0) {
        Bc250DpAudioSinkDefault(&fixed);
        Sink = &fixed;
    }

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    IW(HOT_PLUG_CONTROL, hpc | HPC_CLOCK_GATING_DISABLE);
    // Speaker allocation and connection type, in Linux's order of field updates.
    IR(CHANNEL_SPEAKER, &value);
    value = SET(value, CHANNEL_SPEAKER, SPEAKER_ALLOCATION, Sink->Speakers);
    value = SET(value, CHANNEL_SPEAKER, LFE_PLAYBACK_LEVEL, 0);
    value = SET(value, CHANNEL_SPEAKER, HDMI_CONNECTION, 0);
    value = SET(value, CHANNEL_SPEAKER, DP_CONNECTION, 0);
    value = SET(value, CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO,
                ((value & AZ0(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO_MASK)) >> AZ0(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO__SHIFT)) & ~1ul);
    value = SET(value, CHANNEL_SPEAKER, DP_CONNECTION, 1);
    IW(CHANNEL_SPEAKER, value);
    // One descriptor per format code; only LPCM is supported.
    for (i = 0; i < 14; i++) {
        unsigned long code = i + 1;
        if (code == FORMAT_1BITAUDIO || code == FORMAT_DST) continue;
        value = 0;
        if (code == FORMAT_LPCM) {
            value = SET(value, AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES_STEREO, Sink->LpcmRates);
            value = SET(value, AUDIO_DESCRIPTOR0, MAX_CHANNELS, Sink->LpcmChannels - 1);
            value = SET(value, AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES, Sink->LpcmRates);
            value = SET(value, AUDIO_DESCRIPTOR0, DESCRIPTOR_BYTE_2, Sink->LpcmSizes);
        }
        TRY(Bc250AzIndirectWrite(Io, Endpoint, g_Descriptor[i], value));
        Result->Writes++;
    }
    // set_high_bit_rate_capable(false), set_video_latency(0), set_audio_latency(0): DP latency is 0 in amdgpu_dm
    // (amdgpu_dm.c:6766, "for DP, video and audio latency should be calculated from DPCD caps").
    IR(RESPONSE_HBR, &value);
    IW(RESPONSE_HBR, SET(value, RESPONSE_HBR, HBR_CAPABLE, 0));
    IR(RESPONSE_LIPSYNC, &value);
    IW(RESPONSE_LIPSYNC, SET(value, RESPONSE_LIPSYNC, VIDEO_LIPSYNC, 0));
    IR(RESPONSE_LIPSYNC, &value);
    IW(RESPONSE_LIPSYNC, SET(value, RESPONSE_LIPSYNC, AUDIO_LIPSYNC, 0));
    // Manufacturer and product from the EDID (step 4); 0 for the fixed set.
    IW(SINK_INFO0, SET(SET(0, SINK_INFO0, MANUFACTURER_ID, Sink->Manufacturer), SINK_INFO0, PRODUCT_ID, Sink->Product));
    // Linux's count (dce_audio.c:886-892) includes the terminator: `while (name[len++] != '\0')` stops one past it,
    // capped at 18. Kept as Linux has it, so that the two drivers report the same length for the same name.
    for (length = 0; length < SINK_NAME_MAX && Sink->Name[length] != '\0'; length++) { }
    length++;                                           // the characters plus the terminator
    if (length > SINK_NAME_MAX) length = SINK_NAME_MAX;
    IW(SINK_INFO1, SET(0, SINK_INFO1, SINK_DESCRIPTION_LEN, length));
    IW(SINK_INFO2, SET(0, SINK_INFO2, PORT_ID0, SINK_PORT_ID0));
    IW(SINK_INFO3, SET(0, SINK_INFO3, PORT_ID1, SINK_PORT_ID1));
    IW(SINK_INFO4, SET(SET(SET(SET(0, SINK_INFO4, DESCRIPTION0, NameChar(Sink, 0)), SINK_INFO4, DESCRIPTION1, NameChar(Sink, 1)),
                           SINK_INFO4, DESCRIPTION2, NameChar(Sink, 2)), SINK_INFO4, DESCRIPTION3, NameChar(Sink, 3)));
    IW(SINK_INFO5, SET(SET(SET(SET(0, SINK_INFO5, DESCRIPTION4, NameChar(Sink, 4)), SINK_INFO5, DESCRIPTION5, NameChar(Sink, 5)),
                           SINK_INFO5, DESCRIPTION6, NameChar(Sink, 6)), SINK_INFO5, DESCRIPTION7, NameChar(Sink, 7)));
    IW(SINK_INFO6, SET(SET(SET(SET(0, SINK_INFO6, DESCRIPTION8, NameChar(Sink, 8)), SINK_INFO6, DESCRIPTION9, NameChar(Sink, 9)),
                           SINK_INFO6, DESCRIPTION10, NameChar(Sink, 10)), SINK_INFO6, DESCRIPTION11, NameChar(Sink, 11)));
    IW(SINK_INFO7, SET(SET(SET(SET(0, SINK_INFO7, DESCRIPTION12, NameChar(Sink, 12)), SINK_INFO7, DESCRIPTION13, NameChar(Sink, 13)),
                           SINK_INFO7, DESCRIPTION14, NameChar(Sink, 14)), SINK_INFO7, DESCRIPTION15, NameChar(Sink, 15)));
    IW(SINK_INFO8, SET(SET(0, SINK_INFO8, DESCRIPTION16, NameChar(Sink, 16)), SINK_INFO8, DESCRIPTION17, NameChar(Sink, 17)));
    IR(HOT_PLUG_CONTROL, &hpc);
    hpc &= ~HPC_CLOCK_GATING_DISABLE;
    IW(HOT_PLUG_CONTROL, hpc);
    Result->HotPlugAfter = hpc;
    return BC250_AZ_STATUS_SUCCESS;
}

// dce_aud_az_enable (dce_audio.c:611-631) and dce_aud_az_disable (:638-661). The read-back at the end is Linux's in
// the disable path and an addition in the enable path, so that both report what the register holds afterwards.
long Bc250DpAudioSetEnabled(BC250_AZ_IO* Io, unsigned long Endpoint, int Enable, BC250_DPAUDIO_RESULT* Result)
{
    unsigned long hpc;
    long status;

    Result->HotPlugBefore = Result->HotPlugAfter = Result->SizeRates = Result->PowerStates = Result->Writes = 0;
    IR(HOT_PLUG_CONTROL, &hpc);
    Result->HotPlugBefore = hpc;
    if (Enable) {
        hpc |= HPC_CLOCK_GATING_DISABLE | HPC_AUDIO_ENABLED;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
    } else {
        hpc |= HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_AUDIO_ENABLED;
        IW(HOT_PLUG_CONTROL, hpc);
        hpc &= ~HPC_CLOCK_GATING_DISABLE;
        IW(HOT_PLUG_CONTROL, hpc);
    }
    IR(HOT_PLUG_CONTROL, &Result->HotPlugAfter);
    return BC250_AZ_STATUS_SUCCESS;
}

// ---- step 2: the stream half ---------------------------------------------------------------------------------------

// The two stream encoders share one register layout. Every field is taken from the DP0/DIG0 names; these checks make
// the build fail if the DP1/DIG1 names of a field this file touches ever say otherwise.
#define SAME(r0, r1, field) typedef char Bc250StreamSame_##r0##_##field[(r0##__##field##_MASK == r1##__##field##_MASK && \
                                                                       r0##__##field##__SHIFT == r1##__##field##__SHIFT) ? 1 : -1]
SAME(DP0_DP_SEC_CNTL, DP1_DP_SEC_CNTL, DP_SEC_STREAM_ENABLE);
SAME(DP0_DP_SEC_CNTL, DP1_DP_SEC_CNTL, DP_SEC_ASP_ENABLE);
SAME(DP0_DP_SEC_CNTL, DP1_DP_SEC_CNTL, DP_SEC_ATP_ENABLE);
SAME(DP0_DP_SEC_CNTL, DP1_DP_SEC_CNTL, DP_SEC_AIP_ENABLE);
SAME(DP0_DP_SEC_CNTL, DP1_DP_SEC_CNTL, DP_SEC_ACM_ENABLE);
SAME(DP0_DP_SEC_AUD_N, DP1_DP_SEC_AUD_N, DP_SEC_AUD_N);
SAME(DP0_DP_SEC_TIMESTAMP, DP1_DP_SEC_TIMESTAMP, DP_SEC_TIMESTAMP_MODE);
SAME(DIG0_AFMT_CNTL, DIG1_AFMT_CNTL, AFMT_AUDIO_CLOCK_EN);
SAME(DIG0_AFMT_AUDIO_SRC_CONTROL, DIG1_AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT);
SAME(DIG0_AFMT_AUDIO_PACKET_CONTROL, DIG1_AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND);
SAME(DIG0_AFMT_AUDIO_PACKET_CONTROL, DIG1_AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE);
SAME(DIG0_AFMT_AUDIO_PACKET_CONTROL2, DIG1_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_LAYOUT_OVRD);
SAME(DIG0_AFMT_AUDIO_PACKET_CONTROL2, DIG1_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE);
SAME(DIG0_AFMT_AUDIO_PACKET_CONTROL2, DIG1_AFMT_AUDIO_PACKET_CONTROL2, AFMT_60958_OSF_OVRD);
SAME(DIG0_AFMT_INFOFRAME_CONTROL0, DIG1_AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE);
SAME(DIG0_AFMT_60958_0, DIG1_AFMT_60958_0, AFMT_60958_CS_CLOCK_ACCURACY);
#undef SAME

#define SEC_STREAM DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK
#define SEC_ASP DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK
#define SEC_ATP DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK
#define SEC_AIP DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK
#define SEC_ACM DP0_DP_SEC_CNTL__DP_SEC_ACM_ENABLE_MASK
#define SEC_AUDIO (SEC_STREAM | SEC_ASP | SEC_ATP | SEC_AIP)
#define PKT_SAMPLE_SEND DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK
#define PKT_CS_UPDATE DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_60958_CS_UPDATE_MASK
#define PKT2_LAYOUT_OVRD DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_LAYOUT_OVRD_MASK
#define PKT2_CHANNELS DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE_MASK
#define PKT2_OSF_OVRD DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_60958_OSF_OVRD_MASK
#define AFMT_CLOCK_EN DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK
#define DTO_SEL DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK
#define DTO_512FBR DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO2_USE_512FBR_DTO_MASK
#define ALL 0xFFFFFFFFul
#define FIELD(reg, field, x) (((unsigned long)(x) << reg##__##field##__SHIFT) & reg##__##field##_MASK)

// Linux's values (dcn10_stream_encoder.c:1055-1056, enc1_se_setup_dp_audio): the default Maud/N of the ATP, and the
// timestamp mode in which the encoder computes the audio timestamps itself.
#define AUD_N_DEFAULT 0x8000ul
#define TIMESTAMP_AUTO_CALC 1ul
// speakers_to_channels (dcn10_stream_encoder.c:1129-1161) for the fixed FL/FR allocation of step 1: FL and FR, the
// two lowest channel bits.
#define CHANNELS_FL_FR 0x03ul

typedef struct _STREAM_REGS {
    unsigned long SecCntl, AudN, Timestamp, AfmtCntl, SrcControl, Packet, Packet2, Infoframe0, Cs0;
} STREAM_REGS;
#define STREAM_REGS_OF(dp, dig) { BC250_REG_DMU_##dp##_DP_SEC_CNTL, BC250_REG_DMU_##dp##_DP_SEC_AUD_N, \
    BC250_REG_DMU_##dp##_DP_SEC_TIMESTAMP, BC250_REG_DMU_##dig##_AFMT_CNTL, BC250_REG_DMU_##dig##_AFMT_AUDIO_SRC_CONTROL, \
    BC250_REG_DMU_##dig##_AFMT_AUDIO_PACKET_CONTROL, BC250_REG_DMU_##dig##_AFMT_AUDIO_PACKET_CONTROL2, \
    BC250_REG_DMU_##dig##_AFMT_INFOFRAME_CONTROL0, BC250_REG_DMU_##dig##_AFMT_60958_0 }
static const STREAM_REGS g_StreamRegs[BC250_DPAUDIO_STREAMS] = { STREAM_REGS_OF(DP0, DIG0), STREAM_REGS_OF(DP1, DIG1) };
#undef STREAM_REGS_OF

static void ClearStream(BC250_DPAUDIO_STREAM_RESULT* Result)
{
    Result->Step = BC250_DPAUDIO_STEP_NONE;
    Result->Status = BC250_AZ_STATUS_SUCCESS;
    Result->MismatchOffset = Result->MismatchExpected = Result->MismatchActual = Result->Writes = 0;
    Result->DtoSource = Result->DtoModule = Result->DtoPhase = Result->AfmtCntl = Result->SrcControl = 0;
    Result->PacketControl = Result->PacketControl2 = Result->SecCntl = Result->AudN = Result->Timestamp = 0;
}

// One step: read, change the bits of Mask only, write, read back. Check holds the named bits whose read-back must
// equal what was written: the fields of the step and of earlier steps on the same register, without the update
// strobes (AFMT_60958_CS_UPDATE, AFMT_AUDIO_INFO_UPDATE), whose read-back nothing documents. ReadBack, when given,
// receives the read-back. The first failure of a sequence is kept in Step, Status and the Mismatch fields.
static long Rmw(BC250_AZ_IO* Io, BC250_DPAUDIO_STREAM_RESULT* Result, unsigned long Step, unsigned long Offset,
                unsigned long Mask, unsigned long Value, unsigned long Check, unsigned long* ReadBack)
{
    unsigned long old = 0, want = 0, got = 0;
    long status = Bc250AzRead(Io, Offset, &old);

    if (status >= 0) {
        want = (old & ~Mask) | (Value & Mask);
        status = Bc250AzWrite(Io, Offset, want);
    }
    if (status >= 0) {
        Result->Writes++;
        status = Bc250AzRead(Io, Offset, &got);
    }
    if (status >= 0) {
        if (ReadBack != 0) *ReadBack = got;
        if (((got ^ want) & Check) != 0) status = BC250_AZ_STATUS_MISMATCH;
    }
    if (status < 0 && Result->Step == BC250_DPAUDIO_STEP_NONE) {
        Result->Step = Step;
        Result->Status = status;
        if (status == BC250_AZ_STATUS_MISMATCH) {
            Result->MismatchOffset = Offset;
            Result->MismatchExpected = want & Check;
            Result->MismatchActual = got & Check;
        }
    }
    return status;
}

#define STEP(step, offset, mask, value, check, readback) \
    do { status = Rmw(Io, Result, BC250_DPAUDIO_STEP_##step, (offset), (mask), (value), (check), (readback)); \
         if (status < 0) return status; } while (0)

// The enable, in this order:
//   0. The AFMT memories on: DIO_MEM_PWR_CTRL 0, the write dcn201_init_hw makes at every Linux init ("power AFMT HDMI
//      memory", dcn201_hwseq.c). This driver inherits the firmware's display and never runs that init, and the
//      firmware leaves every HDMIn_MEM_PWR_FORCE at 3 (0x6DB6D800 on unit A): DP audio then played at 0.33x and
//      silent (r19), where Linux, with 0 here, played at 1.0x and audibly (L1007b). The register holds only memory
//      power controls of the DIO (I2C light sleep, the DPx light-sleep disables, the HDMIn memory power), and Linux
//      writes it whole, so this file does too. The read-back is checked on the two DIG memories' force fields only.
//   1. The wall DTO, dce_aud_wall_dto_setup's DP branch (dce_audio.c:1113-1148) in its order: DTO_SEL 1 (DTO1),
//      DTO1 module and phase, then DTO2_USE_512FBR_DTO 1. DCCG_AUDIO_DTO_SOURCE is shared by every audio stream; only
//      its two named fields change.
//   2. The AFMT audio clock (enc1_se_enable_audio_clock). Linux does not wait for AFMT_AUDIO_CLOCK_ON ("does not
//      work well", dcn10_stream_encoder.c:1376-1384); the read-back records it, the check is on CLOCK_EN alone.
//   3. enc1_se_audio_setup: the source select (Azalia endpoint -> this DIG) and the channel enable.
//   4. enc1_se_setup_dp_audio: AUD_N, the timestamp mode, CS_UPDATE, the layout and OSF overrides 0,
//      AUDIO_INFO_UPDATE, CS_CLOCK_ACCURACY 0. Linux writes AUD_N, the timestamp and the source select whole
//      (REG_SET); this file changes only their named fields, so a bit the firmware set beside them stays.
//   5. enc1_se_enable_dp_audio: ASP, then ATP and AIP, then DP_SEC_STREAM_ENABLE last ("after all the other
//      enables"). DP_SEC_CNTL is live on the main link: only these four bits change, the GSP and MPG bits stay.
//   6. enc1_se_audio_mute_control(false): AFMT_AUDIO_SAMPLE_SEND 1.
// Linux runs 1 at the context apply, 3 at the stream's setup, 2, 4, 5, 6 at enable_audio_packet; the order of the
// writes inside each function is Linux's, and 1 comes before everything as there.
long Bc250DpAudioStreamEnable(BC250_AZ_IO* Io, unsigned long Stream, unsigned long Endpoint, unsigned long DtoModule,
                              BC250_DPAUDIO_STREAM_RESULT* Result)
{
    const STREAM_REGS* r;
    long status;

    ClearStream(Result);
    if (Stream >= BC250_DPAUDIO_STREAMS || Endpoint >= BC250_DPAUDIO_ENDPOINTS) {
        Io->Refusals++;
        Result->Status = BC250_AZ_STATUS_INVALID_PARAMETER;
        return Result->Status;
    }
    r = &g_StreamRegs[Stream];
    STEP(AFMT_MEM_POWER, BC250_REG_DMU_DIO_MEM_PWR_CTRL, ALL, 0,
         DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK | DIO_MEM_PWR_CTRL__HDMI1_MEM_PWR_FORCE_MASK, 0);
    STEP(DTO_SELECT, BC250_REG_DMU_DCCG_AUDIO_DTO_SOURCE, DTO_SEL, FIELD(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL, 1),
         DTO_SEL, &Result->DtoSource);
    STEP(DTO1_MODULE, BC250_REG_DMU_DCCG_AUDIO_DTO1_MODULE, ALL, DtoModule, ALL, &Result->DtoModule);
    STEP(DTO1_PHASE, BC250_REG_DMU_DCCG_AUDIO_DTO1_PHASE, ALL, BC250_DPAUDIO_DTO1_PHASE, ALL, &Result->DtoPhase);
    STEP(DTO_512FBR, BC250_REG_DMU_DCCG_AUDIO_DTO_SOURCE, DTO_512FBR, DTO_512FBR, DTO_SEL | DTO_512FBR, &Result->DtoSource);
    STEP(AFMT_CLOCK_ON, r->AfmtCntl, AFMT_CLOCK_EN, AFMT_CLOCK_EN, AFMT_CLOCK_EN, &Result->AfmtCntl);
    STEP(SRC_SELECT, r->SrcControl, DIG0_AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK,
         FIELD(DIG0_AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT, Endpoint),
         DIG0_AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK, &Result->SrcControl);
    STEP(CHANNEL_ENABLE, r->Packet2, PKT2_CHANNELS, FIELD(DIG0_AFMT_AUDIO_PACKET_CONTROL2, AFMT_AUDIO_CHANNEL_ENABLE,
         CHANNELS_FL_FR), PKT2_CHANNELS, &Result->PacketControl2);
    STEP(AUD_N, r->AudN, DP0_DP_SEC_AUD_N__DP_SEC_AUD_N_MASK, FIELD(DP0_DP_SEC_AUD_N, DP_SEC_AUD_N, AUD_N_DEFAULT),
         DP0_DP_SEC_AUD_N__DP_SEC_AUD_N_MASK, &Result->AudN);
    STEP(TIMESTAMP, r->Timestamp, DP0_DP_SEC_TIMESTAMP__DP_SEC_TIMESTAMP_MODE_MASK,
         FIELD(DP0_DP_SEC_TIMESTAMP, DP_SEC_TIMESTAMP_MODE, TIMESTAMP_AUTO_CALC),
         DP0_DP_SEC_TIMESTAMP__DP_SEC_TIMESTAMP_MODE_MASK, &Result->Timestamp);
    STEP(CS_UPDATE, r->Packet, PKT_CS_UPDATE, PKT_CS_UPDATE, 0, &Result->PacketControl);
    STEP(LAYOUT_OVRD, r->Packet2, PKT2_LAYOUT_OVRD | PKT2_OSF_OVRD, 0, PKT2_LAYOUT_OVRD | PKT2_OSF_OVRD | PKT2_CHANNELS,
         &Result->PacketControl2);
    STEP(INFO_UPDATE, r->Infoframe0, DIG0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK,
         DIG0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK, 0, 0);
    STEP(CLOCK_ACCURACY, r->Cs0, DIG0_AFMT_60958_0__AFMT_60958_CS_CLOCK_ACCURACY_MASK, 0,
         DIG0_AFMT_60958_0__AFMT_60958_CS_CLOCK_ACCURACY_MASK, 0);
    STEP(SEC_ASP_ON, r->SecCntl, SEC_ASP, SEC_ASP, SEC_ASP, &Result->SecCntl);
    STEP(SEC_ATP_AIP_ON, r->SecCntl, SEC_ATP | SEC_AIP, SEC_ATP | SEC_AIP, SEC_ASP | SEC_ATP | SEC_AIP, &Result->SecCntl);
    STEP(SEC_STREAM_ON, r->SecCntl, SEC_STREAM, SEC_STREAM, SEC_AUDIO, &Result->SecCntl);
    STEP(SAMPLE_SEND_ON, r->Packet, PKT_SAMPLE_SEND, PKT_SAMPLE_SEND, PKT_SAMPLE_SEND, &Result->PacketControl);
    return BC250_AZ_STATUS_SUCCESS;
}
#undef STEP

// The disable, the enable's reverse: enc1_se_audio_mute_control(true) first (disable_dio_audio_packet), then
// enc1_se_dp_audio_disable: the DP_SEC audio bits (Linux clears ASP, ATP, AIP, ACM and STREAM_ENABLE in one write;
// here STREAM_ENABLE goes first and alone, then ATP and AIP, then ASP and ACM, the enable order reversed), and
// Linux's rule after it: DP_SEC_CNTL is shared with the info frames, so STREAM_ENABLE goes back to 1 when any other
// bit is still set. Then the AFMT audio clock off. Every step runs whatever an earlier one did, so a failed write
// cannot leave the packets on. The DTO, the source select, the channel enable, AUD_N and the timestamp mode stay as
// the enable left them, as in Linux, whose disable path does not touch them: the DTO is a clock that drives no pin
// and no video, and the rest has no effect while the packets are off.
long Bc250DpAudioStreamDisable(BC250_AZ_IO* Io, unsigned long Stream, BC250_DPAUDIO_STREAM_RESULT* Result)
{
    const STREAM_REGS* r;
    unsigned long sec = 0;

    ClearStream(Result);
    if (Stream >= BC250_DPAUDIO_STREAMS) {
        Io->Refusals++;
        Result->Status = BC250_AZ_STATUS_INVALID_PARAMETER;
        return Result->Status;
    }
    r = &g_StreamRegs[Stream];
    (void)Rmw(Io, Result, BC250_DPAUDIO_STEP_SAMPLE_SEND_OFF, r->Packet, PKT_SAMPLE_SEND, 0, PKT_SAMPLE_SEND,
              &Result->PacketControl);
    (void)Rmw(Io, Result, BC250_DPAUDIO_STEP_SEC_STREAM_OFF, r->SecCntl, SEC_STREAM, 0, SEC_STREAM, &Result->SecCntl);
    (void)Rmw(Io, Result, BC250_DPAUDIO_STEP_SEC_ATP_AIP_OFF, r->SecCntl, SEC_ATP | SEC_AIP, 0, SEC_STREAM | SEC_ATP | SEC_AIP,
              &Result->SecCntl);
    if (Rmw(Io, Result, BC250_DPAUDIO_STEP_SEC_ASP_OFF, r->SecCntl, SEC_ASP | SEC_ACM, 0, SEC_AUDIO | SEC_ACM, &sec) >= 0) {
        Result->SecCntl = sec;
        if (sec != 0)
            (void)Rmw(Io, Result, BC250_DPAUDIO_STEP_SEC_STREAM_KEEP, r->SecCntl, SEC_STREAM, SEC_STREAM, SEC_STREAM,
                      &Result->SecCntl);
    }
    (void)Rmw(Io, Result, BC250_DPAUDIO_STEP_AFMT_CLOCK_OFF, r->AfmtCntl, AFMT_CLOCK_EN, 0, AFMT_CLOCK_EN, &Result->AfmtCntl);
    // Read only: what the DTO source select holds now, for the record.
    if (Bc250AzRead(Io, BC250_REG_DMU_DCCG_AUDIO_DTO_SOURCE, &Result->DtoSource) < 0) Result->DtoSource = 0;
    return Result->Status;
}

long Bc250DpAudioStopSequence(BC250_AZ_IO* Io, unsigned long Stream, unsigned long Endpoint, int StreamWritten,
                              int EndpointWritten, BC250_DPAUDIO_STOP* Stop)
{
    ClearStream(&Stop->Stream);
    Stop->Endpoint.HotPlugBefore = Stop->Endpoint.HotPlugAfter = Stop->Endpoint.SizeRates = 0;
    Stop->Endpoint.PowerStates = Stop->Endpoint.Writes = 0;
    Stop->StreamStatus = Stop->EndpointStatus = BC250_AZ_STATUS_SUCCESS;
    if (StreamWritten) Stop->StreamStatus = Bc250DpAudioStreamDisable(Io, Stream, &Stop->Stream);
    if (EndpointWritten) Stop->EndpointStatus = Bc250DpAudioSetEnabled(Io, Endpoint, 0, &Stop->Endpoint);
    return Stop->StreamStatus < 0 ? Stop->StreamStatus : Stop->EndpointStatus;
}

// The order of the groups. Linux enables the endpoint (az_enable) before the packets (dce110_enable_audio_stream).
// This driver sets AUDIO_ENABLED last instead: AUDIO_ENABLED is what the HD Audio side sees as a plugged sink, and
// the class driver may open a stream at once. With it last, Windows never sees an endpoint whose wall clock and DP
// packets are not running yet, and a failure anywhere before it leaves no endpoint at all. The stop sequence is the
// exact reverse, as Linux's disable is (packets off, then az_disable).
unsigned long Bc250DpAudioRun(BC250_AZ_IO* Io, BC250_DPAUDIO_RUN* Run)
{
    unsigned long reason;
    long status;

    Run->Groups = Run->Wrote = Run->StreamWrote = 0;
    Run->Status = Run->UndoStatus = BC250_AZ_STATUS_SUCCESS;
    Run->Init.Writes = Run->Config.Writes = Run->Enable.Writes = 0;
    ClearStream(&Run->Stream);
    ClearStream(&Run->Undo.Stream);
    Run->Undo.Endpoint.Writes = 0;
    Run->Undo.StreamStatus = Run->Undo.EndpointStatus = BC250_AZ_STATUS_SUCCESS;
    Bc250DpAudioObserve(Io, &Run->Obs);
    reason = Bc250DpAudioDecide(&Run->Obs, &Run->Plan);
    if (reason != BC250_DPAUDIO_REASON_OK) return reason;
    Run->Wrote = 1;
    status = Bc250DpAudioHwInit(Io, &Run->Init);
    if (status >= 0) { Run->Groups = 1; status = Bc250DpAudioConfigureSink(Io, Run->Plan.Endpoint, Run->Sink, &Run->Config); }
    if (status >= 0) {
        Run->Groups = 2;
        Run->StreamWrote = 1;
        status = Bc250DpAudioStreamEnable(Io, Run->Plan.Stream, Run->Plan.Endpoint, Run->Plan.DtoModule, &Run->Stream);
    }
    if (status >= 0) { Run->Groups = 3; status = Bc250DpAudioSetEnabled(Io, Run->Plan.Endpoint, 1, &Run->Enable); }
    if (status >= 0) { Run->Groups = 4; return BC250_DPAUDIO_REASON_OK; }
    // Leave silence behind, whatever group failed: the stream off if its group began, AUDIO_ENABLED 0. A failed
    // AUDIO_ENABLED write turns the stream off as well: packets with no endpoint behind them serve nobody.
    Run->Status = status;
    Run->UndoStatus = Bc250DpAudioStopSequence(Io, Run->Plan.Stream, Run->Plan.Endpoint, (int)Run->StreamWrote, 1, &Run->Undo);
    return status == BC250_AZ_STATUS_MISMATCH ? BC250_DPAUDIO_REASON_STREAM_MISMATCH : BC250_DPAUDIO_REASON_WRITE_FAILED;
}

#define BC250_DPAUDIO_REASON_TEXT(n, t) t,
static const char* const g_ReasonText[BC250_DPAUDIO_REASON_COUNT] = { BC250_DPAUDIO_REASON_LIST(BC250_DPAUDIO_REASON_TEXT) };
#define BC250_DPAUDIO_STEP_TEXT(n) #n,
static const char* const g_StepText[BC250_DPAUDIO_STEP_COUNT] = { BC250_DPAUDIO_STEP_LIST(BC250_DPAUDIO_STEP_TEXT) };

const char* Bc250DpAudioReasonText(unsigned long Reason)
{
    return Reason < BC250_DPAUDIO_REASON_COUNT ? g_ReasonText[Reason] : "unknown reason";
}

const char* Bc250DpAudioStepText(unsigned long Step)
{
    return Step < BC250_DPAUDIO_STEP_COUNT ? g_StepText[Step] : "unknown step";
}
