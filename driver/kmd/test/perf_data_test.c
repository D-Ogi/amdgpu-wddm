// Host test of driver/kmd/perf_data.h (BD-404): the input words that dxgkrnl passes with the four performance data
// queries, the unit conversions of every field, the main-fan choice and the GPU version strings.
//
// The input words are the ones a static reading of dxgkrnl 26100.9974 shows (perf_data.h has the details): a node
// query carries NodeOrdinal | PhysicalAdapterIndex << 16 in 4 bytes, the other three carry PhysicalAdapterIndex.
#include <stdio.h>
#include <string.h>

#include "../asic_ip.h"
#include "../perf_data.h"

static int g_failures, g_checks;
#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static int NameIs(const unsigned short* name, const char* want)
{
    unsigned i;
    for (i = 0; i < BC250_PERF_NAME_CHARS; i++) {
        if (name[i] != (unsigned short)(unsigned char)want[i]) return 0;
        if (want[i] == '\0') return 1;
    }
    return 0;
}

static int NameZeroFrom(const unsigned short* name, unsigned from)
{
    unsigned i;
    for (i = from; i < BC250_PERF_NAME_CHARS; i++)
        if (name[i] != 0) return 0;
    return 1;
}

// A board with every reading fresh, as the BC-250 path gives it at the default tier.
static BC250_PERF_SOURCES Bc250Sources(void)
{
    BC250_PERF_SOURCES s;
    memset(&s, 0, sizeof(s));
    s.GfxClockValid = 1;
    s.GfxMHz = 1800u;
    s.GfxVoltageValid = 1;
    s.GfxMv = 950u;
    s.TemperatureValid = 1;
    s.TemperatureMc = 83125;
    s.FanValid = 1;
    s.FanRpm = 1589u;
    s.PoweredOn = 1;
    s.EnvelopeValid = 1;
    s.GfxMaxMHz = 2000u;
    s.GfxMaxMv = 1000u;
    s.GfxMaxOcMHz = 2200u;
    s.GfxMaxOcMv = 1050u;
    s.TemperatureWarningMc = 87000;
    s.TemperatureMaxMc = 90000;
    return s;
}

static void TestNodeInput(void)
{
    unsigned word, ordinal = 77u;

    CHECK(Bc250PerfNodeInput(NULL, 4u, 3u, &ordinal) == BC250_PERF_INPUT_MISSING);
    word = 0u;
    CHECK(Bc250PerfNodeInput(&word, 3u, 3u, &ordinal) == BC250_PERF_INPUT_MISSING);
    CHECK(Bc250PerfNodeInput(&word, 0u, 3u, &ordinal) == BC250_PERF_INPUT_MISSING);
    CHECK(ordinal == 77u);      // a refusal leaves the output alone
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_OK && ordinal == 0u);
    word = 2u;
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_OK && ordinal == 2u);
    word = 3u;                  // one past the last node
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_NODE);
    word = 1u;                  // a start with the gates closed has one node
    CHECK(Bc250PerfNodeInput(&word, 4u, 1u, &ordinal) == BC250_PERF_INPUT_NODE);
    word = 0xFFFFu;
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_NODE);
    word = 1u << 16;            // node 0 of physical adapter 1: there is one physical adapter
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_ADAPTER);
    word = 0xFFFF0000u;
    CHECK(Bc250PerfNodeInput(&word, 4u, 3u, &ordinal) == BC250_PERF_INPUT_ADAPTER);
    word = 0u;                  // a larger input buffer still starts with the word
    CHECK(Bc250PerfNodeInput(&word, 8u, 3u, &ordinal) == BC250_PERF_INPUT_OK && ordinal == 0u);
}

static void TestAdapterInput(void)
{
    unsigned word = 0u;

    CHECK(Bc250PerfAdapterInput(NULL, 4u) == BC250_PERF_INPUT_MISSING);
    CHECK(Bc250PerfAdapterInput(&word, 3u) == BC250_PERF_INPUT_MISSING);
    CHECK(Bc250PerfAdapterInput(&word, 4u) == BC250_PERF_INPUT_OK);
    word = 1u;
    CHECK(Bc250PerfAdapterInput(&word, 4u) == BC250_PERF_INPUT_ADAPTER);
    word = 0x10000u;
    CHECK(Bc250PerfAdapterInput(&word, 4u) == BC250_PERF_INPUT_ADAPTER);
}

static void TestDeciCelsius(void)
{
    CHECK(Bc250PerfDeciCelsius(87000) == 870u);
    CHECK(Bc250PerfDeciCelsius(86999) == 869u);     // truncated, never rounded up past a limit
    CHECK(Bc250PerfDeciCelsius(83125) == 831u);
    CHECK(Bc250PerfDeciCelsius(100) == 1u);
    CHECK(Bc250PerfDeciCelsius(99) == 0u);
    CHECK(Bc250PerfDeciCelsius(0) == 0u);
    CHECK(Bc250PerfDeciCelsius(-5000) == 0u);       // the field is unsigned: below 0 C reads as 0
    CHECK(Bc250PerfDeciCelsius(150000) == 1500u);
}

static void TestNode(void)
{
    BC250_PERF_SOURCES s = Bc250Sources();
    BC250_PERF_NODE n;

    memset(&n, 0xA5, sizeof(n));
    Bc250PerfFillNode(&s, 1, &n);
    CHECK(n.Frequency == 1800000000ull);
    CHECK(n.MaxFrequency == 2000000000ull);
    CHECK(n.MaxFrequencyOC == 2200000000ull);
    CHECK(n.Voltage == 950u);
    CHECK(n.VoltageMax == 1000u);
    CHECK(n.VoltageMaxOC == 1050u);
    CHECK(n.MaxTransitionLatency == 0ull);

    // A node outside the GFX clock domain (the copy engine): no source for any field.
    memset(&n, 0xA5, sizeof(n));
    Bc250PerfFillNode(&s, 0, &n);
    CHECK(n.Frequency == 0ull && n.MaxFrequency == 0ull && n.MaxFrequencyOC == 0ull);
    CHECK(n.Voltage == 0u && n.VoltageMax == 0u && n.VoltageMaxOC == 0u && n.MaxTransitionLatency == 0ull);

    // Stale readings: the limits stay, the readings go.
    s.GfxClockValid = 0;
    s.GfxVoltageValid = 0;
    Bc250PerfFillNode(&s, 1, &n);
    CHECK(n.Frequency == 0ull && n.Voltage == 0u);
    CHECK(n.MaxFrequency == 2000000000ull && n.VoltageMax == 1000u);

    // A part without a board envelope (another AMD GPU): readings only, no limits.
    s = Bc250Sources();
    s.EnvelopeValid = 0;
    Bc250PerfFillNode(&s, 1, &n);
    CHECK(n.Frequency == 1800000000ull && n.Voltage == 950u);
    CHECK(n.MaxFrequency == 0ull && n.MaxFrequencyOC == 0ull && n.VoltageMax == 0u && n.VoltageMaxOC == 0u);

    // The largest clock the envelope can name does not overflow the hertz value.
    s = Bc250Sources();
    s.GfxMHz = 0xFFFFFFFFu;
    Bc250PerfFillNode(&s, 1, &n);
    CHECK(n.Frequency == 0xFFFFFFFFull * 1000000ull);
}

static void TestAdapter(void)
{
    BC250_PERF_SOURCES s = Bc250Sources();
    BC250_PERF_ADAPTER a;

    memset(&a, 0xA5, sizeof(a));
    Bc250PerfFillAdapter(&s, &a);
    CHECK(a.Temperature == 831u);
    CHECK(a.FanRPM == 1589u);
    CHECK(a.PowerStateOverride == 1u);
    // No honest source in this driver: zero, the value dxgkrnl itself holds when a driver gives no answer.
    CHECK(a.MemoryFrequency == 0ull && a.MaxMemoryFrequency == 0ull && a.MaxMemoryFrequencyOC == 0ull);
    CHECK(a.MemoryBandwidth == 0ull && a.PCIEBandwidth == 0ull);
    CHECK(a.Power == 0u);

    s.TemperatureValid = 0;
    s.FanValid = 0;
    s.PoweredOn = 0;
    memset(&a, 0xA5, sizeof(a));
    Bc250PerfFillAdapter(&s, &a);
    CHECK(a.Temperature == 0u && a.FanRPM == 0u && a.PowerStateOverride == 0u);
    CHECK(a.MemoryFrequency == 0ull && a.Power == 0u);
}

static void TestCaps(void)
{
    BC250_PERF_SOURCES s = Bc250Sources();
    BC250_PERF_CAPS c;

    memset(&c, 0xA5, sizeof(c));
    Bc250PerfFillCaps(&s, &c);
    CHECK(c.TemperatureWarning == 870u);
    CHECK(c.TemperatureMax == 900u);
    CHECK(c.MaxFanRPM == 0u);
    CHECK(c.MaxMemoryBandwidth == 0ull && c.MaxPCIEBandwidth == 0ull);

    s.EnvelopeValid = 0;
    memset(&c, 0xA5, sizeof(c));
    Bc250PerfFillCaps(&s, &c);
    CHECK(c.TemperatureWarning == 0u && c.TemperatureMax == 0u && c.MaxFanRPM == 0u);
    CHECK(c.MaxMemoryBandwidth == 0ull && c.MaxPCIEBandwidth == 0ull);
}

static void TestMainFan(void)
{
    const unsigned rpm[5] = { 0u, 1589u, 0u, 2400u, 0u };
    unsigned out = 77u;

    // E01: five tachometers, one with a fan on it.
    CHECK(Bc250PerfMainFan(rpm, 5u, 0x1Fu, 0x03u, &out) == 1 && out == 1589u);
    // The fastest present channel that the sample accepted.
    CHECK(Bc250PerfMainFan(rpm, 5u, 0x1Fu, 0x1Fu, &out) == 1 && out == 2400u);
    // A refused channel does not count, however fast it read.
    CHECK(Bc250PerfMainFan(rpm, 5u, 0x17u, 0x1Fu, &out) == 1 && out == 1589u);
    // Present channels that answered and read 0: a reading of 0 RPM.
    CHECK(Bc250PerfMainFan(rpm, 5u, 0x05u, 0x05u, &out) == 1 && out == 0u);
    // Nothing present, or nothing accepted: no reading, the output untouched.
    out = 77u;
    CHECK(Bc250PerfMainFan(rpm, 5u, 0x1Fu, 0u, &out) == 0 && out == 77u);
    CHECK(Bc250PerfMainFan(rpm, 5u, 0u, 0x1Fu, &out) == 0 && out == 77u);
    // Mask bits past the count are ignored.
    CHECK(Bc250PerfMainFan(rpm, 2u, 0xFFFFFFFFu, 0xFFFFFFFFu, &out) == 1 && out == 1589u);
}

static void TestNames(void)
{
    unsigned short name[BC250_PERF_NAME_CHARS];
    unsigned short src[40];
    unsigned i;

    memset(name, 0xA5, sizeof(name));
    Bc250PerfArchitecture(BC250_IP_VERSION(10, 1, 3), name);
    CHECK(NameIs(name, "GC 10.1.3"));
    CHECK(NameZeroFrom(name, 9u));

    memset(name, 0xA5, sizeof(name));
    Bc250PerfArchitecture(BC250_IP_VERSION(255, 255, 255), name);
    CHECK(NameIs(name, "GC 255.255.255"));

    memset(name, 0xA5, sizeof(name));
    Bc250PerfArchitecture(0u, name);        // no row in the IP table: no name
    CHECK(NameZeroFrom(name, 0u));

    for (i = 0; i < 39u; i++) src[i] = (unsigned short)('A' + i % 26u);
    src[39] = 0;
    memset(name, 0xA5, sizeof(name));
    Bc250PerfCopyName(name, src);
    CHECK(name[BC250_PERF_NAME_CHARS - 1u] == 0);    // truncated, always terminated
    CHECK(name[0] == 'A' && name[30] == (unsigned short)('A' + 30u % 26u));

    src[0] = '1'; src[1] = '1'; src[2] = '3'; src[3] = 0;
    memset(name, 0xA5, sizeof(name));
    Bc250PerfCopyName(name, src);
    CHECK(NameIs(name, "113"));
    CHECK(NameZeroFrom(name, 3u));

    memset(name, 0xA5, sizeof(name));
    Bc250PerfCopyName(name, NULL);
    CHECK(NameZeroFrom(name, 0u));
}

int main(void)
{
    TestNodeInput();
    TestAdapterInput();
    TestDeciCelsius();
    TestNode();
    TestAdapter();
    TestCaps();
    TestMainFan();
    TestNames();
    printf("perf_data_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
