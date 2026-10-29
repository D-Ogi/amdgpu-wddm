/* The SMU metrics binding (driver/kmd/smu_metrics.c, KMD 0.7.215), compiled from the shipping file against
 * smu_metrics_native_mock.h: the registry gate, the page, the cadence, the refusal latch, the published snapshot,
 * the escape's copy and the log.
 *
 *   pwsh driver\shim\test\run_smu_metrics.ps1
 */
#define WIN32_NO_STATUS
#include "smu_metrics-native.inc"

/* The page against the reservation table: inside the GART window, clear of the PSP's three pages (end - 0x19000 ..
 * end - 0x16000, psp.c) and of the last 64 KB (the IP discovery table). */
typedef char page_in_window[(BC250_VRAM_SMU_TABLE_BELOW == 0x14000ull && BC250_VRAM_SMU_TABLE_BELOW < BC250_VRAM_GART_BELOW) ? 1 : -1];
typedef char page_clear[(BC250_VRAM_SMU_TABLE_BELOW <= 0x16000ull && BC250_VRAM_SMU_TABLE_BELOW - 0x1000ull >= 0x10000ull) ? 1 : -1];

static BC250_DEVICE device;

/* Unit A's carve-out: MC 0xF400000000, 8 GB; the page is 0x14000 under its end. */
#define UNIT_A_MC 0xF400000000ull
#define UNIT_A_LEN 0x200000000ull
#define UNIT_A_PHYS 0x80000000ll

static void unit_a(void)
{
    memset(&device, 0, sizeof(device));
    device.FullWddm = TRUE;
    device.VramEnabled = TRUE;
    device.VramPhysical.QuadPart = UNIT_A_PHYS;
    device.VramLength = UNIT_A_LEN;
    device.VramMcBase = UNIT_A_MC;
    device.Smu.Online = TRUE;
    native_expect_mc = UNIT_A_MC + UNIT_A_LEN - BC250_VRAM_SMU_TABLE_BELOW;
    native_expect_physical = UNIT_A_PHYS + (LONGLONG)(UNIT_A_LEN - BC250_VRAM_SMU_TABLE_BELOW);
    native_maps = native_unmaps = native_map_fail = 0;
    native_reads = 0;
    native_result = 0;
    native_lines = 0;
    native_setting_present = 0;
    native_setting_reads = 0;
    native_layout_present = 0;
    native_layout_reads = 0;
    native_dma = BC250_SMU_METRICS_BYTES;   /* the stock firmware, as on unit A */
    SmuMetricsInitialize(&device);
}

static void put16(unsigned off, unsigned v) { native_table[off] = (unsigned char)v; native_table[off + 1] = (unsigned char)(v >> 8); }
static void put32(unsigned off, unsigned v) { put16(off, v & 0xFFFFu); put16(off + 2, v >> 16); }

static void good_table(void)
{
    memset(native_table, 0, sizeof(native_table));
    put16(BC250_SMU_METRICS_OFF_GFXCLK, 1500);
    put16(BC250_SMU_METRICS_OFF_GFX_TEMP, 6200);
    put32(BC250_SMU_METRICS_OFF_VOLTAGE, 900);
    put32(BC250_SMU_METRICS_OFF_VOLTAGE + 4, 919);
    put32(BC250_SMU_METRICS_OFF_POWER, 21000);
    put32(BC250_SMU_METRICS_OFF_POWER + 4, 48000);
    put32(BC250_SMU_METRICS_OFF_SOCKET, 78000);
    put16(BC250_SMU_METRICS_OFF_SOC_TEMP, 6050);
    put16(BC250_SMU_METRICS_OFF_THROTTLER, 4);
    put32(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_SOCKET, 77400);
    put16(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_GFXCLK, 1490);
    put16(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_GFX_TEMP, 6150);
    put32(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_VOLTAGE, 900);
    put32(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_VOLTAGE + 4, 915);
    put16(BC250_SMU_METRICS_HALF + BC250_SMU_METRICS_OFF_SOC_TEMP, 6000);
}

/* Governor ticks of 25 ms for ms milliseconds, each one calling the sampler as DpmTick does. */
static void ticks(ULONG ms)
{
    ULONG t;
    for (t = 0; t < ms; t += 25) {
        native_advance_ms(25);
        SmuMetricsSample(&device);
    }
}

static void gate_off(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    native_setting_present = 1;
    native_setting_value = 0;
    SmuMetricsStart(&device);
    CHECK(native_setting_reads == 1 && native_maps == 0);
    ticks(5000);
    CHECK(native_reads == 0);                               /* EnableSmuMetrics 0: no message at all */
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OFF && x.SocketPowerMw == 0);
    CHECK(native_log_has("EnableSmuMetrics 0"));
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_lines == 0);                               /* an off reader logs nothing every 5 s */
    SmuMetricsStop(&device);
    CHECK(native_unmaps == 0);
}

static void no_page(void)
{
    BC250_DPM_METRICS x;
    /* Display-only start, VRAM closed, no SMU owner, a mapping that fails: no reading, no message. */
    unit_a(); device.FullWddm = FALSE;
    SmuMetricsStart(&device);
    CHECK(native_maps == 0);
    ticks(3000);
    CHECK(native_reads == 0 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_NO_TABLE);
    unit_a(); device.VramEnabled = FALSE;
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(native_maps == 0 && native_reads == 0);
    unit_a(); device.Smu.Online = FALSE;
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(native_maps == 0 && native_reads == 0);
    unit_a(); native_map_fail = 1;
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(native_maps == 1 && native_reads == 0 && native_unmaps == 0);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_NO_TABLE);
    CHECK(native_log_has("no table page"));
    /* A carve-out of 1 MB has no top window at all. */
    unit_a(); device.VramLength = 0x100000;
    SmuMetricsStart(&device);
    CHECK(native_maps == 0);
}

static void reading(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    good_table();
    SmuMetricsStart(&device);                               /* absent value: on (GuardReadSetting(..., 1)) */
    CHECK(native_setting_reads == 1 && native_maps == 1);
    CHECK(native_log_has("table page MC 0xF5FFFEC000"));
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_WAITING);
    CHECK(native_layout_reads == 1);
    /* The first read waits one period after the start. The tail stays poisoned (stock firmware): the first two
     * tables count toward the latch and are not published, the third latches the stock layout and is read. */
    ticks(975);
    CHECK(native_reads == 0);
    ticks(25);
    CHECK(native_reads == 1);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_WAITING && x.MetricsFailures == 0);
    ticks(1000);
    CHECK(native_reads == 2 && !SmuMetricsFill(&device, &x) && !native_log_has("latched"));
    ticks(1000);
    CHECK(native_reads == 3 && native_log_has("stock layout latched after 3 tables in a row"));
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OK);
    CHECK(x.SocketPowerMw == 78000 && x.SocketPowerAvgMw == 77400 && x.GfxPowerMw == 48000 && x.SocPowerMw == 21000);
    CHECK(x.GfxMv == 919 && x.SocMv == 900 && x.GfxMHz == 1500 && x.GfxTemperatureCc == 6200 && x.SocTemperatureCc == 6050);
    CHECK(x.ThrottlerStatus == 4 && x.MetricsReads == 1 && x.MetricsFailures == 0 && x.MetricsAgeMs == 0);
    /* Rate: 40 ticks a second, one read a second. */
    ticks(10000);
    CHECK(native_reads == 13);
    native_advance_ms(640);
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsAgeMs == 640);
    /* The log line fits the ring and says what the escape says. */
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_lines == 2 && native_log_has("mW p78000 a77400 g48000 s21000") && native_log_has("gfx 919 mV 1500 MHz 62.0 C soc 900 mV 60.5 C thr 4"));
    /* A governor that stops calling (paused for a power transition): the copy ages out after three seconds. */
    native_advance_ms(2360);
    CHECK(SmuMetricsFill(&device, &x));
    native_advance_ms(1);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OK && x.SocketPowerMw == 78000);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_log_has("no fresh table"));
    /* Offline owner (between SmuOwnerStop and Start): counted as nothing, retried a period later. */
    device.Smu.Online = FALSE;
    ticks(2000);
    CHECK(native_reads == 15);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsFailures == 0);
    device.Smu.Online = TRUE;
    ticks(1000);
    CHECK(native_reads == 16 && SmuMetricsFill(&device, &x));
    SmuMetricsStop(&device);
    CHECK(native_unmaps == 1 && native_log_has("stop after") && native_log_has("state ok, layout stock"));
    SmuMetricsStop(&device);                                /* idempotent */
    CHECK(native_unmaps == 1);
    /* A sample after the stop touches nothing. */
    ticks(3000);
    CHECK(native_reads == 16);
}

static void bad_tables(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    memset(native_table, 0xFF, sizeof(native_table));      /* the firmware answered OK and wrote nothing */
    SmuMetricsStart(&device);
    ticks(5000);
    CHECK(native_reads == 3);                               /* the third bad table ends this start */
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_BAD_TABLE && x.MetricsFailures == 3);
    CHECK(native_log_has("bad table") && native_log_has("off until next start"));
    CHECK(native_log_has("layout auto, last tail stock, run 0"));   /* never latched */
    /* Not latched for the boot: the next start reads again. */
    good_table();
    SmuMetricsStart(&device);
    CHECK(native_unmaps == 1 && native_maps == 2);          /* the earlier mapping went first */
    ticks(3000);
    CHECK(native_reads == 6 && SmuMetricsFill(&device, &x));
    SmuMetricsStop(&device);
}

static void overflow_and_age(void)
{
    BC250_DPM_METRICS x;
    ULONG i;
    unit_a();
    good_table();
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(SmuMetricsFill(&device, &x));
    put16(68, 100); put16(70, 99); /* C0Residency[6] and [7] overwrote the GPU pair */
    native_table[114] = 0x67;      /* marker does not admit corrupt fields */
    for (i = 1; i <= 3; i++) {
        ticks(1000);
        CHECK(SmuMetricsFill(&device, &x) == (i < 3));
        CHECK(x.MetricsFailures == i && x.MetricsReads == 1 && x.MetricsAgeMs == i * 1000);
        CHECK(x.GfxMHz == 1500 && x.GfxTemperatureCc == 6200 && x.SocketPowerMw == 78000);
    }
    CHECK(x.MetricsState == BC250_DPM_METRICS_BAD_TABLE);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_log_has("reject 3 why 5 sp00000067 m67 1") && native_log_has("no fresh table"));
    ticks(5000);
    CHECK(native_reads == 6 && !SmuMetricsFill(&device, &x));
    SmuMetricsStop(&device);

    /* A transient refusal followed by a valid table recovers, preserving the last refusal reason. */
    good_table();
    SmuMetricsStart(&device);
    ticks(3000);
    put16(68, 100); put16(70, 99);
    ticks(1000);
    good_table();
    ticks(1000);
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsFailures == 1 && x.MetricsAgeMs == 0);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_log_has("reject 1 why 5 sp00000000 m67 0") && native_lines == 2);
    /* Zero-power skips do not renew the last good sample: consumers lose POWER after three periods. */
    put32(220, 0);
    ticks(4000);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OK);
    SmuMetricsStop(&device);
}

/* A board with the patched 8-core SMU firmware: a 284-byte DMA in the eight-core layout. */
static void eight_table(void)
{
    ULONG i;
    memset(native_table, 0, sizeof(native_table));
    for (i = 0; i < 8; i++) put16(i * 2, 3500);                           /* CoreFrequency[8] */
    put16(0x44, 5500); put16(0x46, 5500);                                 /* L3Temperature[2]: offset 68 */
    put16(BC250_SMU_METRICS_8C_OFF_GFXCLK, 1750);
    put16(BC250_SMU_METRICS_8C_OFF_GFX_TEMP, 6400);
    put32(BC250_SMU_METRICS_8C_OFF_VOLTAGE, 1100);
    put32(BC250_SMU_METRICS_8C_OFF_VOLTAGE + 4, 950);
    put32(BC250_SMU_METRICS_8C_OFF_POWER, 33000);
    put32(BC250_SMU_METRICS_8C_OFF_POWER + 4, 61000);
    put32(BC250_SMU_METRICS_8C_OFF_SOCKET, 112000);
    put16(BC250_SMU_METRICS_8C_OFF_SOC_TEMP, 6600);
    put16(BC250_SMU_METRICS_8C_OFF_THROTTLER, 0x10);
    put32(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_SOCKET, 109500);
    put16(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_GFXCLK, 1740);
    put16(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_GFX_TEMP, 6350);
    put32(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_VOLTAGE, 1100);
    put32(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_VOLTAGE + 4, 945);
    put16(BC250_SMU_METRICS_8C_HALF + BC250_SMU_METRICS_8C_OFF_SOC_TEMP, 6550);
    put32(BC250_SMU_METRICS_8C_OFF_ACCNT, 42);
    native_dma = BC250_SMU_METRICS_8C_BYTES;
}

static void eight_core_board(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    eight_table();
    SmuMetricsStart(&device);
    ticks(2000);
    CHECK(native_reads == 2 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_WAITING);
    ticks(1000);
    CHECK(native_reads == 3 && native_log_has("8-core layout latched after 3 tables in a row"));
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OK && x.MetricsFailures == 0);
    CHECK(x.SocketPowerMw == 112000 && x.SocketPowerAvgMw == 109500 && x.GfxPowerMw == 61000 && x.SocPowerMw == 33000);
    CHECK(x.GfxMv == 950 && x.SocMv == 1100 && x.GfxMHz == 1750 && x.GfxTemperatureCc == 6400);
    CHECK(x.SocTemperatureCc == 6600 && x.ThrottlerStatus == 0x10 && x.MetricsReads == 1);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_lines == 2 && native_log_has("mW p112000 a109500 g61000 s33000 reject 0 why 0"));
    CHECK(native_log_has("gfx 950 mV 1750 MHz 64.0 C soc 1100 mV 66.0 C thr 10"));
    /* The DMA shrinks to the stock length after the latch: the tail is poisoned again. Refused with reason 7 and
     * never latched again; three in a row stop the reader for this start. */
    native_dma = BC250_SMU_METRICS_BYTES;
    ticks(3000);
    CHECK(native_reads == 6 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_BAD_TABLE);
    CHECK(x.MetricsFailures == 3 && x.GfxMHz == 1750);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_log_has("reject 3 why 7"));
    SmuMetricsStop(&device);
    CHECK(native_log_has("state bad table, layout 8-core"));
}

static void tail_change(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    good_table();
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsReads == 1);
    /* One tail byte written while the stock layout is latched: refused with reason 7, the last table kept. */
    native_table[BC250_SMU_METRICS_COPY_BYTES - 1u] = 0x00;
    native_dma = BC250_SMU_METRICS_COPY_BYTES;
    ticks(1000);
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsFailures == 1 && x.MetricsReads == 1 && x.MetricsAgeMs == 1000);
    native_dma = BC250_SMU_METRICS_BYTES;
    ticks(1000);
    CHECK(SmuMetricsFill(&device, &x) && x.MetricsReads == 2 && x.MetricsAgeMs == 0);
    native_lines = 0;
    SmuMetricsLogLine(&device, "telemetry");
    CHECK(native_log_has("reject 1 why 7"));
    SmuMetricsStop(&device);
}

static void forced_layout(void)
{
    BC250_DPM_METRICS x;
    /* SmuMetricsLayout 1 on the stock firmware: no latch wait, the first table is read. */
    unit_a();
    good_table();
    native_layout_present = 1; native_layout_value = 1;
    SmuMetricsStart(&device);
    CHECK(native_layout_reads == 1 && native_log_has("SmuMetricsLayout 1: stock layout forced, no tail check"));
    ticks(1000);
    CHECK(native_reads == 1 && SmuMetricsFill(&device, &x) && x.GfxMHz == 1500 && !native_log_has("latched"));
    SmuMetricsStop(&device);
    /* SmuMetricsLayout 2 on the stock firmware: the average lies in the poisoned tail, every table is refused. */
    unit_a();
    good_table();
    native_layout_present = 1; native_layout_value = 2;
    SmuMetricsStart(&device);
    CHECK(native_log_has("SmuMetricsLayout 2: 8-core layout forced, no tail check"));
    ticks(5000);
    CHECK(native_reads == 3 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_BAD_TABLE);
    CHECK(native_log_has("status 0 parse 2") && native_log_has("layout 8-core, last tail stock, run 0"));
    SmuMetricsStop(&device);
    /* SmuMetricsLayout 1 on the eight-core firmware: offset 68 is L3Temperature[0], refused as a clock. */
    unit_a();
    eight_table();
    native_layout_present = 1; native_layout_value = 1;
    SmuMetricsStart(&device);
    ticks(5000);
    CHECK(native_reads == 3 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_BAD_TABLE);
    CHECK(native_log_has("status 0 parse 6"));
    SmuMetricsStop(&device);
    /* Any other value detects, and says so once. */
    unit_a();
    eight_table();
    native_layout_present = 1; native_layout_value = 9;
    SmuMetricsStart(&device);
    CHECK(native_log_has("SmuMetricsLayout 9 is not 0, 1 or 2: the layout is detected"));
    ticks(3000);
    CHECK(SmuMetricsFill(&device, &x) && x.GfxMHz == 1750 && native_log_has("8-core layout latched"));
    SmuMetricsStop(&device);
}

/* Last: the latch is the image's (a static in smu_metrics.c), as on the lab. */
static void refusal(void)
{
    BC250_DPM_METRICS x;
    unit_a();
    good_table();
    SmuMetricsStart(&device);
    ticks(3000);
    CHECK(native_reads == 3 && SmuMetricsFill(&device, &x));
    native_result = 0xFE;                                   /* the firmware refuses the transfer */
    ticks(1000);
    CHECK(native_reads == 4);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_REFUSED && x.MetricsFailures == 1);
    CHECK(native_log_has("refused") && native_log_has("off until next boot"));
    native_result = 0;
    ticks(10000);
    CHECK(native_reads == 4);                               /* never again in this start */
    /* A new start of the adapter in the same boot: nothing is mapped and nothing is sent. */
    SmuMetricsStart(&device);
    CHECK(native_maps == 1 && native_unmaps == 1);
    ticks(10000);
    CHECK(native_reads == 4 && !SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_REFUSED);
    CHECK(native_log_has("refused the table earlier in this boot"));
    /* The switch still wins: EnableSmuMetrics 0 reports off, not refused. */
    native_setting_present = 1; native_setting_value = 0;
    SmuMetricsStart(&device);
    CHECK(!SmuMetricsFill(&device, &x) && x.MetricsState == BC250_DPM_METRICS_OFF);
    /* A new device object in the same driver image (unit_a initializes it again): still latched. */
    unit_a();
    SmuMetricsStart(&device);
    ticks(5000);
    CHECK(native_maps == 0 && native_reads == 0);
}

int main(void)
{
    gate_off();
    no_page();
    reading();
    bad_tables();
    overflow_and_age();
    eight_core_board();
    tail_change();
    forced_layout();
    refusal();
    printf("SMU metrics binding: %ld checks, %ld failures\n", native_checks, native_failures);
    return native_failures ? 1 : 0;
}
