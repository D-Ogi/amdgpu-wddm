/* Host-side ABI validation, named decode and explicit positive controls.
 * The masks come from the AMD GFX10 header, through the generated table. */
#ifndef BC250_CLI_WAVE_DECODE_H
#define BC250_CLI_WAVE_DECODE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../../../driver/kmd/wave_snapshot.h"

typedef struct BC250_WAVE_FIELD {
    unsigned Register;
    const char *Name;
    uint32_t Mask;
    unsigned Shift;
} BC250_WAVE_FIELD;
#include "wave_fields.generated.h"

typedef char WaveHeaderSizeCheck[sizeof(BC250_WAVE_HEADER) == 128 ? 1 : -1];
typedef char WaveRecordSizeCheck[sizeof(BC250_WAVE_RECORD) == 96 ? 1 : -1];

static __inline uint32_t WaveFieldValue(const BC250_WAVE_FIELD *field, uint32_t value)
{
    return (value & field->Mask) >> field->Shift;
}

/* Use memcpy for the header and records: registry bytes need not be aligned.
 * Size and count arithmetic is bounded before any record access. */
static __inline int WaveValidateBlob(const void *blob, size_t bytes, const char **error)
{
    BC250_WAVE_HEADER h;
    size_t expected;
    uint32_t i, se, sh;
#define WAVE_REJECT(why) do { *error = (why); return 0; } while (0)
    if (!blob || bytes < sizeof(h)) WAVE_REJECT("short header");
    memcpy(&h, blob, sizeof(h));
    if (h.AbiVersion != BC250_WAVE_ABI_VERSION) WAVE_REJECT("unsupported ABI version");
    if (h.WaveCount > BC250_WAVE_MAX_RECORDS) WAVE_REJECT("wave count exceeds ABI limit");
    expected = sizeof(h) + (size_t)h.WaveCount * sizeof(BC250_WAVE_RECORD);
    if (bytes != expected || h.Bytes != expected) WAVE_REJECT("byte count does not match wave count");
    if (h.Kind != BC250_WAVE_KIND_LIVE && h.Kind != BC250_WAVE_KIND_PRE_KILL) WAVE_REJECT("unknown capture kind");
    if (h.Flags & ~(BC250_WAVE_COMPLETE | BC250_WAVE_TRUNCATED | BC250_WAVE_READ_ERROR | BC250_WAVE_BUDGET_EXPIRED))
        WAVE_REJECT("unknown capture flags");
    if (h.Reserved0 || h.Reserved[0] || h.Reserved[1] || h.Reserved[2] || h.Reserved[3])
        WAVE_REJECT("nonzero reserved header fields");
    if (h.EndTime100ns < h.StartTime100ns) WAVE_REJECT("capture time moved backwards");
    if (h.SeCount > 2 || h.ShPerSe > 2 || h.WgpPerSh > 16 || h.SimdPerWgp > 4 || h.WaveSlotsPerSimd > 32)
        WAVE_REJECT("topology exceeds GFX10 selector or ABI limits");
    if ((h.Flags & BC250_WAVE_COMPLETE) || h.WaveCount) {
        if (!h.SeCount || !h.ShPerSe || !h.WgpPerSh || !h.SimdPerWgp || !h.WaveSlotsPerSimd)
            WAVE_REJECT("capture has no topology");
    }
    if (h.WaveCount > h.SlotsVisited) WAVE_REJECT("more waves than visited slots");
    if (h.SlotsVisited > h.SeCount * h.ShPerSe * h.WgpPerSh * h.SimdPerWgp * h.WaveSlotsPerSimd)
        WAVE_REJECT("visited slot count exceeds topology");
    if (h.WgpVisited > h.SeCount * h.ShPerSe * h.WgpPerSh) WAVE_REJECT("visited WGP count exceeds topology");
    for (se = 0; se < 2; ++se) for (sh = 0; sh < 2; ++sh) {
        uint32_t mask = h.WgpMask[se][sh];
        if ((se >= h.SeCount || sh >= h.ShPerSe) && mask) WAVE_REJECT("WGP mask exceeds SE/SH topology");
        if (mask & ~((1u << h.WgpPerSh) - 1u)) WAVE_REJECT("WGP mask exceeds WGP topology");
    }
    for (i = 0; i < h.WaveCount; ++i) {
        BC250_WAVE_RECORD r;
        memcpy(&r, (const unsigned char *)blob + sizeof(h) + (size_t)i * sizeof(r), sizeof(r));
        if (r.Se >= h.SeCount || r.Sh >= h.ShPerSe || r.Wgp >= h.WgpPerSh ||
            r.Simd >= h.SimdPerWgp || r.Slot >= h.WaveSlotsPerSimd)
            WAVE_REJECT("record selector exceeds topology");
        if (!(h.WgpMask[r.Se][r.Sh] & (1u << r.Wgp))) WAVE_REJECT("record selects a disabled WGP");
        if (!(r.Raw[Bc250WaveStatus] & SQ_WAVE_STATUS__VALID_MASK)) WAVE_REJECT("record was not a valid wave");
    }
    *error = NULL;
    return 1;
#undef WAVE_REJECT
}

static __inline uint64_t WavePc(const BC250_WAVE_RECORD *r)
{
    return ((uint64_t)(r->Raw[Bc250WavePcHi] & SQ_WAVE_PC_HI__PC_HI_MASK) << 32) |
        (r->Raw[Bc250WavePcLo] & SQ_WAVE_PC_LO__PC_LO_MASK);
}

static __inline int WaveIdentityStable(const BC250_WAVE_RECORD *r)
{
    return r->Raw[Bc250WaveStatus] == r->StatusAfter && r->Raw[Bc250WaveHwId1] == r->HwId1After &&
        r->Raw[Bc250WaveHwId2] == r->HwId2After && !!(r->StatusAfter & SQ_WAVE_STATUS__VALID_MASK);
}

/* Called only after WaveValidateBlob, on aligned storage. The positive PC
 * control permits unrelated desktop waves, but requires at least one stable
 * wave in the supplied half-open GPU virtual address interval. */
static __inline int WaveExpectation(const BC250_WAVE_SNAPSHOT *s, int idle, int pc,
                                    uint64_t first, uint64_t end, uint32_t *matches)
{
    uint32_t i, se, sh, enabled = 0;
    *matches = 0;
    if (s->Header.Result || s->Header.Flags != BC250_WAVE_COMPLETE || s->Header.InvalidReads)
        return 0;
    for (se = 0; se < s->Header.SeCount; ++se) for (sh = 0; sh < s->Header.ShPerSe; ++sh) {
        uint32_t mask = s->Header.WgpMask[se][sh];
        while (mask) { ++enabled; mask &= mask - 1; }
    }
    if (!enabled || s->Header.WgpVisited != enabled ||
        s->Header.SlotsVisited != enabled * s->Header.SimdPerWgp * s->Header.WaveSlotsPerSimd)
        return 0;
    if (idle) return s->Header.WaveCount == 0;
    if (!pc) return 1;
    if (first >= end) return 0;
    for (i = 0; i < s->Header.WaveCount; ++i) {
        const BC250_WAVE_RECORD *r = &s->Waves[i];
        uint64_t address = WavePc(r);
        if (WaveIdentityStable(r) && address >= first && address < end) ++*matches;
    }
    return *matches != 0;
}

static __inline void WavePrintRegister(unsigned index, uint32_t raw, const char *suffix)
{
    size_t f;
    printf("  %s%s raw=0x%08X", WaveRegisterNames[index], suffix, (unsigned)raw);
    for (f = 0; f < sizeof(WaveFields) / sizeof(WaveFields[0]); ++f) {
        const BC250_WAVE_FIELD *field = &WaveFields[f];
        if (field->Register == index)
            printf(" %s=0x%X", field->Name, (unsigned)WaveFieldValue(field, raw));
    }
    printf("\n");
}

static __inline void WavePrint(const BC250_WAVE_SNAPSHOT *s)
{
    const BC250_WAVE_HEADER *h = &s->Header;
    uint32_t i;
    printf("wave snapshot abi=%u bytes=%u driver=0x%08X kind=%s result=0x%08X flags=0x%X\n",
           h->AbiVersion, h->Bytes, h->DriverVersion, h->Kind ? "pre-kill" : "live", (unsigned)h->Result, h->Flags);
    printf("waves=%u slots_visited=%u wgp_visited=%u invalid_reads=%u start_100ns=%llu end_100ns=%llu\n",
           h->WaveCount, h->SlotsVisited, h->WgpVisited, h->InvalidReads,
           (unsigned long long)h->StartTime100ns, (unsigned long long)h->EndTime100ns);
    printf("topology se=%u sh_per_se=%u wgp_per_sh=%u simd_per_wgp=%u slots_per_simd=%u\n",
           h->SeCount, h->ShPerSe, h->WgpPerSh, h->SimdPerWgp, h->WaveSlotsPerSimd);
    printf("wgp_masks=%08X,%08X;%08X,%08X recovery_seq=%u fence=%u vmid=%u fault_offset=0x%X\n",
           h->WgpMask[0][0], h->WgpMask[0][1], h->WgpMask[1][0], h->WgpMask[1][1],
           h->Seq, h->Fence, h->Vmid, h->FaultOffset);
    printf("WARNING: sequential live reads, not an atomic context image. Equal rechecks do not prove atomicity.\n");
    for (i = 0; i < h->WaveCount; ++i) {
        const BC250_WAVE_RECORD *r = &s->Waves[i];
        unsigned reg;
        uint64_t exec = ((uint64_t)r->Raw[Bc250WaveExecHi] << 32) | r->Raw[Bc250WaveExecLo];
        printf("wave[%u] SE=%u SH=%u WGP=%u SIMD=%u slot=%u PC=0x%012llX EXEC=0x%016llX recheck=%s\n",
               i, r->Se, r->Sh, r->Wgp, r->Simd, r->Slot, (unsigned long long)WavePc(r),
               (unsigned long long)exec, WaveIdentityStable(r) ? "unchanged" : "CHANGED");
        for (reg = 0; reg < Bc250WaveRegisterCount; ++reg) WavePrintRegister(reg, r->Raw[reg], "");
        WavePrintRegister(Bc250WaveStatus, r->StatusAfter, "_AFTER");
        WavePrintRegister(Bc250WaveHwId1, r->HwId1After, "_AFTER");
        WavePrintRegister(Bc250WaveHwId2, r->HwId2After, "_AFTER");
    }
}
#endif
