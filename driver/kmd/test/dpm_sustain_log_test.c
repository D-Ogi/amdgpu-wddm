/* Host test of the sustained raise's lines in the driver log (dpm_sustain_log.h, BD-336). The log summary must show
 * whether the rule is on in every read, also before the rule chose anything, because the start line goes out of the
 * KMD log ring within seconds of the boot. The format strings are dpm.c's own: run_dpm_sustain_log.ps1 reads them
 * from the source into dpm_sustain_formats.h, so this test prints the lines that the driver prints.
 */
#include <stdio.h>
#include <string.h>
#include "dpm_sustain_log.h"
#include "dpm_sustain_formats.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static char line[512];

/* The summary's configuration line of one start, or "" when the block does not print it. */
static const char* ConfigLine(const char* State, unsigned long Permille, unsigned long HoldMs, unsigned long Raises,
                              unsigned long Refusals)
{
    line[0] = '\0';
    if (Bc250DpmSustainLines(1, Raises, Refusals) & BC250_DPM_SUSTAIN_LINE_CONFIG)
        (void)snprintf(line, sizeof(line), DPM_SUSTAIN_CONFIG_FORMAT, Permille, HoldMs, State);
    return line;
}

int main(void)
{
    const char* a;
    const char* b;

    /* ---- the word of a start -------------------------------------------------------------------------------- */
    CHECK(strcmp(Bc250DpmSustainState(1, 1), "on") == 0);
    CHECK(strcmp(Bc250DpmSustainState(0, 1), "off: this start does not govern the clock") == 0);
    CHECK(strcmp(Bc250DpmSustainState(0, 0), "off: this start does not govern the clock") == 0);
    CHECK(strcmp(Bc250DpmSustainState(1, 0), "off: the hold is out of the allowed range") == 0);

    /* ---- the summary prints the configuration line in every case, the counters at 0 included ------------------ */
    CHECK(strcmp(ConfigLine(Bc250DpmSustainState(1, 1), 800, 250, 0, 0),
                 "dpm: summary rules: sustained raise: avg at 800 permille for 250 ms raises -> on") == 0);
    CHECK(strcmp(ConfigLine(Bc250DpmSustainState(0, 0), 800, 0, 0, 0),
                 "dpm: summary rules: sustained raise: avg at 800 permille for 0 ms raises -> off: this start does not "
                 "govern the clock") == 0);
    CHECK(strcmp(ConfigLine(Bc250DpmSustainState(1, 0), 800, 0, 0, 0),
                 "dpm: summary rules: sustained raise: avg at 800 permille for 0 ms raises -> off: the hold is out of "
                 "the allowed range") == 0);
    CHECK(strcmp(ConfigLine(Bc250DpmSustainState(1, 1), 800, 250, 7, 2),
                 "dpm: summary rules: sustained raise: avg at 800 permille for 250 ms raises -> on") == 0);

    /* ---- the counters line: only after a choice, in the summary and in the telemetry block, as before ------- */
    CHECK(!(Bc250DpmSustainLines(1, 0, 0) & BC250_DPM_SUSTAIN_LINE_COUNTERS));
    CHECK(!(Bc250DpmSustainLines(0, 0, 0) & BC250_DPM_SUSTAIN_LINE_COUNTERS));
    CHECK(Bc250DpmSustainLines(1, 3, 0) & BC250_DPM_SUSTAIN_LINE_COUNTERS);
    CHECK(Bc250DpmSustainLines(0, 0, 2) & BC250_DPM_SUSTAIN_LINE_COUNTERS);
    /* The telemetry block never prints the configuration line: the ring would get one more line every block. */
    CHECK(!(Bc250DpmSustainLines(0, 0, 0) & BC250_DPM_SUSTAIN_LINE_CONFIG));
    CHECK(!(Bc250DpmSustainLines(0, 5, 5) & BC250_DPM_SUSTAIN_LINE_CONFIG));
    (void)snprintf(line, sizeof(line), DPM_SUSTAIN_COUNTERS_FORMAT, "summary", 3ul, 1ul, 250ul);
    CHECK(strcmp(line, "dpm: summary sustained raises 3 refused 1, hold 250 ms") == 0);

    /* ---- one pattern finds the start line and the summary line: the same words after "sustained raise:" ------ */
    a = strstr(DPM_SUSTAIN_START_FORMAT, "sustained raise: ");
    b = strstr(DPM_SUSTAIN_CONFIG_FORMAT, "sustained raise: ");
    CHECK(a != NULL && b != NULL);
    CHECK(strcmp(a, b) == 0);

    printf("PASS dpm_sustain_log: the summary line for on and both off reasons with the counters at 0, counters only "
           "after a choice\n");
    return 0;
}
