// Host test of DisplayPort audio steps 0 and 1: driver/kmd/dpaudio_seq.c is compiled as it is, the very file the
// miniport links, and driven against a fake register file. Gate "dpaudio" of tools/quality/quick.ps1.
//
// What it has to prove, in the order the risk runs:
//   1. Nothing is written unless every precondition holds. Each refusal gets its own case, and each case checks
//      that the fake saw no write but the INDEX selects of the reads.
//   2. The writes, when they happen, are Linux's (dce_audio.c dce_aud_hw_init, dce_aud_az_configure for DP,
//      dce_aud_az_enable, dce_aud_az_disable), in Linux's order, inside the CLOCK_GATING_DISABLE bracket, with
//      the values written out here as numbers, not recomputed with the production macros.
//   3. Every indirect access selects its index first, on the endpoint it means.
//   4. The tables refuse every other offset and index, so no caller can reach past them.
//   5. A failed write anywhere in the groups ends them and leaves AUDIO_ENABLED 0.
#include <stdio.h>
#include <string.h>
#define BC250_REGS_WITH_AUDIO_TABLES
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dpaudio_seq.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define FAKE_FAIL ((long)0xC0000001L)        // STATUS_UNSUCCESSFUL from the fake's injected failures
#define HPC_CGD AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE_MASK
#define HPC_AE AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK

// ---- the fake register file --------------------------------------------------------------------------------------

enum { OP_DIRECT = 1, OP_DATA = 2 };
typedef struct { unsigned long Op, Endpoint, Where, Value; } TRACE;  // a logical write: direct, or DATA at an index

typedef struct {
    unsigned long DirectOffset[64], DirectValue[64], Directs;
    unsigned long Ix[2][0x4000];
    unsigned long Index[2];
    int Armed[2];                           // an INDEX select waits for its DATA access
    unsigned long Protocol;                 // DATA accesses without a fresh INDEX select
    unsigned long Reads, Writes, IndexWrites;
    unsigned long FailWriteAt;              // the n-th Write call (1-based) fails; 0 = none
    unsigned long FailReadOffset;           // a read of this offset fails; 0 = none
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
            f->Trace[f->Traced++] = (TRACE){ OP_DATA, e, f->Index[e], value };
            return 0;
        }
    }
    *Direct(f, offset) = value;
    f->Trace[f->Traced++] = (TRACE){ OP_DIRECT, 0, offset, value };
    return 0;
}

static void Io(BC250_AZ_IO* io, FAKE* f)
{
    memset(io, 0, sizeof(*io));
    io->Context = f;
    io->Read = FakeRead;
    io->Write = FakeWrite;
}

// Unit A under Linux (M819, M820): codec 0x1002AA01 rev 0x00100700, audio strapped on, DP0 on DIG0 in SST, HPD0
// sensing, both pins 0x185600F0. The other values are set to all ones where a read-modify-write must keep bits.
static void UnitA(FAKE* f)
{
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

// ---- the cases ---------------------------------------------------------------------------------------------------

static BC250_DPAUDIO_RUN g_Run;
static FAKE g_Fake;

static void Gate(void)
{
    CHECK(Bc250DpAudioGate(1, 1, 1) == BC250_DPAUDIO_REASON_OK);
    CHECK(Bc250DpAudioGate(1, 0, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 2, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);           // only 1 opens it
    CHECK(Bc250DpAudioGate(1, 0xFFFFFFFFul, 1) == BC250_DPAUDIO_REASON_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 0) == BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(1, 1, 7) == BC250_DPAUDIO_REASON_ENDPOINT_SWITCH_OFF);
    CHECK(Bc250DpAudioGate(0, 1, 1) == BC250_DPAUDIO_REASON_NO_MMIO);
    CHECK(Bc250DpAudioGate(0, 0, 0) == BC250_DPAUDIO_REASON_SWITCH_OFF);          // the master switch first
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
    CHECK(g_Run.Wrote == 0 && g_Run.Groups == 0);
    CHECK(g_Fake.Traced == 0);
    CHECK(g_Fake.Writes == g_Fake.IndexWrites);
    CHECK(io.IndirectWrites == 0 && io.DirectWrites == 0);
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
}

static void Notes(void)
{
    BC250_AZ_IO io;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    Bc250DpAudioObserve(&io, &g_Run.Obs);
    CHECK(Bc250DpAudioDecide(&g_Run.Obs, &g_Run.Plan) == BC250_DPAUDIO_REASON_OK && g_Run.Plan.Notes == 0);
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

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    CHECK(Bc250DpAudioRun(&io, &g_Run) == BC250_DPAUDIO_REASON_OK);
    CHECK(g_Run.Plan.Stream == 0 && g_Run.Plan.Endpoint == 0 && g_Run.Groups == 3 && g_Run.Wrote == 1);
    g_Expected = 0;
    ExpectHwInit();
    ExpectConfigure(0);
    ExpectEnable(0);
    CHECK(TraceMatches(&g_Fake, "start on DP0"));
    CHECK(g_Fake.Protocol == 0);
    CHECK(g_Run.Init.Writes == 4 && g_Run.Config.Writes == 27 && g_Run.Enable.Writes == 2);
    CHECK(io.DirectWrites == 2 && io.IndirectWrites == 31 && io.Refusals == 0);
    CHECK(g_Run.Init.SizeRates == 0xFFFFF070ul && g_Run.Init.PowerStates == 0xC0000000ul);
    CHECK(g_Run.Enable.HotPlugBefore == 0x10 && g_Run.Enable.HotPlugAfter == 0x80000010ul);
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x80000010ul);      // enabled, gating back on
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x10);              // the other endpoint untouched
    // The stop path: dce_aud_az_disable.
    g_Fake.Traced = 0;
    g_Expected = 0;
    ExpectDisable(0, 0x80000010ul);
    CHECK(Bc250DpAudioSetEnabled(&io, 0, 0, &g_Run.Clear) == 0);
    CHECK(TraceMatches(&g_Fake, "stop on DP0"));
    CHECK(g_Run.Clear.HotPlugBefore == 0x80000010ul && g_Run.Clear.HotPlugAfter == 0x10);
    CHECK(g_Fake.Protocol == 0);
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
    ExpectEnable(1);
    CHECK(TraceMatches(&g_Fake, "start on DP1"));
    CHECK(g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x10);              // hw_init's bracket closed, no enable
    CHECK(g_Fake.Ix[1][BC250_AZ_IX_HOT_PLUG_CONTROL] == 0x80000010ul);
}

// Write calls a group makes on a fresh unit A, INDEX selects included.
static unsigned long Calls(int group)
{
    BC250_AZ_IO io;

    UnitA(&g_Fake);
    Io(&io, &g_Fake);
    if (group == 0) Bc250DpAudioObserve(&io, &g_Run.Obs);
    if (group == 1) Bc250DpAudioHwInit(&io, &g_Run.Init);
    if (group == 2) Bc250DpAudioConfigure(&io, 0, &g_Run.Config);
    if (group == 3) Bc250DpAudioSetEnabled(&io, 0, 1, &g_Run.Enable);
    return g_Fake.Writes;
}

static void WriteFailures(void)
{
    unsigned long observe = Calls(0), init = Calls(1), configure = Calls(2), enable = Calls(3), k, bad = 0;

    CHECK(observe == 16 && init > 0 && configure > 0 && enable > 0);
    for (k = observe + 1; k <= observe + init + configure + enable; k++) {
        BC250_AZ_IO io;
        unsigned long groups = k <= observe + init ? 0 : k <= observe + init + configure ? 1 : 2, reason;

        UnitA(&g_Fake);
        g_Fake.FailWriteAt = k;
        Io(&io, &g_Fake);
        reason = Bc250DpAudioRun(&io, &g_Run);
        if (reason != BC250_DPAUDIO_REASON_WRITE_FAILED || g_Run.Groups != groups || g_Run.Status != FAKE_FAIL ||
            g_Run.ClearStatus != 0 || (g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL] & (HPC_AE | HPC_CGD)) != 0 ||
            g_Run.Wrote != 1) {
            printf("  write %lu failed: reason %lu groups %lu (expected %lu) status 0x%08lX clear 0x%08lX hpc 0x%08lX\n", k,
                   reason, g_Run.Groups, groups, (unsigned long)g_Run.Status, (unsigned long)g_Run.ClearStatus,
                   g_Fake.Ix[0][BC250_AZ_IX_HOT_PLUG_CONTROL]);
            bad++;
        }
    }
    CHECK(bad == 0);
}

static void Tables(void)
{
    BC250_AZ_IO io;
    unsigned long value = 0, i, writable = 0;

    // Only six offsets take a write: the two INDEX/DATA pairs and the two function parameters of hw_init.
    for (i = 0; i < BC250_MMIO_AUDIO_ALLOW_COUNT; i++) writable += (unsigned long)Bc250AzWriteAllowed(g_MmioAudioAllow[i]);
    CHECK(writable == BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT && writable == 6);
    CHECK(Bc250AzWriteAllowed(BC250_REG_DMU_AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DC_PINSTRAPS));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DP0_DP_SEC_CNTL));         // step 2 territory
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DCCG_AUDIO_DTO1_MODULE));
    CHECK(!Bc250AzWriteAllowed(BC250_REG_DMU_DIG0_AFMT_CNTL));
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
    // Every write-table index is readable, so each read-modify-write can read what it writes.
    for (i = 0; i < BC250_AZ_IX_WRITE_ALLOW_COUNT; i++) {
        unsigned long j, found = 0;
        for (j = 0; j < BC250_AZ_IX_READ_ALLOW_COUNT; j++) found |= g_AzIxReadAllow[j] == g_AzIxWriteAllow[i];
        CHECK(found);
    }
}

static void Reasons(void)
{
    unsigned long i, j;

    for (i = 0; i < BC250_DPAUDIO_REASON_COUNT; i++) {
        CHECK(Bc250DpAudioReasonText(i) != NULL && Bc250DpAudioReasonText(i)[0] != 0);
        for (j = 0; j < i; j++) CHECK(strcmp(Bc250DpAudioReasonText(i), Bc250DpAudioReasonText(j)) != 0);
    }
    CHECK(strcmp(Bc250DpAudioReasonText(BC250_DPAUDIO_REASON_COUNT), "unknown reason") == 0);
}
typedef char ReasonOkIsZero[BC250_DPAUDIO_REASON_OK == 0 ? 1 : -1];     // a zeroed record reads "OK" nowhere else

int main(void)
{
    Gate();
    ObserveBindsEverySlot();
    Refusals();
    Notes();
    StartOnStreamZero();
    StartOnStreamOne();
    WriteFailures();
    Tables();
    Reasons();
    printf("dpaudio: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
