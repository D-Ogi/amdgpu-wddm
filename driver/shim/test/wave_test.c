/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* CPU-only model of GFX10 bank and wave selection. No lab or host MMIO. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bc250_wave.h"
#include "nv.h"
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

typedef struct { const char *Name; unsigned long Offset; } BC250_DCN_REG_INFO;
#define BC250_REGS_WITH_TABLES
#include "../../kmd/regs.generated.h"

/* Wire layout is the same in the CLI, the kernel build and the host model. */
typedef char wave_header_size_check[sizeof(BC250_WAVE_HEADER) == 128 ? 1 : -1];
typedef char wave_record_size_check[sizeof(BC250_WAVE_RECORD) == 96 ? 1 : -1];
typedef char wave_record_raw_offset_check[offsetof(BC250_WAVE_RECORD, Raw) == 32 ? 1 : -1];
typedef char wave_header_clock_offset_check[offsetof(BC250_WAVE_HEADER, StartTime100ns) == 64 ? 1 : -1];
typedef char wave_snapshot_offset_check[offsetof(BC250_WAVE_SNAPSHOT, Waves) == 128 ? 1 : -1];
typedef char wave_escape_offset_check[offsetof(BC250_ESCAPE_WAVES, Snapshot) == 32 ? 1 : -1];

static BC250_WAVE_SNAPSHOT snapshot;
static struct amdgpu_device device;
static unsigned int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %u: %s\n", __LINE__, #x); } } while (0)
static struct {
    u32 grbm, index, cc[2][2], user[2][2], spi[2][2], valid_wgps[2][2];
    u32 operations, fail_at, poison_at, fault, forbidden, restore_attempts;
    u32 stop_calls, budget_at, max_slot, data_reads, status_reads, identity_reads;
    u32 full, active, retire, identity_change;
    u32 mask_logs;
    char masks[4][160];
} m;
static u32 ix_grbm, ix_index, ix_data, ix_cc, ix_user, ix_spi;

static u32 broadcast(void)
{
    return GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
}

static int allowed(u32 index)
{
    u32 i;
    for (i = 0; i < BC250_MMIO_GFX_ALLOW_COUNT; i++)
        if (g_MmioGfxAllow[i] == index * 4u) return 1;
    return 0;
}

static int access(u32 index)
{
    m.operations++;
    if (!allowed(index)) m.forbidden++;
    if (m.operations == m.fail_at) m.fault = 1;
    return !m.fault;
}

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int index)
{
    u32 se, sh, instance, wgp, simd, slot, address, identity;
    (void)adev;
    if (!access(index)) return 0xffffffffu;
    if (m.operations == m.poison_at) return 0xffffffffu;
    if (index == ix_grbm) return m.grbm;
    if (index == ix_index) return m.index;
    se = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SE_INDEX);
    sh = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, SA_INDEX);
    if (se >= 2 || sh >= 2) { m.forbidden++; return 0xffffffffu; }
    if (index == ix_cc) return m.cc[se][sh];
    if (index == ix_user) return m.user[se][sh];
    if (index == ix_spi) return m.spi[se][sh];
    if (index != ix_data) { m.forbidden++; return 0xffffffffu; }
    m.data_reads++;
    instance = REG_GET_FIELD(m.grbm, GRBM_GFX_INDEX, INSTANCE_INDEX);
    wgp = instance >> 2;
    simd = instance & 3;
    slot = REG_GET_FIELD(m.index, SQ_IND_INDEX, WAVE_ID);
    address = REG_GET_FIELD(m.index, SQ_IND_INDEX, INDEX);
    if (wgp >= 5 || slot >= 20 ||
        !(m.valid_wgps[se][sh] & (1u << wgp)) ||
        (m.grbm & broadcast())) { m.forbidden++; return 0xffffffffu; }
    if (slot > m.max_slot) m.max_slot = slot;
    if (address == ixSQ_WAVE_STATUS) {
        m.status_reads++;
        if (!m.full && !(m.active && se == 0 && sh == 0 && wgp == 0 && simd == 0 && slot == 0))
            return 0;
        if (m.retire && m.status_reads == 2) return 0;
        return SQ_WAVE_STATUS__VALID_MASK | SQ_WAVE_STATUS__IN_BARRIER_MASK;
    }
    if (address == ixSQ_WAVE_HW_ID1) {
        m.identity_reads++;
        identity = REG_SET_FIELD(0, SQ_WAVE_HW_ID1, SE_ID, se);
        identity = REG_SET_FIELD(identity, SQ_WAVE_HW_ID1, SA_ID, sh);
        identity = REG_SET_FIELD(identity, SQ_WAVE_HW_ID1, WGP_ID, wgp);
        identity = REG_SET_FIELD(identity, SQ_WAVE_HW_ID1, SIMD_ID, simd);
        identity = REG_SET_FIELD(identity, SQ_WAVE_HW_ID1, WAVE_ID, slot);
        if (m.identity_change && m.identity_reads == 2)
            identity = REG_SET_FIELD(identity, SQ_WAVE_HW_ID1, WAVE_ID, slot + 1);
        return identity;
    }
    if (address == ixSQ_WAVE_HW_ID2) return REG_SET_FIELD(0, SQ_WAVE_HW_ID2, VM_ID, 3);
    if (address == ixSQ_WAVE_IB_STS2) return SQ_WAVE_IB_STS2__WAVE64_MASK;
    if (address == ixSQ_WAVE_PC_LO) return 0x12345678u;
    if (address == ixSQ_WAVE_PC_HI) return 0x4321u;
    if (address == ixSQ_WAVE_EXEC_LO) return 0xf0f0u;
    if (address == ixSQ_WAVE_EXEC_HI) return 0x1010u;
    return address;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{
    (void)adev;
    if (index == ix_grbm && value == broadcast()) m.restore_attempts++;
    if (!access(index)) return; /* Like the KMD fault latch: even cleanup is blocked. */
    if (index == ix_grbm) m.grbm = value;
    else if (index == ix_index) m.index = value;
    else m.forbidden++; /* Including SQ_IND_DATA, SQ_CMD and topology writes. */
}

void bc250_shim_udelay(unsigned int usec) { (void)usec; }
void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
    va_list args;
    (void)level; (void)dev;
    if (strncmp(fmt, "waves mask ", 11)) return;
    CHECK(m.mask_logs < 4);
    va_start(args, fmt);
    vsnprintf(m.masks[m.mask_logs++], sizeof(m.masks[0]), fmt, args);
    va_end(args);
}
void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{ (void)adev; (void)index; (void)value; m.forbidden++; }
int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain,
                        unsigned int size, unsigned int align, struct bc250_mem *out)
{ (void)adev; (void)domain; (void)size; (void)align; (void)out; m.forbidden++; return -1; }
void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *mem)
{ (void)adev; (void)mem; m.forbidden++; }

static unsigned int stop(void *context)
{
    (void)context;
    m.stop_calls++;
    if (m.fault) return BC250_WAVE_READ_ERROR;
    if (m.budget_at && m.stop_calls >= m.budget_at) return BC250_WAVE_BUDGET_EXPIRED;
    return 0;
}

static void reset(u32 active)
{
    u32 se, sh;
    memset(&m, 0, sizeof(m));
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.Header.Seq = 77;
    snapshot.Header.Kind = BC250_WAVE_KIND_PRE_KILL;
    m.grbm = broadcast();
    for (se = 0; se < 2; se++)
        for (sh = 0; sh < 2; sh++) {
            m.cc[se][sh] = REG_SET_FIELD(0, CC_GC_SHADER_ARRAY_CONFIG, INACTIVE_WGPS, ~active & 31u);
            m.spi[se][sh] = REG_SET_FIELD(0, SPI_PG_ENABLE_STATIC_WGP_MASK, WGP_MASK, active);
            m.valid_wgps[se][sh] = active;
        }
    device.ip_versions[GC_HWIP][0] = IP_VERSION(10, 1, 3);
    device.gfx.config.max_shader_engines = 2;
    device.gfx.config.max_sh_per_se = 2;
}

static int run(void) { return bc250_gfx_read_waves(&device, &snapshot, stop, NULL); }

int main(void)
{
    u32 i, operations, stop_calls;
    struct amdgpu_device *adev = &device; /* SOC15_REG_OFFSET uses this name. */
    memset(&device, 0, sizeof(device));
    cyan_skillfish_reg_base_init(&device);
    ix_grbm = SOC15_REG_OFFSET(GC, 0, mmGRBM_GFX_INDEX);
    ix_index = SOC15_REG_OFFSET(GC, 0, mmSQ_IND_INDEX);
    ix_data = SOC15_REG_OFFSET(GC, 0, mmSQ_IND_DATA);
    ix_cc = SOC15_REG_OFFSET(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
    ix_user = SOC15_REG_OFFSET(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);
    ix_spi = SOC15_REG_OFFSET(GC, 0, mmSPI_PG_ENABLE_STATIC_WGP_MASK);
    CHECK(bc250_gfx_wave_broadcast_value() == broadcast());

    reset(31);
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 0);
    CHECK(snapshot.Header.SlotsVisited == 1600); CHECK(snapshot.Header.WgpVisited == 20);
    CHECK(snapshot.Header.Flags == BC250_WAVE_COMPLETE); CHECK(m.max_slot == 19);
    CHECK(m.forbidden == 0); CHECK(m.grbm == broadcast()); CHECK(m.restore_attempts == 1);
    CHECK(snapshot.Header.Seq == 77); CHECK(snapshot.Header.Kind == BC250_WAVE_KIND_PRE_KILL);
    CHECK(m.mask_logs == 4 && strstr(m.masks[0], "cc_active1f spi_active1f used1f user_ignored0") != NULL);

    reset(0);
    CHECK(run() == -1); CHECK(snapshot.Header.WaveCount == 0);
    CHECK(snapshot.Header.SlotsVisited == 0); CHECK(snapshot.Header.WgpVisited == 0);
    CHECK(snapshot.Header.Flags == BC250_WAVE_READ_ERROR); CHECK(m.grbm == broadcast());

    reset(31); m.full = 1;
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 1600);
    CHECK(snapshot.Waves[1599].Se == 1 && snapshot.Waves[1599].Sh == 1 && snapshot.Waves[1599].Wgp == 4);
    CHECK(snapshot.Waves[1599].Simd == 3 && snapshot.Waves[1599].Slot == 19);
    CHECK(snapshot.Waves[0].Raw[Bc250WavePcLo] == 0x12345678u);
    CHECK(snapshot.Waves[0].Raw[Bc250WavePcHi] == 0x4321u);
    CHECK(snapshot.Waves[0].Raw[Bc250WaveExecLo] == 0xf0f0u);
    CHECK(snapshot.Waves[0].Raw[Bc250WaveExecHi] == 0x1010u);
    CHECK(snapshot.Waves[0].Raw[Bc250WaveIbSts2] == SQ_WAVE_IB_STS2__WAVE64_MASK);
    CHECK(snapshot.Waves[0].StatusAfter == snapshot.Waves[0].Raw[Bc250WaveStatus]);
    CHECK(m.forbidden == 0);

    reset(0x15); m.full = 1;
    m.user[1][0] = REG_SET_FIELD(0, GC_USER_SHADER_ARRAY_CONFIG, INACTIVE_WGPS, 4);
    m.spi[1][0] = REG_SET_FIELD(0, SPI_PG_ENABLE_STATIC_WGP_MASK, WGP_MASK, 0x11);
    m.valid_wgps[1][0] = 0x11;
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 880);
    CHECK(snapshot.Header.WgpMask[1][0] == 0x11); CHECK(m.forbidden == 0);
    for (i = 0; i < snapshot.Header.WaveCount; i++)
        CHECK(snapshot.Waves[i].Wgp == 0 || snapshot.Waves[i].Wgp == 2 || snapshot.Waves[i].Wgp == 4);

    /* USER can be all ones without a backend error, as UMR documents. Retain
     * CC holes, and record the mask choice rather than certifying empty GPU. */
    reset(0x15); m.full = 1; m.user[0][0] = 0xffffffffu;
    m.spi[0][0] = 0; /* This bank must use CC even without the SPI fallback. */
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 960);
    CHECK(snapshot.Header.WgpMask[0][0] == 0x15 && m.forbidden == 0);
    CHECK(strstr(m.masks[0], "userffffffff") != NULL);
    CHECK(strstr(m.masks[0], "used15 user_ignored1") != NULL);

    /* A dispatch-enabled WGP is not hidden by the enumeration view. */
    reset(1); m.full = 1;
    m.spi[0][0] = REG_SET_FIELD(0, SPI_PG_ENABLE_STATIC_WGP_MASK, WGP_MASK, 3);
    m.valid_wgps[0][0] = 3;
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 400);
    CHECK(snapshot.Header.WgpMask[0][0] == 3 && m.forbidden == 0);
    CHECK(strstr(m.masks[0], "cc_active1 spi_active3 used3") != NULL);

    reset(1); m.spi[0][0] = 0xffffffffu;
    CHECK(run() == -1); CHECK(snapshot.Header.InvalidReads == 1);
    CHECK(snapshot.Header.WaveCount == 0 && m.mask_logs == 0 && m.grbm == broadcast());

    reset(1); m.active = 1; m.retire = 1; m.identity_change = 1;
    CHECK(run() == 0); CHECK(snapshot.Header.WaveCount == 1);
    CHECK(snapshot.Waves[0].StatusAfter == 0);
    CHECK(snapshot.Waves[0].HwId1After != snapshot.Waves[0].Raw[Bc250WaveHwId1]);

    reset(1); m.active = 1;
    CHECK(run() == 0); operations = m.operations; stop_calls = m.stop_calls;
    for (i = 1; i <= operations; i++) {
        reset(1); m.active = 1; m.fail_at = i;
        CHECK(run() == -1); CHECK(snapshot.Header.Flags & BC250_WAVE_READ_ERROR);
        CHECK(!(snapshot.Header.Flags & BC250_WAVE_COMPLETE));
        CHECK(m.restore_attempts == 1); CHECK(m.forbidden == 0);
    }
    for (i = 1; i <= stop_calls; i++) {
        reset(1); m.active = 1; m.budget_at = i;
        CHECK(run() == -1); CHECK(snapshot.Header.Flags & BC250_WAVE_BUDGET_EXPIRED);
        CHECK(!(snapshot.Header.Flags & BC250_WAVE_COMPLETE));
        CHECK(m.grbm == broadcast()); CHECK(m.restore_attempts == 1); CHECK(m.forbidden == 0);
    }
    /* No callback fault: reject an all-ones read rather than calling it VALID. */
    reset(1); m.poison_at = 2;
    CHECK(run() == -1); CHECK(snapshot.Header.InvalidReads == 1); CHECK(snapshot.Header.WaveCount == 0);
    CHECK(m.grbm == broadcast());
    reset(1); m.poison_at = 7;
    CHECK(run() == -1); CHECK(snapshot.Header.InvalidReads == 1); CHECK(snapshot.Header.WaveCount == 0);
    CHECK(m.grbm == broadcast());

    reset(1); device.ip_versions[GC_HWIP][0] = IP_VERSION(10, 3, 0);
    CHECK(run() == -1); CHECK(m.operations == 0);
    reset(1); device.gfx.config.max_shader_engines = 3;
    CHECK(run() == -1); CHECK(m.operations == 0);
    reset(1); device.gfx.config.max_sh_per_se = 1;
    CHECK(run() == -1); CHECK(m.operations == 0);
    reset(1); CHECK(bc250_gfx_read_waves(&device, &snapshot, NULL, NULL) == -1); CHECK(m.operations == 0);
    CHECK(bc250_gfx_read_waves(NULL, &snapshot, stop, NULL) == -1);
    CHECK(bc250_gfx_read_waves(&device, NULL, stop, NULL) == -1);

    printf("wave tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
