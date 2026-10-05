/* Stand-in for deqp-vk.exe to exercise run-batch.ps1's qpa handling without a GPU.
 * Reads --deqp-caselist-file and --deqp-log-filename; behaviour from FAKE_MODE:
 *   normal    every listed case Pass, except names containing "missing" (never begun), #endSession, exit 0
 *   crash     case 1 Pass, case 2 begun then an access violation (no crash handler output)
 *   watchdog  case 1 Pass, case 2 #terminateTestCaseResult Timeout, exit 0 (as tcu::App on watchdog)
 *   hang      case 1 begun, then sleeps forever (the runner's bound must kill it)
 *   devlost   case 1 Pass, case 2 Fail "VK_ERROR_DEVICE_LOST", exit 1 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pass(FILE *q, const char *c, const char *code, const char *text)
{
    fprintf(q, "#beginTestCaseResult %s\n<TestCaseResult CasePath=\"%s\">\n", c, c);
    fprintf(q, " <Number Name=\"TestDuration\" Description=\"x\" Tag=\"Time\" Unit=\"us\">1234</Number>\n");
    fprintf(q, " <Result StatusCode=\"%s\">%s</Result>\n</TestCaseResult>\n\n#endTestCaseResult\n\n", code, text);
    fflush(q);
}

int main(int argc, char **argv)
{
    const char *list = NULL, *log = NULL, *mode = getenv("FAKE_MODE");
    char line[1024];
    int i, n = 0;
    FILE *l, *q;
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--deqp-caselist-file=", 21)) list = argv[i] + 21;
        if (!strncmp(argv[i], "--deqp-log-filename=", 20)) log = argv[i] + 20;
    }
    if (!mode) mode = "normal";
    if (!list || !log || !(l = fopen(list, "r")) || !(q = fopen(log, "w"))) return 2;
    fprintf(q, "#sessionInfo deviceName FAKE BC-250 device\n\n#beginSession\n\n");
    fflush(q);
    while (fgets(line, sizeof line, l)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        n++;
        if (!strcmp(mode, "normal")) {
            if (!strstr(line, "missing")) pass(q, line, "Pass", "Pass");
        } else if (n == 1 && strcmp(mode, "hang")) {
            pass(q, line, "Pass", "Pass");
        } else if (!strcmp(mode, "crash")) {
            fprintf(q, "#beginTestCaseResult %s\n<TestCaseResult>\n", line);
            fflush(q);
            *(volatile int *)0 = 1;
        } else if (!strcmp(mode, "watchdog")) {
            fprintf(q, "#beginTestCaseResult %s\n<TestCaseResult>\n\n#terminateTestCaseResult Timeout\n", line);
            fflush(q);
            return 0;
        } else if (!strcmp(mode, "hang")) {
            fprintf(q, "#beginTestCaseResult %s\n<TestCaseResult>\n", line);
            fflush(q);
            for (;;) Sleep(1000);
        } else if (!strcmp(mode, "devlost")) {
            pass(q, line, "Fail", "VK_ERROR_DEVICE_LOST at x.cpp:1");
            return 1;
        }
    }
    fprintf(q, "#endSession\n");
    fclose(q);
    return 0;
}
