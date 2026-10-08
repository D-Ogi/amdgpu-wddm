// Host test of DisplayPort audio steps 0, 1, 2 and 4: driver/kmd/dpaudio_seq.c is compiled as it is, the very file
// the miniport links, and driven against a fake register file. Gate "dpaudio" of tools/quality/quick.ps1.
//
// What it has to prove, in the order the risk runs:
//   1. Nothing is written unless every precondition holds. Each refusal gets its own case, and each case checks
//      that the fake saw no write but the INDEX selects of the reads, so AUDIO_ENABLED stays 0.
//   2. The writes, when they happen, are Linux's (dce_audio.c dce_aud_hw_init, dce_aud_az_configure for DP,
//      dce_aud_wall_dto_setup for DP, dce_aud_az_enable, dce_aud_az_disable; dcn10_stream_encoder.c
//      enc1_se_dp_audio_setup, _enable, _disable, enc1_se_audio_mute_control), with the values written out here as
//      numbers, not recomputed with the production macros. The stream half ends with the values unit A read under
//      Linux (M820), except the DTO1 module, which follows the counted reference clock (M788).
//   3. The endpoint is never shown without its stream: AUDIO_ENABLED is the last write of a start, and a failed or
//      mismatched write anywhere before it leaves AUDIO_ENABLED 0 and the stream off.
//   4. Every write of the stream half is a read-modify-write of named bits, read back at once; a read-back that
//      differs undoes everything at once.
//   5. The stop sequence is the enable reversed: SAMPLE_SEND 0, DP_SEC STREAM_ENABLE first off, ATP and AIP, ASP,
//      AFMT clock off, then AUDIO_ENABLED 0; DP_SEC info-frame bits stay, with their master enable.
//   6. Every indirect access selects its index first, on the endpoint it means.
//   7. The tables refuse every other offset and index, so no caller can reach past them.
//   8. Step 4: the sink from an EDID (edid.c parses the lab monitor's redacted EDID) is Linux's audio_info:
//      identity in Linux's byte order, the monitor name, the LPCM descriptor with the most channels capped at 8,
//      the speaker byte or DEFAULT_SPEAKER_LOCATION; an EDID without LPCM, a bad one or none gives the fixed set.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BC250_REGS_WITH_AUDIO_TABLES
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dpaudio_seq.h"
#include "edid_lab_redacted.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define FAKE_FAIL ((long)0xC0000001L)        // STATUS_UNSUCCESSFUL from the fake's injected failures
#define HPC_CGD AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE_MASK
#define HPC_AE AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK

// ---- the fake register file --------------------------------------------------------------------------------------

enum { OP_DIRECT = 1, OP_DATA = 2 };
typedef struct { unsigned long Op, Endpoint, Where, Value; } TRACE;  // a logical write: direct, or DATA at an index

typedef struct {
    unsigned long DirectOffset[128], DirectValue[128], Directs;
    unsigned long Ix[2][0x4000];
    unsigned long Index[2];
    int Armed[2];                           // an INDEX select waits for its DATA access
    unsigned long Protocol;                 // DATA accesses without a fresh INDEX select
    unsigned long Reads, Writes, IndexWrites;
    unsigned long FailWriteAt;              // the n-th Write call (1-based) fails; 0 = none
    unsigned long FailReadOffset;           // a read of this offset fails; 0 = none
    unsigned long StuckOffset, StuckMask;   // a direct write here keeps these bits as they were (a read-back differs)
    unsigned long AudioEnabledWrites;       // HOT_PLUG_CONTROL writes with AUDIO_ENABLED set, either endpoint
    TRACE Trace[256];
    unsigned long Traced;
} FAKE;

static const unsigned long g_Index[2] = { BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX,
                                          BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX };
static const unsigned long g_Data[2] = { BC250_REG_DMU_AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA,
                                         BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_DATA };

static unsigned long* Direct(FAKE* f, unsigned long offset)
{
    unsigned long i;

    for (i = 0; i < f->Directs; i++) if (f->DirectOffset[i] == offset) return &f->DirectValue[i];
    f->DirectOffset[f->Directs] = offset;
    f->DirectValue[f->Directs] = 0;
    return &f->DirectValue[f->Directs++];
}

static void Set(FAKE* f, unsigned long offset, unsigned long value) { *Direct(f, offset) = value; }
static unsigned long Get(FAKE* f, unsigned long offset) { return *Direct(f, offset); }

static long FakeRead(void* context, unsigned long offset, unsigned long* value)
{
    FAKE* f = (FAKE*)context;
    unsigned long e;

    f->Reads++;
    if (f->FailReadOffset != 0 && offset == f->FailReadOffset) return FAKE_FAIL;
    for (e = 0; e < 2; e++) {
        if (offset == g_Index[e]) { *value = f->Index[e]; return 0; }
        if (offset == g_Data[e]) {
            if (!f->Armed[e]) f->Protocol++;
            f->Armed[e] = 0;
            *value = f->Ix[e][f->Index[e]];
            return 0;
        }
    }
    *value = *Direct(f, offset);
    return 0;
}

static long FakeWrite(void* context, unsigned long offset, unsigned long value)
{
    FAKE* f = (FAKE*)context;
    unsigned long e;

    f->Writes++;
    if (f->FailWriteAt != 0 && f->Writes == f->FailWriteAt) return FAKE_FAIL;
    for (e = 0; e < 2; e++) {
        if (offset == g_Index[e]) { f->Index[e] = value & 0x3FFF; f->Armed[e] = 1; f->IndexWrites++; return 0; }
        if (offset == g_Data[e]) {
            if (!f->Armed[e]) f->Protocol++;
            f->Armed[e] = 0;
            f->Ix[e][f->Index[e]] = value;
            if (f->Index[e] == BC250_AZ_IX_HOT_PLUG_CONTROL && (value & HPC_AE)) f->AudioEnabledWrites++;
            f->Trace[f->Traced++] = (TRACE){ OP_DATA, e, f->Index[e], value };
            return 0;
        }
    }
    f->Trace[f->Traced++] = (TRACE){ OP_DIRECT, 0, offset, value };
    if (offset == f->StuckOffset) value = (value & ~f->StuckMask) | (*Direct(f, offset) & f->StuckMask);
    *Direct(f, offset) = value;
    return 0;
}

static void Io(BC250_AZ_IO* io, FAKE* f)
{
    memset(io, 0, sizeof(*io));
    io->Context = f;
    io->Read = FakeRead;
    io->Write = FakeWrite;
}

// The stream registers of encoder n, by name.
typedef struct { unsigned long Sec, AudN, Timestamp, Afmt, Src, Pkt, Pkt2, Info0, Cs0; } SREGS;
static const SREGS g_S[2] = {
    { BC250_REG_DMU_DP0_DP_SEC_CNTL, BC250_REG_DMU_DP0_DP_SEC_AUD_N, BC250_REG_DMU_DP0_DP_SEC_TIMESTAMP,
      BC250_REG_DMU_DIG0_AFMT_CNTL, BC250_REG_DMU_DIG0_AFMT_AUDIO_SRC_CONTROL, BC250_REG_DMU_DIG0_AFMT_AUDIO_PACKET_CONTROL,
      BC250_REG_DMU_DIG0_AFMT_AUDIO_PACKET_CONTROL2, BC250_REG_DMU_DIG0_AFMT_INFOFRAME_CONTROL0, BC250_REG_DMU_DIG0_AFMT_60958_0 },
    { BC250_REG_DMU_DP1_DP_SEC_CNTL, BC250_REG_DMU_DP1_DP_SEC_AUD_N, BC250_REG_DMU_DP1_DP_SEC_TIMESTAMP,
      BC250_REG_DMU_DIG1_AFMT_CNTL, BC250_REG_DMU_DIG1_AFMT_AUDIO_SRC_CONTROL, BC250_REG_DMU_DIG1_AFMT_AUDIO_PACKET_CONTROL,
      BC250_REG_DMU_DIG1_AFMT_AUDIO_PACKET_CONTROL2, BC250_REG_DMU_DIG1_AFMT_INFOFRAME_CONTROL0, BC250_REG_DMU_DIG1_AFMT_60958_0 },
};
#define DTO_SOURCE BC250_REG_DMU_DCCG_AUDIO_DTO_SOURCE
#define DTO1_MODULE BC250_REG_DMU_DCCG_AUDIO_DTO1_MODULE
#define DTO1_PHASE BC250_REG_DMU_DCCG_AUDIO_DTO1_PHASE
#define REFCLK BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT

// Unit A under Linux (M819, M820): codec 0x1002AA01 rev 0x00100700, audio strapped on, DP0 on DIG0 in SST, HPD0
// sensing, both pins 0x185600F0. The endpoint values are set to all ones where a read-modify-write must keep bits.
// The stream half reads what the lab read under the b21 driver (step 1 only): DTO_SOURCE 0x30, both DTOs phase 0
// module 1, AFMT_CNTL 0x100 (clock on, enable off), PACKET_CONTROL 0x04000800, PACKET_CONTROL2 0, DP_SEC_CNTL 0,
// and the reference clock counter 6000 (600.000 MHz, M788).
static void UnitA(FAKE* f)
{
    unsigned long n;

    memset(f, 0, sizeof(*f));
    Set(f, BC250_REG_DMU_AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID, BC250_DPAUDIO_CODEC_ID);
    Set(f, BC250_REG_DMU_AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID, BC250_DPAUDIO_CODEC_REVISION);
    Set(f, BC250_REG_DMU_DC_PINSTRAPS, DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK);
    Set(f, BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL, DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK);
    Set(f, BC250_REG_DMU_DIG0_DIG_BE_CNTL, 1ul << DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT);
    Set(f, BC250_REG_DMU_HPD0_DC_HPD_INT_STATUS, HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK);
    Set(f, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, 0xFFFFFFFFul);
    Set(f, BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, 0);
    for (unsigned long e = 0; e < 2; e++) {
        f->Ix[e][BC250_AZ_IX_RESPONSE_CONFIGURATION_DEFAULT] = BC250_DPAUDIO_CONFIG_DEFAULT;
        f->Ix[e][BC250_AZ_IX_HOT_PLUG_CONTROL] = 0x10;           // CLOCK_ON_STATE, kept by every write
        f->Ix[e][BC250_AZ_IX_CHANNEL_SPEAKER] = 0xFFFFFFFFul;
        f->Ix[e][BC250_AZ_IX_RESPONSE_HBR] = 0xFFFFFFFFul;
        f->Ix[e][BC250_AZ_IX_RESPONSE_LIPSYNC] = 0xFFFFFFFFul;
    }
    Set(f, BC250_REG_DMU_DIO_MEM_PWR_CTRL, 0x6DB6D800ul);  // the firmware's value on unit A (r19): every HDMIn force 3
    Set(f, REFCLK, 6000);
    Set(f, DTO_SOURCE, 0x30);
    Set(f, BC250_REG_DMU_DCCG_AUDIO_DTO0_MODULE, 1);
    Set(f, DTO1_MODULE, 1);
    for (n = 0; n < 2; n++) {
        Set(f, g_S[n].Afmt, 0x100);
        Set(f, g_S[n].Pkt, 0x04000800ul);
    }
}

// The same unit with the monitor on DP1, carried by DIG1 in SST.
static void StreamOne(FAKE* f)
{
    UnitA(f);
    Set(f, BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL, 0);
    Set(f, BC250_REG_DMU_DIG0_DIG_BE_CNTL, 0);
    Set(f, BC250_REG_DMU_DP1_DP_VID_STREAM_CNTL, DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK);
    Set(f, BC250_REG_DMU_DIG1_DIG_BE_CNTL, 2ul << DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT);
    Set(f, BC250_REG_DMU_HPD1_DC_HPD_INT_STATUS, HPD1_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK);
}

// ---- the expected writes, as numbers -----------------------------------------------------------------------------

static TRACE g_Expect[256];
static unsigned long g_Expected;

static void X(unsigned long e, unsigned long index, unsigned long value) { g_Expect[g_Expected++] = (TRACE){ OP_DATA, e, index, value }; }
static void D(unsigned long offset, unsigned long value) { g_Expect[g_Expected++] = (TRACE){ OP_DIRECT, 0, offset, value }; }

static void ExpectHwInit(void)
{
    X(0, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x11);
    D(BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, 0xFFFFF070ul);  // rates 32/44.1/48 kHz
    D(BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, 0xC0000000ul);         // CLKSTOP, EPSS
    X(0, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x10);
}

static void ExpectConfigure(unsigned long e)
{
    static const unsigned long descriptor[14] = {
        BC250_AZ_IX_AUDIO_DESCRIPTOR0, BC250_AZ_IX_AUDIO_DESCRIPTOR1, BC250_AZ_IX_AUDIO_DESCRIPTOR2, BC250_AZ_IX_AUDIO_DESCRIPTOR3,
        BC250_AZ_IX_AUDIO_DESCRIPTOR4, BC250_AZ_IX_AUDIO_DESCRIPTOR5, BC250_AZ_IX_AUDIO_DESCRIPTOR6, BC250_AZ_IX_AUDIO_DESCRIPTOR7,
        BC250_AZ_IX_AUDIO_DESCRIPTOR8, BC250_AZ_IX_AUDIO_DESCRIPTOR9, BC250_AZ_IX_AUDIO_DESCRIPTOR10, BC250_AZ_IX_AUDIO_DESCRIPTOR11,
        BC250_AZ_IX_AUDIO_DESCRIPTOR12, BC250_AZ_IX_AUDIO_DESCRIPTOR13 };
    unsigned long i;

    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x11);
    // From all ones: SPEAKER_ALLOCATION 1 (FL/FR), CHANNEL_ALLOCATION kept, HDMI 0, DP 1, EXTRA_CONNECTION_INFO bit 0
    // cleared (0x3E), LFE 0, the unnamed bits 7 and 26, LEVEL_SHIFT and DOWN_MIX_INHIBIT kept.
    X(e, BC250_AZ_IX_CHANNEL_SPEAKER, 0xFCFAFF81ul);
    for (i = 0; i < 14; i++) {
        if (i == 8 || i == 12) continue;    // format codes 9 (1-bit audio) and 13 (DST): Linux skips them
        // LPCM: stereo rates 0x07, byte 2 = 16 bit, rates 0x07, max channels 2 - 1.
        X(e, descriptor[i], i == 0 ? 0x07010701ul : 0);
    }
    X(e, BC250_AZ_IX_RESPONSE_HBR, 0xFFFFFFFEul);
    X(e, BC250_AZ_IX_RESPONSE_LIPSYNC, 0xFFFFFF00ul);
    X(e, BC250_AZ_IX_RESPONSE_LIPSYNC, 0xFFFF0000ul);
    X(e, BC250_AZ_IX_SINK_INFO0, 0);
    X(e, BC250_AZ_IX_SINK_INFO1, 10);                     // "BC-250 DP" and its terminator, as Linux counts
    X(e, BC250_AZ_IX_SINK_INFO2, 0x5558859eul);
    X(e, BC250_AZ_IX_SINK_INFO3, 0x0d989449ul);
    X(e, BC250_AZ_IX_SINK_INFO4, 0x322D4342ul);           // 'B' 'C' '-' '2'
    X(e, BC250_AZ_IX_SINK_INFO5, 0x44203035ul);           // '5' '0' ' ' 'D'
    X(e, BC250_AZ_IX_SINK_INFO6, 0x00000050ul);           // 'P'
    X(e, BC250_AZ_IX_SINK_INFO7, 0);
    X(e, BC250_AZ_IX_SINK_INFO8, 0);
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x10);
}

// The stream half on encoder n for endpoint e, from the lab's b21 values (UnitA). Gsp: DP_SEC_CNTL info-frame bits
// the firmware left on, which every write must keep.
static void ExpectStream(unsigned long n, unsigned long e, unsigned long module, unsigned long gsp)
{
    const SREGS* s = &g_S[n];

    D(BC250_REG_DMU_DIO_MEM_PWR_CTRL, 0);                 // the AFMT memories on, as dcn201_init_hw
    D(DTO_SOURCE, 0x00000010ul);                          // DTO_SEL 3 -> 1 (DTO1)
    D(DTO1_MODULE, module);
    D(DTO1_PHASE, 240000);                                // 24 MHz in 100 Hz units
    D(DTO_SOURCE, 0x00100010ul);                          // + DTO2_USE_512FBR_DTO: M820's 0x00100010
    D(s->Afmt, 0x101);                                    // AFMT_AUDIO_CLOCK_EN; CLOCK_ON was already 1: M820's 0x101
    D(s->Src, e);                                         // AFMT_AUDIO_SRC_SELECT = the endpoint
    D(s->Pkt2, 0x300);                                    // AFMT_AUDIO_CHANNEL_ENABLE FL, FR
    D(s->AudN, 0x8000);
    D(s->Timestamp, 1);                                   // auto-calc
    D(s->Pkt, 0x04000800ul);                              // AFMT_60958_CS_UPDATE (already 1 on the lab)
    D(s->Pkt2, 0x300);                                    // LAYOUT_OVRD 0, OSF_OVRD 0: M820's 0x300
    D(s->Info0, 0x80);                                    // AFMT_AUDIO_INFO_UPDATE
    D(s->Cs0, 0);                                         // AFMT_60958_CS_CLOCK_ACCURACY 0
    D(s->Sec, gsp | 0x0010);                              // ASP
    D(s->Sec, gsp | 0x1110);                              // + ATP, AIP
    D(s->Sec, gsp | 0x1111);                              // + STREAM_ENABLE last: M820's 0x1111
    D(s->Pkt, 0x04000801ul);                              // AFMT_AUDIO_SAMPLE_SEND: M820's 0x04000801
}

// The stop half from the state ExpectStream leaves.
static void ExpectStreamOff(unsigned long n, unsigned long gsp)
{
    const SREGS* s = &g_S[n];

    D(s->Pkt, 0x04000800ul);                              // SAMPLE_SEND 0
    D(s->Sec, gsp | 0x1110);                              // STREAM_ENABLE first
    D(s->Sec, gsp | 0x0010);                              // ATP, AIP
    D(s->Sec, gsp);                                       // ASP, ACM
    if (gsp != 0) D(s->Sec, gsp | 0x0001);                // Linux: the master stays on for the info frames
    D(s->Afmt, 0x100);                                    // AFMT_AUDIO_CLOCK_EN 0
}

static void ExpectEnable(unsigned long e)
{
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x80000011ul);
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, 0x80000010ul);
}

static void ExpectDisable(unsigned long e, unsigned long hpc)
{
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, hpc | HPC_CGD);
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, (hpc | HPC_CGD) & ~HPC_AE);
    X(e, BC250_AZ_IX_HOT_PLUG_CONTROL, hpc & ~(HPC_CGD | HPC_AE));
}

static int TraceMatches(const FAKE* f, const char* what)
{
    unsigned long i;

    if (f->Traced != g_Expected) {
        printf("  %s: %lu writes, expected %lu\n", what, f->Traced, g_Expected);
        for (i = 0; i < f->Traced && i < g_Expected; i++)
            if (memcmp(&f->Trace[i], &g_Expect[i], sizeof(TRACE)) != 0) {
                printf("  %s: first difference at write %lu: at 0x%lX = 0x%08lX, expected at 0x%lX = 0x%08lX\n", what, i,
                       f->Trace[i].Where, f->Trace[i].Value, g_Expect[i].Where, g_Expect[i].Value);
                break;
            }
        return 0;
    }
    for (i = 0; i < g_Expected; i++) {
        const TRACE* a = &f->Trace[i];
        const TRACE* b = &g_Expect[i];
        if (a->Op != b->Op || a->Endpoint != b->Endpoint || a->Where != b->Where || a->Value != b->Value) {
            printf("  %s: write %lu is op %lu ep %lu at 0x%lX = 0x%08lX, expected op %lu ep %lu at 0x%lX = 0x%08lX\n", what, i,
                   a->Op, a->Endpoint, a->Where, a->Value, b->Op, b->Endpoint, b->Where, b->Value);
            return 0;
        }
    }
    return 1;
}

// The audio state a stop or a failure must leave: AUDIO_ENABLED 0 on both endpoints, and on stream n no DP_SEC
// audio bit, no SAMPLE_SEND and no AFMT audio clock enable.
static int Silent(FAKE* f, unsigned long n)
{
    return (f->Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & (HPC_AE | HPC_CGD)) == 0 &&
           (f->Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] & (HPC_AE | HPC_CGD)) == 0 &&
           (Get(f, g_S[n].Sec) & 0x11111ul) == 0 && (Get(f, g_S[n].Pkt) & 1) == 0 && (Get(f, g_S[n].Afmt) & 1) == 0;
}

// ---- the cases ---------------------------------------------------------------------------------------------------

static BC250_DPAUDIO_RUN g_Run;
static FAKE g_Fake;

static void Gate(void)
{
    CHECK(Bc250DpAudioGate(1, 1, 1, 1) == BC250_DPAUDIO_REASON_OK);
    CHECK(Bc250DpAudioGate(1, 0, 1, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 2, 1, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);        // only 1 opens it
    CHECK(Bc250DpAudioGate(1, 0xFFFFFFFFul, 1, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 0, 1) == BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 7, 1) == BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF);
    // The stream switch off refuses the whole start: no endpoint without its stream.
    CHECK(Bc250DpAudioGate(1, 1, 1, 0) == BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 1, 2) == BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 1, 0xFFFFFFFFul) == BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(0, 1, 1, 1) == BC250_DPAUDIO_REASON_NO_MMIO);
    CHECK(Bc250DpAudioGate(0, 0, 0, 0) == BC250_DPAUDIO_REASON_SWITCH_OFF);       // the master switch first
    CHECK(Bc250DpAudioGate(0, 1, 0, 0) == BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(0, 1, 1, 0) == BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF);
}

static void ObserveBindsEverySlot(void)
{
    BC250_AZ_IO io;
    unsigned long long all = (BC250_DPAUDIO_OBS_COUNT == 64) ? ~0ull : ((1ull << BC250_DPAUDIO_OBS_COUNT) - 1);

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(g_Run.Obs.ValidMask == all);
    CHECK(g_Run.Obs.FirstFailure == 0);
    CHECK(g_Fake.Traced == 0);                            // reads only: no DATA and no direct write
    CHECK(g_Fake.IndexWrites == io.IndirectReads);        // one INDEX select per indirect read, nothing else
    CHECK(g_Fake.Writes == g_Fake.IndexWrites);
    CHECK(g_Fake.Protocol == 0);
    CHECK(io.Refusals == 0 && io.IndirectWrites == 0 && io.DirectWrites == 0);
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_CODEC_VENDOR_DEVICE] == BC250_DPAUDIO_CODEC_ID);
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT] == BC250_DPAUDIO_CONFIG_DEFAULT);
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_EP0_HOT_PLUG_CONTROL] == 0x10);
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_REFCLK_COUNT] == 6000);
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_DIG0_AFMT_AUDIO_PACKET_CONTROL] == 0x04000800ul);
    // A failed read leaves its slot 0 and invalid and is remembered, the others are still read.
    UnitA(&g_Fake);
    g_Fake.FailReadOffset = BC250_REG_DMU_DC_PINSTRAPS;
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(g_Run.Obs.ValidMask == (all & ~(1ull << BC250_DPAUDIO_OBS_DC_PINSTRAPS)));
    CHECK(g_Run.Obs.Regs[BC250_DPAUDIO_OBS_DC_PINSTRAPS] == 0);
    CHECK(g_Run.Obs.FirstFailure == FAKE_FAIL);
}

// One refusal: the run returns the reason and nothing reaches the fake but the INDEX selects of the reads.
static void Refuses(void (*setup)(FAKE*), unsigned long reason, const char* what)
{
    BC250_AZ_IO io;
    unsigned long got;

    setup(&g_Fake);
    Io(&io, &g_Fake);
    got = Bc250DpAudioRun(&io, &g_Run);
    if (got != reason) printf("  %s: reason %lu (%s), expected %lu\n", what, got, Bc250DpAudioReasonText(got), reason);
    CHECK(got == reason);
    CHECK(g_Run.Wrote == 0 && g_Run.StreamWrote == 0 && g_Run.Groups == 0);
    CHECK(g_Fake.Traced == 0);
    CHECK(g_Fake.Writes == g_Fake.IndexWrites);
    CHECK(io.IndirectWrites == 0 && io.DirectWrites == 0);
    CHECK(g_Fake.AudioEnabledWrites == 0);                // AUDIO_ENABLED stays 0
    CHECK((g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & HPC_AE) == 0 && (g_Fake.Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] & HPC_AE) == 0);
}

static void NoCodec(FAKE* f) { UnitA(f); Set(f, BC250_REG_DMU_AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID, 0x1002AA02ul); }
static void NoStraps(FAKE* f) { UnitA(f); Set(f, BC250_REG_DMU_DC_PINSTRAPS, ~(unsigned long)DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK); }
static void NoStream(FAKE* f) { UnitA(f); Set(f, BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL, ~(unsigned long)DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK); }
static void TwoStreams(FAKE* f) { UnitA(f); Set(f, BC250_REG_DMU_DP1_DP_VID_STREAM_CNTL, DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK); }
static void Mst(FAKE* f)
{
    UnitA(f);
    Set(f, BC250_REG_DMU_DIG0_DIG_BE_CNTL, (1ul << DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT) | (5ul << DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT));
}
static void OtherFrontEnd(FAKE* f) { UnitA(f); Set(f, BC250_REG_DMU_DIG0_DIG_BE_CNTL, 2ul << DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT); }
static void Ep0Config(FAKE* f) { UnitA(f); f->Ix[0][BC250_AZ_IX_RESPONSE_CONFIGURATION_DEFAULT] = 0x185600F1ul; }
static void Ep1ConfigOnStreamOne(FAKE* f) { StreamOne(f); f->Ix[1][BC250_AZ_IX_RESPONSE_CONFIGURATION_DEFAULT] = 0; }
static void Ep0ConfigOnStreamOne(FAKE* f) { StreamOne(f); f->Ix[0][BC250_AZ_IX_RESPONSE_CONFIGURATION_DEFAULT] = 0x40000000ul; }
static void UnreadStream(FAKE* f) { UnitA(f); f->FailReadOffset = BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL; }
static void UnreadCodec(FAKE* f) { UnitA(f); f->FailReadOffset = BC250_REG_DMU_AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID; }
static void UnreadConfig(FAKE* f) { UnitA(f); f->FailReadOffset = BC250_REG_DMU_AZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_DATA; }
static void NoRefClock(FAKE* f) { UnitA(f); Set(f, REFCLK, 0); }
static void LowRefClock(FAKE* f) { UnitA(f); Set(f, REFCLK, 4999); }
static void HighRefClock(FAKE* f) { UnitA(f); Set(f, REFCLK, 7001); }
static void UnreadRefClock(FAKE* f) { UnitA(f); f->FailReadOffset = REFCLK; }

static void Refusals(void)
{
    Refuses(NoCodec, BC250_DPAUDIO_REASON_CODEC_ID, "codec");
    Refuses(NoStraps, BC250_DPAUDIO_REASON_STRAPS, "straps");
    Refuses(NoStream, BC250_DPAUDIO_REASON_NO_STREAM, "no stream");
    Refuses(TwoStreams, BC250_DPAUDIO_REASON_TWO_STREAMS, "two streams");
    Refuses(Mst, BC250_DPAUDIO_REASON_NOT_DP_SST, "mst");
    Refuses(OtherFrontEnd, BC250_DPAUDIO_REASON_NOT_DP_SST, "front end");
    Refuses(Ep0Config, BC250_DPAUDIO_REASON_CONFIG_DEFAULT, "endpoint 0 config");
    Refuses(Ep1ConfigOnStreamOne, BC250_DPAUDIO_REASON_CONFIG_DEFAULT, "endpoint 1 config");
    Refuses(Ep0ConfigOnStreamOne, BC250_DPAUDIO_REASON_CONFIG_DEFAULT, "endpoint 0 config, stream 1");
    Refuses(UnreadStream, BC250_DPAUDIO_REASON_READ_FAILED, "stream unread");
    Refuses(UnreadCodec, BC250_DPAUDIO_REASON_READ_FAILED, "codec unread");
    Refuses(UnreadConfig, BC250_DPAUDIO_REASON_READ_FAILED, "endpoint 1 data unread");
    // Step 2's precondition refuses the whole start, step 1 included: no endpoint without a clock.
    Refuses(NoRefClock, BC250_DPAUDIO_REASON_REFCLK, "reference clock 0");
    Refuses(LowRefClock, BC250_DPAUDIO_REASON_REFCLK, "reference clock 499.9 MHz");
    Refuses(HighRefClock, BC250_DPAUDIO_REASON_REFCLK, "reference clock 700.1 MHz");
    Refuses(UnreadRefClock, BC250_DPAUDIO_REASON_READ_FAILED, "reference clock unread");
}

static void Notes(void)
{
    BC250_AZ_IO io;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK && g_Run.Plan.Notes == 0);
    // The DTO1 module follows the counted clock: 6000 x 100 kHz = 600 000 kHz, x 10 = 6 000 000.
    CHECK(g_Run.Plan.RefClock == 6000 && g_Run.Plan.DtoModule == 6000000ul);
    UnitA(&g_Fake);
    Set(&g_Fake, REFCLK, 5988);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK && g_Run.Plan.DtoModule == 5988000ul);
    UnitA(&g_Fake);
    Set(&g_Fake, REFCLK, 5000);                           // the bounds themselves are admitted
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK);
    UnitA(&g_Fake);
    Set(&g_Fake, REFCLK, 7000);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK);
    UnitA(&g_Fake);
    Set(&g_Fake, BC250_REG_DMU_HPD0_DC_HPD_INT_STATUS, 0);
    g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] = HPC_AE;
    Set(&g_Fake, BC250_REG_DMU_AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID, 0x00100800ul);
    g_Fake.Ix[0][BC250_AZ_IX_UNSOLICITED_RESPONSE] = AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_UNSOLICITED_RESPONSE__ENABLE_MASK;
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    // Notes never refuse.
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK);
    CHECK(g_Run.Plan.Notes == (BC250_DPAUDIO_NOTE_HPD_LOW | BC250_DPAUDIO_NOTE_INHERITED | BC250_DPAUDIO_NOTE_REVISION |
                               BC250_DPAUDIO_NOTE_UNSOLICITED));
    // HPD1 low does not concern a stream on DP0.
    UnitA(&g_Fake);
    Set(&g_Fake, BC250_REG_DMU_HPD1_DC_HPD_INT_STATUS, 0);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK && g_Run.Plan.Notes == 0);
}

static void StartOnStreamZero(void)
{
    BC250_AZ_IO io;
    BC250_DPAUDIO_STOP stop;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    CHECK(g_Run.Plan.Stream == 0 && g_Run.Plan.Endpoint == 0 && g_Run.Groups == 4 && g_Run.Wrote == 1 && g_Run.StreamWrote == 1);
    // The whole order: hw_init, configure, the stream, AUDIO_ENABLED last.
    g_Expected = 0;
    ExpectHwInit();
    ExpectConfigure(0);
    ExpectStream(0, 0, 6000000ul, 0);
    ExpectEnable(0);
    CHECK(TraceMatches(&g_Fake, "start on DP0"));
    CHECK(g_Fake.Trace[g_Fake.Traced - 2].Op == OP_DATA && (g_Fake.Trace[g_Fake.Traced - 2].Value & HPC_AE));
    CHECK(g_Fake.AudioEnabledWrites == 2);                // only the two writes of the enable group carry it
    CHECK(g_Fake.Protocol == 0);
    CHECK(g_Run.Init.Writes == 4 && g_Run.Config.Writes == 27 && g_Run.Stream.Writes == 18 && g_Run.Enable.Writes == 2);
    CHECK(io.DirectWrites == 2 + 18 && io.IndirectWrites == 31 && io.Refusals == 0);
    CHECK(g_Run.Init.SizeRates == 0xFFFFF070ul && g_Run.Init.PowerStates == 0xC0000000ul);
    CHECK(g_Run.Enable.HotPlugBefore == 0x10 && g_Run.Enable.HotPlugAfter == 0x80000010ul);
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x80000010ul);      // enabled, gating back on
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x10);              // the other endpoint untouched
    // M820, register by register (the DTO1 module excepted: 6 000 000 from the counted clock, not Linux's 5 988 740).
    CHECK(Get(&g_Fake, DTO_SOURCE) == 0x00100010ul && Get(&g_Fake, DTO1_PHASE) == 240000 && Get(&g_Fake, DTO1_MODULE) == 6000000ul);
    CHECK(Get(&g_Fake, g_S[0].Afmt) == 0x101 && Get(&g_Fake, g_S[0].Pkt) == 0x04000801ul && Get(&g_Fake, g_S[0].Pkt2) == 0x300);
    CHECK(Get(&g_Fake, g_S[0].Sec) == 0x1111 && Get(&g_Fake, g_S[0].AudN) == 0x8000 && Get(&g_Fake, g_S[0].Timestamp) == 1);
    CHECK(Get(&g_Fake, g_S[0].Src) == 0);
    CHECK(Get(&g_Fake, BC250_REG_DMU_DCCG_AUDIO_DTO0_MODULE) == 1);          // DTO0 untouched
    CHECK(Get(&g_Fake, g_S[1].Sec) == 0 && Get(&g_Fake, g_S[1].Afmt) == 0x100);  // the other encoder untouched
    // The result reports what the registers hold.
    CHECK(g_Run.Stream.Step == BC250_DPAUDIO_STEP_NONE && g_Run.Stream.Status == 0);
    CHECK(g_Run.Stream.DtoSource == 0x00100010ul && g_Run.Stream.DtoModule == 6000000ul && g_Run.Stream.DtoPhase == 240000);
    CHECK(g_Run.Stream.AfmtCntl == 0x101 && g_Run.Stream.SecCntl == 0x1111 && g_Run.Stream.PacketControl == 0x04000801ul);
    CHECK(g_Run.Stream.PacketControl2 == 0x300 && g_Run.Stream.AudN == 0x8000 && g_Run.Stream.Timestamp == 1);
    // The stop sequence: the stream off in reverse order, then dce_aud_az_disable.
    g_Fake.Traced = 0;
    g_Expected = 0;
    ExpectStreamOff(0, 0);
    ExpectDisable(0, 0x80000010ul);
    CHECK(Bc250DpAudioStopSequence(&io, 0, 0, 1, 1, &stop) == 0);
    CHECK(TraceMatches(&g_Fake, "stop on DP0"));
    CHECK(Silent(&g_Fake, 0));
    CHECK(stop.Stream.Step == BC250_DPAUDIO_STEP_NONE && stop.Stream.SecCntl == 0 && stop.Stream.AfmtCntl == 0x100);
    CHECK(stop.Stream.PacketControl == 0x04000800ul && stop.Stream.DtoSource == 0x00100010ul);
    CHECK(stop.Endpoint.HotPlugBefore == 0x80000010ul && stop.Endpoint.HotPlugAfter == 0x10);
    CHECK(Get(&g_Fake, DTO1_MODULE) == 6000000ul);        // the DTO stays programmed, as in Linux
    CHECK(g_Fake.Protocol == 0);
    // Only the endpoint half, or only the stream half, when only that one is owed.
    g_Fake.Traced = 0;
    g_Expected = 0;
    ExpectDisable(0, 0x10);
    CHECK(Bc250DpAudioStopSequence(&io, 0, 0, 0, 1, &stop) == 0);
    CHECK(TraceMatches(&g_Fake, "stop, endpoint only"));
    CHECK(Bc250DpAudioStopSequence(&io, 0, 0, 0, 0, &stop) == 0);
    CHECK(TraceMatches(&g_Fake, "stop, nothing owed"));
}

static void StartOnStreamOne(void)
{
    BC250_AZ_IO io;

    StreamOne(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    CHECK(g_Run.Plan.Stream == 1 && g_Run.Plan.Endpoint == 1);
    g_Expected = 0;
    ExpectHwInit();                                       // endpoint 0 whatever the stream, as Linux
    ExpectConfigure(1);
    ExpectStream(1, 1, 6000000ul, 0);                     // DIG1/DP1, source select = endpoint 1
    ExpectEnable(1);
    CHECK(TraceMatches(&g_Fake, "start on DP1"));
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x10);              // hw_init's bracket closed, no enable
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x80000010ul);
    CHECK(Get(&g_Fake, g_S[1].Src) == 1 && Get(&g_Fake, g_S[1].Sec) == 0x1111);
    CHECK(Get(&g_Fake, g_S[0].Sec) == 0 && Get(&g_Fake, g_S[0].Afmt) == 0x100 && Get(&g_Fake, g_S[0].Pkt) == 0x04000800ul);
}

// Bits beside the named ones stay: the firmware's info-frame packets in DP_SEC_CNTL, and every other bit of the AFMT
// and DP_SEC registers. The stop keeps the info frames with their master enable, as Linux's disable does.
static void KeepsOtherBits(void)
{
    BC250_AZ_IO io;
    BC250_DPAUDIO_STREAM_RESULT r;
    const unsigned long gsp = DP0_DP_SEC_CNTL__DP_SEC_GSP0_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_MPG_ENABLE_MASK;

    UnitA(&g_Fake);
    Set(&g_Fake, g_S[0].Sec, gsp);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioStreamEnable(&io, 0, 0, 6000000ul, &r) == 0);
    g_Expected = 0;
    ExpectStream(0, 0, 6000000ul, gsp);
    CHECK(TraceMatches(&g_Fake, "enable beside info frames"));
    g_Fake.Traced = 0;
    g_Expected = 0;
    ExpectStreamOff(0, gsp);
    CHECK(Bc250DpAudioStreamDisable(&io, 0, &r) == 0);
    CHECK(TraceMatches(&g_Fake, "disable beside info frames"));
    CHECK(Get(&g_Fake, g_S[0].Sec) == (gsp | DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK) && r.SecCntl == Get(&g_Fake, g_S[0].Sec));
    // All ones elsewhere: only the named fields move.
    UnitA(&g_Fake);
    Set(&g_Fake, DTO_SOURCE, 0xFFFFFFFFul);
    Set(&g_Fake, g_S[0].AudN, 0xFF000000ul);
    Set(&g_Fake, g_S[0].Timestamp, 0xFFFFFFFEul);
    Set(&g_Fake, g_S[0].Src, 0xFFFFFFF8ul);
    Set(&g_Fake, g_S[0].Info0, 0xFFFFFF7Ful);
    Set(&g_Fake, g_S[0].Cs0, 0xFFFFFFFFul);
    Set(&g_Fake, g_S[0].Pkt2, 0xFFFFFFFFul);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioStreamEnable(&io, 0, 0, 6000000ul, &r) == 0);
    CHECK(Get(&g_Fake, DTO_SOURCE) == 0xFFFFFFDFul);      // DTO_SEL 3 -> 1, 512FBR already 1, the rest kept
    CHECK(Get(&g_Fake, g_S[0].AudN) == 0xFF008000ul && Get(&g_Fake, g_S[0].Timestamp) == 0xFFFFFFFFul);
    CHECK(Get(&g_Fake, g_S[0].Src) == 0xFFFFFFF8ul && Get(&g_Fake, g_S[0].Info0) == 0xFFFFFFFFul);
    CHECK(Get(&g_Fake, g_S[0].Cs0) == 0xCFFFFFFFul);      // CS_CLOCK_ACCURACY 0 only
    CHECK(Get(&g_Fake, g_S[0].Pkt2) == 0xEFFF03FEul);     // channels 0x03, LAYOUT_OVRD 0, OSF_OVRD 0, the rest kept
    // Out-of-range stream or endpoint: refused, nothing written.
    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioStreamEnable(&io, 2, 0, 6000000ul, &r) == BC250_AZ_STATUS_INVALID_PARAMETER);
    CHECK(Bc250DpAudioStreamEnable(&io, 0, 2, 6000000ul, &r) == BC250_AZ_STATUS_INVALID_PARAMETER);
    CHECK(Bc250DpAudioStreamDisable(&io, 2, &r) == BC250_AZ_STATUS_INVALID_PARAMETER);
    CHECK(g_Fake.Writes == 0 && g_Fake.Reads == 0 && io.Refusals == 3);
}

// Write calls a group makes on a fresh unit A, INDEX selects included.
static unsigned long Calls(int group)
{
    BC250_AZ_IO io;
    BC250_DPAUDIO_STREAM_RESULT r;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    if (group == 0) Bc250DpAudioObserve(&io, &g_Run.Obs);
    if (group == 1) Bc250DpAudioHwInit(&io, &g_Run.Init);
    if (group == 2) Bc250DpAudioConfigure(&io, 0, &g_Run.Config);
    if (group == 3) Bc250DpAudioStreamEnable(&io, 0, 0, 6000000ul, &r);
    if (group == 4) Bc250DpAudioSetEnabled(&io, 0, 1, &g_Run.Enable);
    return g_Fake.Writes;
}

// A failed write anywhere ends the groups, and the stop sequence leaves silence: AUDIO_ENABLED 0 on both endpoints,
// and once the stream group began, no DP_SEC audio bit, no SAMPLE_SEND and no AFMT clock. Before the enable group,
// AUDIO_ENABLED is never written 1 at all.
static void WriteFailures(void)
{
    unsigned long observe = Calls(0), init = Calls(1), configure = Calls(2), stream = Calls(3), enable = Calls(4), k, bad = 0;
    const unsigned long base = observe + init + configure;

    CHECK(observe == 16 && init > 0 && configure > 0 && stream == 18 && enable > 0);
    for (k = observe + 1; k <= base + stream + enable; k++) {
        BC250_AZ_IO io;
        unsigned long groups = k <= observe + init ? 0 : k <= base ? 1 : k <= base + stream ? 2 : 3, reason;
        int streamBegan = k > base;

        UnitA(&g_Fake);
        g_Fake.FailWriteAt = k;
        Io(&io, &g_Fake);
        reason = Bc250DpAudioRun(&io, &g_Run);
        if (reason != BC250_DPAUDIO_REASON_WRITE_FAILED || g_Run.Groups != groups || g_Run.Status != FAKE_FAIL ||
            g_Run.UndoStatus != 0 || !Silent(&g_Fake, 0) || g_Run.Wrote != 1 || g_Run.StreamWrote != (unsigned long)streamBegan ||
            (groups < 3 && g_Fake.AudioEnabledWrites != 0) ||
            (groups == 2 && g_Run.Stream.Step == BC250_DPAUDIO_STEP_NONE) ||
            (streamBegan && g_Run.Undo.StreamStatus != 0) || (!streamBegan && g_Run.Undo.Stream.Writes != 0)) {
            printf("  write %lu failed: reason %lu groups %lu (expected %lu) status 0x%08lX undo 0x%08lX ae %lu sec 0x%08lX\n",
                   k, reason, g_Run.Groups, groups, (unsigned long)g_Run.Status, (unsigned long)g_Run.UndoStatus,
                   g_Fake.AudioEnabledWrites, Get(&g_Fake, g_S[0].Sec));
            bad++;
        }
    }
    CHECK(bad == 0);
}

// A read-back that differs in a named bit undoes everything at once: the step and the bits are reported, the stop
// sequence runs, and AUDIO_ENABLED was never written 1.
typedef struct { unsigned long Offset, Mask, Step; } STUCK;

static void Mismatches(void)
{
    const STUCK cases[] = {
        { BC250_REG_DMU_DIO_MEM_PWR_CTRL, DIO_MEM_PWR_CTRL__HDMI0_MEM_PWR_FORCE_MASK, BC250_DPAUDIO_STEP_AFMT_MEM_POWER },
        { DTO_SOURCE, DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK, BC250_DPAUDIO_STEP_DTO_SELECT },
        { DTO1_MODULE, 0x00000001ul, BC250_DPAUDIO_STEP_DTO1_MODULE },
        { DTO1_PHASE, 0x00000080ul, BC250_DPAUDIO_STEP_DTO1_PHASE },       // 240000 is 0x3A980
        { DTO_SOURCE, DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO2_USE_512FBR_DTO_MASK, BC250_DPAUDIO_STEP_DTO_512FBR },
        { BC250_REG_DMU_DIG0_AFMT_CNTL, DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK, BC250_DPAUDIO_STEP_AFMT_CLOCK_ON },
        { BC250_REG_DMU_DIG0_AFMT_AUDIO_PACKET_CONTROL2, 0x00000100ul, BC250_DPAUDIO_STEP_CHANNEL_ENABLE },
        { BC250_REG_DMU_DP0_DP_SEC_AUD_N, 0x00008000ul, BC250_DPAUDIO_STEP_AUD_N },
        { BC250_REG_DMU_DP0_DP_SEC_TIMESTAMP, 0x00000001ul, BC250_DPAUDIO_STEP_TIMESTAMP },
        { BC250_REG_DMU_DP0_DP_SEC_CNTL, DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK, BC250_DPAUDIO_STEP_SEC_ASP_ON },
        { BC250_REG_DMU_DP0_DP_SEC_CNTL, DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK, BC250_DPAUDIO_STEP_SEC_ATP_AIP_ON },
        { BC250_REG_DMU_DP0_DP_SEC_CNTL, DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK, BC250_DPAUDIO_STEP_SEC_STREAM_ON },
        { BC250_REG_DMU_DIG0_AFMT_AUDIO_PACKET_CONTROL, DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK,
          BC250_DPAUDIO_STEP_SAMPLE_SEND_ON },
    };
    unsigned long i, bad = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        BC250_AZ_IO io;
        unsigned long reason;

        UnitA(&g_Fake);
        g_Fake.StuckOffset = cases[i].Offset;
        g_Fake.StuckMask = cases[i].Mask;
        Io(&io, &g_Fake);
        reason = Bc250DpAudioRun(&io, &g_Run);
        if (reason != BC250_DPAUDIO_REASON_STREAM_MISMATCH || g_Run.Groups != 2 || g_Run.Status != BC250_AZ_STATUS_MISMATCH ||
            g_Run.Stream.Step != cases[i].Step || g_Run.Stream.MismatchOffset != cases[i].Offset ||
            ((g_Run.Stream.MismatchExpected ^ g_Run.Stream.MismatchActual) & cases[i].Mask) == 0 ||
            g_Run.StreamWrote != 1 || g_Fake.AudioEnabledWrites != 0 || g_Run.Enable.Writes != 0 ||
            (cases[i].Mask != DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK && !Silent(&g_Fake, 0)) ||
            (g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & HPC_AE) != 0 || g_Run.Undo.Stream.Writes == 0) {
            printf("  stuck 0x%05lX/0x%08lX: reason %lu groups %lu step %lu (%s, expected %s) at 0x%05lX 0x%08lX/0x%08lX\n",
                   cases[i].Offset, cases[i].Mask, reason, g_Run.Groups, g_Run.Stream.Step,
                   Bc250DpAudioStepText(g_Run.Stream.Step), Bc250DpAudioStepText(cases[i].Step), g_Run.Stream.MismatchOffset,
                   g_Run.Stream.MismatchExpected, g_Run.Stream.MismatchActual);
            bad++;
        }
    }
    CHECK(bad == 0);
    // The undo of a mismatch at the last DP_SEC step: the trace ends with the stop sequence and the endpoint disable,
    // and the enable group never ran.
    UnitA(&g_Fake);
    g_Fake.StuckOffset = BC250_REG_DMU_DIG0_AFMT_AUDIO_PACKET_CONTROL;
    g_Fake.StuckMask = DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK;
    {
        BC250_AZ_IO io;

        Io(&io, &g_Fake);
        CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_STREAM_MISMATCH);
    }
    g_Expected = 0;
    ExpectHwInit();
    ExpectConfigure(0);
    ExpectStream(0, 0, 6000000ul, 0);
    ExpectStreamOff(0, 0);
    ExpectDisable(0, 0x10);
    CHECK(TraceMatches(&g_Fake, "mismatch at SAMPLE_SEND, undone"));
    CHECK(g_Run.Stream.MismatchExpected == 1 && g_Run.Stream.MismatchActual == 0);
    // Update strobes are not compared: a CS_UPDATE or AUDIO_INFO_UPDATE that reads back 0 is no mismatch.
    UnitA(&g_Fake);
    g_Fake.StuckOffset = BC250_REG_DMU_DIG0_AFMT_INFOFRAME_CONTROL0;
    g_Fake.StuckMask = DIG0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK;
    {
        BC250_AZ_IO io;

        Io(&io, &g_Fake);
        CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    }
}

// The stop half runs every step whatever fails, so one bad write cannot leave the packets on.
static void StopBestEffort(void)
{
    BC250_AZ_IO io;
    BC250_DPAUDIO_STOP stop;
    unsigned long first;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    first = g_Fake.Writes;
    g_Fake.FailWriteAt = first + 1;                       // the SAMPLE_SEND 0 write
    g_Fake.Traced = 0;
    CHECK(Bc250DpAudioStopSequence(&io, 0, 0, 1, 1, &stop) == FAKE_FAIL);
    CHECK(stop.Stream.Step == BC250_DPAUDIO_STEP_SAMPLE_SEND_OFF && stop.StreamStatus == FAKE_FAIL && stop.EndpointStatus == 0);
    CHECK((Get(&g_Fake, g_S[0].Sec) & 0x11111ul) == 0 && (Get(&g_Fake, g_S[0].Afmt) & 1) == 0);
    CHECK((g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & HPC_AE) == 0);
    CHECK(g_Fake.Traced == 4 + 3);                        // DP_SEC x3 and the AFMT clock, then the endpoint
}

static void Tables(void)
{
    BC250_AZ_IO io;
    unsigned long value = 0, i, writable = 0;

    // 27 offsets take a write: the two INDEX/DATA pairs, the two function parameters of hw_init, and step 2's DTO
    // source, DTO1 module and phase, and nine AFMT/DP_SEC registers per stream encoder.
    for (i = 0; i < BC250_MMIO_AUDIO_ALLOW_COUNT; i++) writable += (unsigned long)Bc250AzWriteAllowed(g_MmioAudioAllow[i]);
    CHECK(writable == BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT && writable == 28);
    CHECK(Bc250AzWriteAllowed(BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES));
    for (i = 0; i < 2; i++) {
        CHECK(Bc250AzWriteAllowed(g_S[i].Sec) && Bc250AzWriteAllowed(g_S[i].AudN) && Bc250AzWriteAllowed(g_S[i].Timestamp));
        CHECK(Bc250AzWriteAllowed(g_S[i].Afmt) && Bc250AzWriteAllowed(g_S[i].Src) && Bc250AzWriteAllowed(g_S[i].Pkt));
        CHECK(Bc250AzWriteAllowed(g_S[i].Pkt2) && Bc250AzWriteAllowed(g_S[i].Info0) && Bc250AzWriteAllowed(g_S[i].Cs0));
    }
    CHECK(Bc250AzWriteAllowed(DTO_SOURCE) && Bc250AzWriteAllowed(DTO1_MODULE) && Bc250AzWriteAllowed(DTO1_PHASE));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DC_PINSTRAPS));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DCCG_AUDIO_DTO0_MODULE));     // DTO0 is HDMI's: read only
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DCCG_AUDIO_DTO0_PHASE));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DP0_DP_SEC_AUD_M_READBACK));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DIG0_AFMT_STATUS));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DP0_DP_VID_STREAM_CNTL));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DIG0_DIG_BE_CNTL));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_HPD0_DC_HPD_INT_STATUS));
    CHECK(!Bc250AzWriteAllowed(REFCLK) && Bc250AzReadAllowed(REFCLK));   // the clock counter: read only
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_OTG0_OTG_CONTROL));
    CHECK(!Bc250AzReadAllowed(BC250_REG_GC_GRBM_STATUS));
    CHECK(!Bc250AzReadAllowed(BC250_REG_DMU_DC_PINSTRAPS + 2));          // misaligned
    CHECK(Bc250AzReadAllowed(BC250_REG_DMU_HPD1_DC_HPD_INT_STATUS));

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250AzRead(&io, BC250_REG_GC_GRBM_STATUS, &value) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzWrite(&io, BC250_REG_DMU_DC_PINSTRAPS, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzWrite(&io, BC250_REG_DMU_OTG0_OTG_CONTROL, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    // The read-only indices: the pin's configuration, sense, widget and unsolicited response.
    CHECK(Bc250AzIndirectWrite(&io, 0, BC250_AZ_IX_RESPONSE_CONFIGURATION_DEFAULT, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzIndirectWrite(&io, 0, BC250_AZ_IX_RESPONSE_PIN_SENSE, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzIndirectWrite(&io, 1, BC250_AZ_IX_WIDGET_CONTROL, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzIndirectWrite(&io, 1, BC250_AZ_IX_UNSOLICITED_RESPONSE, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    // An index on neither table, next to one that is.
    CHECK(Bc250AzIndirectRead(&io, 0, BC250_AZ_IX_HOT_PLUG_CONTROL + 1, &value) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzIndirectWrite(&io, 0, BC250_AZ_IX_HOT_PLUG_CONTROL + 1, 0) == BC250_AZ_STATUS_ACCESS_DENIED);
    CHECK(Bc250AzIndirectRead(&io, 2, BC250_AZ_IX_HOT_PLUG_CONTROL, &value) == BC250_AZ_STATUS_INVALID_PARAMETER);
    CHECK(Bc250AzIndirectWrite(&io, 2, BC250_AZ_IX_HOT_PLUG_CONTROL, 0) == BC250_AZ_STATUS_INVALID_PARAMETER);
    CHECK(io.Refusals == 11);
    CHECK(g_Fake.Writes == 0 && g_Fake.Reads == 0);                     // a refusal reaches nothing
    // Every write-table offset and index is readable, so each read-modify-write can read what it writes.
    for (i = 0; i < BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT; i++) CHECK(Bc250AzReadAllowed(g_MmioAudioWriteAllow[i]));
    for (i = 0; i < BC250_AZ_IX_WRITE_ALLOW_COUNT; i++) {
        unsigned long j, found = 0;
        for (j = 0; j < BC250_AZ_IX_READ_ALLOW_COUNT; j++) found |= g_AzIxReadAllow[j] == g_AzIxWriteAllow[i];
        CHECK(found);
    }
}

static void Texts(void)
{
    unsigned long i, j;

    for (i = 0; i < BC250_DPAUDIO_REASON_COUNT; i++) {
        CHECK(Bc250DpAudioReasonText(i) != NULL && Bc250DpAudioReasonText(i)[0] != 0);
        for (j = 0; j < i; j++) CHECK(strcmp(Bc250DpAudioReasonText(i), Bc250DpAudioReasonText(j)) != 0);
    }
    CHECK(strcmp(Bc250DpAudioReasonText(BC250_DPAUDIO_REASON_COUNT), "unknown reason") == 0);
    for (i = 0; i < BC250_DPAUDIO_STEP_COUNT; i++) {
        CHECK(Bc250DpAudioStepText(i) != NULL && Bc250DpAudioStepText(i)[0] != 0);
        for (j = 0; j < i; j++) CHECK(strcmp(Bc250DpAudioStepText(i), Bc250DpAudioStepText(j)) != 0);
    }
    CHECK(strcmp(Bc250DpAudioStepText(BC250_DPAUDIO_STEP_SEC_STREAM_ON), "SEC_STREAM_ON") == 0);
    CHECK(strcmp(Bc250DpAudioStepText(BC250_DPAUDIO_STEP_COUNT), "unknown step") == 0);
}
// ABI 1 numbers keep their meaning: the reasons and slots of 0.7.215 are where they were.
typedef char ReasonsAppended[(BC250_DPAUDIO_REASON_PATH_OFF == 14 && BC250_DPAUDIO_REASON_STREAM_SWITCH_OFF == 15) ? 1 : -1];
typedef char SlotsAppended[(BC250_DPAUDIO_OBS_EP1_SINK_INFO1 == 52 && BC250_DPAUDIO_OBS_REFCLK_COUNT == 53) ? 1 : -1];
typedef char ReasonOkIsZero[BC250_DPAUDIO_REASON_OK == 0 ? 1 : -1];     // a zeroed record reads "OK" nowhere else
typedef char StepNoneIsZero[BC250_DPAUDIO_STEP_NONE == 0 ? 1 : -1];
// The ABI 1 prefix is the 0.7.215 layout: the stream record starts right after LastStatus.
typedef char Abi1Prefix[(offsetof(BC250_ESCAPE_DPAUDIO, SwitchStream) == BC250_DPAUDIO_ABI1_SIZE &&
                         sizeof(BC250_ESCAPE_DPAUDIO) == 480) ? 1 : -1];


// ---- step 4: the sink from the EDID -----------------------------------------------------------------------------

// Configure with a sink on endpoint e of a fresh unit A; the endpoint's registers as the group left them.
static void ConfigureWith(const BC250_DPAUDIO_SINK* sink, unsigned long e)
{
    BC250_AZ_IO io;
    BC250_DPAUDIO_RESULT r;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioConfigureSink(&io, e, sink, &r) == 0);
    CHECK(g_Fake.Protocol == 0);
}

static void StepFour(void)
{
    static BC250_EDID_INFO info;
    static unsigned char tv[256];
    BC250_DPAUDIO_SINK sink, fixed;
    BC250_AZ_IO io;
    unsigned long i;

    // The lab monitor: LEN (bytes 0x30 0xAE), product 0x1144, LPCM 2 channels, 32-192 kHz, 16/20/24 bit, FL/FR.
    CHECK(Bc250EdidParse(g_LabEdid, sizeof(g_LabEdid), &info) == BC250_EDID_OK);
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 1 && sink.FromEdid == 1);
    CHECK(sink.Manufacturer == 0xAE30ul && sink.Product == 0x1144ul);
    CHECK(strcmp(sink.Name, "LEN LT2452pwC") == 0);
    CHECK(sink.LpcmChannels == 2 && sink.LpcmRates == 0x7F && sink.LpcmSizes == 0x07 && sink.Speakers == 0x01);
    ConfigureWith(&sink, 1);
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_CHANNEL_SPEAKER] == 0xFCFAFF81ul);       // the speaker byte is 1, as the fixed set
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_AUDIO_DESCRIPTOR0] == 0x7F077F01ul);     // stereo rates, byte 2, rates, channels - 1
    for (i = 1; i < 14; i++) CHECK(g_Fake.Ix[1][BC250_AZ_IX_AUDIO_DESCRIPTOR0 + i] == 0);
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO0] == 0x1144AE30ul);            // PRODUCT_ID 31:16, MANUFACTURER_ID 15:0
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO1] == 14);                      // 13 characters and the terminator
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO4] == 0x204E454Cul);            // 'L' 'E' 'N' ' '
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO5] == 0x3432544Cul);            // 'L' 'T' '2' '4'
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO6] == 0x77703235ul);            // '5' '2' 'p' 'w'
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO7] == 0x00000043ul);            // 'C'
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_SINK_INFO8] == 0);
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_RESPONSE_HBR] == 0xFFFFFFFEul);          // HBR_CAPABLE 0 whatever the sink says
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO0] == 0);                       // the other endpoint is not touched

    // A whole run with the sink: the configure group carries it, nothing else changes.
    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    g_Run.Sink = &sink;
    CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    g_Run.Sink = 0;
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_AUDIO_DESCRIPTOR0] == 0x7F077F01ul && g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO0] == 0x1144AE30ul);
    CHECK((g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & HPC_AE) != 0);

    // The fixed set: Sink NULL and Bc250DpAudioSinkDefault give the same writes as step 1 (ExpectConfigure).
    Bc250DpAudioSinkDefault(&fixed);
    CHECK(fixed.FromEdid == 0 && strcmp(fixed.Name, "BC-250 DP") == 0 && fixed.LpcmChannels == 2 &&
          fixed.LpcmRates == 0x07 && fixed.LpcmSizes == 0x01 && fixed.Speakers == 0x01 && fixed.Manufacturer == 0);
    ConfigureWith(&fixed, 0);
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_AUDIO_DESCRIPTOR0] == 0x07010701ul && g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO0] == 0 &&
          g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO1] == 10 && g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO4] == 0x322D4342ul);

    // An 8-channel LPCM descriptor after a 2-channel one: the most channels win; a 7-speaker byte is kept.
    info.Sads[1] = info.Sads[0];
    info.Sads[1].Channels = 8;
    info.Sads[1].Rates = 0x07;
    info.SadCount = 2;
    info.Speaker = 0x7F;
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 1 && sink.LpcmChannels == 8 && sink.LpcmRates == 0x07 &&
          sink.Speakers == 0x7F);
    ConfigureWith(&sink, 0);
    CHECK((g_Fake.Ix[0][BC250_AZ_IX_AUDIO_DESCRIPTOR0] & 0x7ul) == 7);      // MAX_CHANNELS = 8 - 1
    // More than 8 channels cannot be described: capped (3-bit field).
    info.Sads[1].Channels = 9;
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 1 && sink.LpcmChannels == 8);
    // No speaker allocation block: DEFAULT_SPEAKER_LOCATION.
    info.HasSpeaker = 0;
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 1 && sink.Speakers == 5);
    // No name descriptor: an empty name, length 1 as Linux counts it.
    info.HasName = 0;
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 1 && sink.Name[0] == '\0');
    ConfigureWith(&sink, 0);
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO1] == 1 && g_Fake.Ix[0][BC250_AZ_IX_SINK_INFO4] == 0);

    // Negative controls: the fixed set and a return of 0.
    CHECK(Bc250DpAudioSinkFromEdid(0, &sink) == 0 && sink.FromEdid == 0 && sink.LpcmRates == 0x07);
    for (i = 0; i < info.SadCount; i++) info.Sads[i].Format = 2;    // AC-3 only: no LPCM descriptor
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 0 && sink.FromEdid == 0 && sink.Speakers == 0x01);
    memcpy(tv, g_LabEdid, sizeof(tv));
    tv[127] ^= 0x01;                                                    // the base block checksum breaks
    CHECK(Bc250EdidParse(tv, sizeof(tv), &info) == BC250_EDID_CHECKSUM);
    CHECK(Bc250DpAudioSinkFromEdid(&info, &sink) == 0 && sink.FromEdid == 0);
}

int main(void)
{
    Gate();
    ObserveBindsEverySlot();
    Refusals();
    Notes();
    StartOnStreamZero();
    StartOnStreamOne();
    KeepsOtherBits();
    WriteFailures();
    Mismatches();
    StopBestEffort();
    Tables();
    Texts();
    StepFour();
    printf("dpaudio: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
