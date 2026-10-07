// The SMU metrics table (0.7.215, docs/design/dpm.md "Power reading"): the package power, the two rails, and the
// table's own clock and temperatures, read once a second by the DPM governor thread and published for RUN_DPM ABI 3.
//
// What lives where:
//   driver/shim/bc250_smu_metrics.c   the message list, the decode and the reader's rules (host-tested)
//   driver/kmd/smu.c                  SmuReadMetrics: the three messages, under the one SMU owner's lock
//   this file                         the page, the registry gate, the cadence, the snapshot and the log
//
// Settings, REG_DWORD under Services\bc250kmd\Parameters:
//   EnableSmuMetrics   1 (the default, and what the INF and the release installer write) reads the table. 0 sends
//                      no metrics message at all and maps nothing: the driver then behaves as 0.7.214.1 did, and
//                      RUN_DPM ABI 3 reports state OFF. Read once per start.
//
// The safety rules, each one in code:
//   - Bounded. Every message is the transport's bounded poll (20 ms, smu.c). A refusal comes back at once.
//   - Rate-limited. At most one read per BC250_SMU_METRICS_PERIOD_MS (1 s), from the governor thread only, and none
//     while a power transition holds the governor (DpmTick returns before this file is called). The escape reads
//     the published copy and sends nothing.
//   - Never a retry loop. A refusal or a timeout ends the reader for the rest of this driver image's life, which is
//     this boot unless the driver itself is replaced (g_SmuMetricsRefused); three tables in a row that fail the
//     checks end it for this start. Either way the governor runs on as before, and the reading is "no reading".
//   - No new SMU writer. The messages go through the existing owner and its lock (smu.c), on their own allowlist,
//     with the arguments fixed by this page. The address messages go out once per owner start.
//   - The page. One page of the carve-out's top 2 MB (BC250_VRAM_SMU_TABLE_BELOW), outside Windows' memory segment
//     and clear of the GART table, the PSP's pages and the IP discovery table, mapped uncached for the start.
#include "bc250kmd.h"

#define SMU_METRICS_SETTING L"EnableSmuMetrics"

// A refusal latches here for as long as this driver image is loaded: a later start of the adapter (a PnP stop, a
// display-only restart) sends nothing again. Interlocked only.
static volatile LONG g_SmuMetricsRefused;

static const char* const g_SmuMetricsState[] = { "off", "waiting", "ok", "refused", "no table", "bad table" };
C_ASSERT(ARRAYSIZE(g_SmuMetricsState) == BC250_SMU_METRICS_STATE_COUNT);

static const char* StateText(ULONG State)
{
    return State < ARRAYSIZE(g_SmuMetricsState) ? g_SmuMetricsState[State] : "?";
}

static ULONGLONG NowMs(void)
{
    return KeQueryInterruptTime() / 10000ull;
}

static void Publish(BC250_SMU_METRICS* M)
{
    BC250_SMU_METRICS_SNAP snap;
    KIRQL irql;
    RtlZeroMemory(&snap, sizeof(snap));
    snap.State = M->Reader.state;
    snap.Reads = M->Reader.reads;
    snap.Failures = M->Reader.failures;
    snap.LastStatus = M->Reader.last_status;
    snap.LastParse = M->Reader.last_parse;
    snap.HasTable = M->Reader.has_last ? TRUE : FALSE;
    // The reader's clock is NowMs; the snapshot keeps interrupt time so the escape can age it without the reader.
    snap.TableAt = M->Reader.has_last ? M->Reader.last_ok_ms * 10000ull : 0;
    snap.Table = M->Reader.last;
    KeAcquireSpinLock(&M->SnapLock, &irql);
    M->Snap = snap;
    KeReleaseSpinLock(&M->SnapLock, irql);
}

void SmuMetricsInitialize(BC250_DEVICE* Device)
{
    BC250_SMU_METRICS* m = &Device->SmuMetrics;
    RtlZeroMemory(m, sizeof(*m));
    KeInitializeSpinLock(&m->SnapLock);
    bc250_smu_metrics_reader_init(&m->Reader, 0, 0, 0);
    m->Snap.State = BC250_SMU_METRICS_STATE_OFF;
}

void SmuMetricsStart(BC250_DEVICE* Device)
{
    BC250_SMU_METRICS* m = &Device->SmuMetrics;
    BOOLEAN refused = InterlockedCompareExchange(&g_SmuMetricsRefused, 0, 0) != 0;
    BOOLEAN tableOk = FALSE;
    ULONGLONG offset = 0;

    SmuMetricsStop(Device);             // a start after a start that never stopped: no mapping leaks
    m->Enabled = GuardReadSetting(SMU_METRICS_SETTING, 1) != 0;
    m->LoggedEnd = FALSE;
    m->TableMc = 0;
    m->Physical.QuadPart = 0;
    if (m->Enabled && !refused && Device->FullWddm && Device->Smu.Online && Device->VramEnabled &&
        Device->VramLength > BC250_VRAM_TOP_RESERVED) {
        offset = Device->VramLength - BC250_VRAM_SMU_TABLE_BELOW;
        m->TableMc = Device->VramMcBase + offset;
        if (bc250_smu_metrics_table_allowed(m->TableMc, Device->VramMcBase, Device->VramLength)) {
            m->Physical.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)offset;
            m->Page = (volatile ULONG*)MmMapIoSpaceEx(m->Physical, BC250_SMU_METRICS_PAGE, PAGE_READWRITE | PAGE_NOCACHE);
            tableOk = m->Page != NULL;
        }
    }
    bc250_smu_metrics_reader_init(&m->Reader, m->Enabled, tableOk, refused);
    // The first read waits one period, so the start transaction and the governor's first ticks go out alone.
    m->Reader.next_ms = NowMs() + BC250_SMU_METRICS_PERIOD_MS;
    Publish(m);
    if (!m->Enabled)
        GuardLog("smu metrics: off (EnableSmuMetrics 0): no metrics message is sent");
    else if (refused)
        GuardLog("smu metrics: off: the firmware refused the table earlier in this boot");
    else if (!tableOk)
        GuardLog("smu metrics: no table page (full %u smu %u vram %u, mc 0x%llX): no reading", Device->FullWddm,
                 Device->Smu.Online, Device->VramEnabled, m->TableMc);
    else
        GuardLog("smu metrics: table page MC 0x%llX, physical 0x%llX, one read every %lu ms", m->TableMc,
                 (ULONGLONG)m->Physical.QuadPart, (ULONG)BC250_SMU_METRICS_PERIOD_MS);
}

void SmuMetricsStop(BC250_DEVICE* Device)
{
    BC250_SMU_METRICS* m = &Device->SmuMetrics;
    if (m->Page != NULL) {
        MmUnmapIoSpace((PVOID)m->Page, BC250_SMU_METRICS_PAGE);
        m->Page = NULL;
        GuardLog("smu metrics: stop after %lu tables, %lu failures, state %s", m->Reader.reads, m->Reader.failures,
                 StateText(m->Reader.state));
    }
}

void SmuMetricsSample(BC250_DEVICE* Device)
{
    BC250_SMU_METRICS* m = &Device->SmuMetrics;
    UCHAR copy[BC250_SMU_METRICS_BYTES];
    ULONGLONG now = NowMs();
    int status, ended;

    C_ASSERT(BC250_SMU_METRICS_BYTES % 4u == 0);
    if (m->Page == NULL || !bc250_smu_metrics_due(&m->Reader, now)) return;
    RtlZeroMemory(copy, sizeof(copy));
    status = SmuReadMetrics(&Device->Smu, m->TableMc, m->Page, copy, sizeof(copy));
    ended = bc250_smu_metrics_record(&m->Reader, NowMs(), status, copy, sizeof(copy));
    if (ended) InterlockedExchange(&g_SmuMetricsRefused, 1);
    Publish(m);
    if (!m->LoggedEnd && m->Reader.state != BC250_SMU_METRICS_STATE_OK &&
        m->Reader.state != BC250_SMU_METRICS_STATE_WAITING) {
        m->LoggedEnd = TRUE;
        GuardLog("smu metrics: %s, status %d parse %lu, %lu tables: off until next %s", StateText(m->Reader.state),
                 m->Reader.last_status, m->Reader.last_parse, m->Reader.reads, ended ? "boot" : "start");
    }
}

// Clocks (0.7.216.15, RUN_DPM ABI 4) is filled from the same table as Out, or zeroed with it.
BOOLEAN SmuMetricsFill(BC250_DEVICE* Device, BC250_DPM_METRICS* Out, _Out_opt_ BC250_DPM_CLOCKS* Clocks)
{
    BC250_SMU_METRICS* m = &Device->SmuMetrics;
    BC250_SMU_METRICS_SNAP snap;
    ULONGLONG now = KeQueryInterruptTime(), age;
    ULONG i;
    KIRQL irql;

    KeAcquireSpinLock(&m->SnapLock, &irql);
    snap = m->Snap;
    KeReleaseSpinLock(&m->SnapLock, irql);
    RtlZeroMemory(Out, sizeof(*Out));
    if (Clocks != NULL) RtlZeroMemory(Clocks, sizeof(*Clocks));
    Out->MetricsState = snap.State;
    Out->MetricsReads = snap.Reads;
    Out->MetricsFailures = snap.Failures;
    if (!snap.HasTable) return FALSE;
    age = now > snap.TableAt ? (now - snap.TableAt) / 10000ull : 0;
    Out->MetricsAgeMs = age > MAXULONG ? MAXULONG : (ULONG)age;
    Out->SocketPowerMw = snap.Table.socket_mw;
    Out->SocketPowerAvgMw = snap.Table.socket_avg_mw;
    Out->GfxPowerMw = snap.Table.gfx_mw;
    Out->SocPowerMw = snap.Table.soc_mw;
    Out->GfxMv = snap.Table.gfx_mv;
    Out->SocMv = snap.Table.soc_mv;
    Out->GfxMHz = snap.Table.gfx_mhz;
    Out->GfxTemperatureCc = snap.Table.gfx_cc;
    Out->SocTemperatureCc = snap.Table.soc_cc;
    Out->ThrottlerStatus = snap.Table.throttler;
    if (Clocks != NULL) {
        Clocks->SocclkMHz = snap.Table.socclk_mhz;
        Clocks->MemclkMHz = snap.Table.memclk_mhz;
        Clocks->VclkMHz = snap.Table.vclk_mhz;
        Clocks->DclkMHz = snap.Table.dclk_mhz;
        for (i = 0; i < BC250_SMU_METRICS_L3; i++) Clocks->L3MHz[i] = snap.Table.l3_mhz[i];
        for (i = 0; i < BC250_DPM_CPU_CLOCKS; i++) Clocks->CpuCoreMHz[i] = snap.Table.core_mhz[i];
    }
    return snap.State == BC250_SMU_METRICS_STATE_OK && age <= BC250_SMU_METRICS_FRESH_MS;
}

void SmuMetricsLogLine(BC250_DEVICE* Device, _In_z_ const char* What)
{
    BC250_DPM_METRICS x;
    BC250_DPM_CLOCKS c;
    ULONG i, coreTop = 0, coreLow = 0;
    BOOLEAN fresh = SmuMetricsFill(Device, &x, &c);
    if (x.MetricsState == BC250_SMU_METRICS_STATE_OFF) return;
    if (!fresh) {
        GuardLog("smu metrics: %s %s, no fresh table (%lu tables, %lu failures)", What, StateText(x.MetricsState),
                 x.MetricsReads, x.MetricsFailures);
        return;
    }
    // Two lines: one would pass the log ring's 160 bytes at its worst case (tools/quality/guardlog_width.py).
    GuardLog("smu metrics: %s %lu.%01lu W avg %lu.%01lu W, gfx %lu.%01lu W soc %lu.%01lu W", What,
             x.SocketPowerMw / 1000, x.SocketPowerMw % 1000 / 100, x.SocketPowerAvgMw / 1000,
             x.SocketPowerAvgMw % 1000 / 100, x.GfxPowerMw / 1000, x.GfxPowerMw % 1000 / 100, x.SocPowerMw / 1000,
             x.SocPowerMw % 1000 / 100);
    GuardLog("smu metrics: %s gfx %lu mV %lu MHz %lu.%lu C soc %lu mV %lu.%lu C thr %lX", What, x.GfxMv, x.GfxMHz,
             x.GfxTemperatureCc / 100, x.GfxTemperatureCc % 100 / 10, x.SocMv, x.SocTemperatureCc / 100,
             x.SocTemperatureCc % 100 / 10, x.ThrottlerStatus);
    // The SoC side and the processor as the table sees them (0.7.216.15, K137): a fresh boot against the state
    // after a GPU stop. A third line, by the same width rule.
    for (i = 0; i < BC250_DPM_CPU_CLOCKS; i++) {
        if (c.CpuCoreMHz[i] > coreTop) coreTop = c.CpuCoreMHz[i];
        if (c.CpuCoreMHz[i] && (!coreLow || c.CpuCoreMHz[i] < coreLow)) coreLow = c.CpuCoreMHz[i];
    }
    GuardLog("smu metrics: %s MHz soc %lu mem %lu v/d %lu/%lu l3 %lu/%lu cpu %lu-%lu", What, c.SocclkMHz,
             c.MemclkMHz, c.VclkMHz, c.DclkMHz, c.L3MHz[0], c.L3MHz[1], coreLow, coreTop);
}
