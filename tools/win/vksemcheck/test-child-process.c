/* CPU-only test of the production child launcher. No Vulkan loader or device.
 * The optional negative control uses the original STARTUPINFO call verbatim.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "child-process.h"

static unsigned failures;
#define CHECK(test) do { if (!(test)) { ++failures; printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)

static BOOL Launch(const wchar_t *self, wchar_t *line, PROCESS_INFORMATION *pi)
{
#ifdef TEST_LEGACY_STARTUP
    STARTUPINFOW si = { sizeof si };
    memset(pi, 0, sizeof *pi);
    return CreateProcessW(self, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, pi);
#else
    return StartChildWithStandardHandles(self, line, pi);
#endif
}

static void Capture(const wchar_t *self, BOOL missing)
{
    HANDLE saved[3] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    HANDLE outRead = NULL, outWrite = NULL, errRead = NULL, errWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE event = CreateEventW(&sa, TRUE, FALSE, NULL);
    CHECK(event != NULL);
    if (!event) return;
    CHECK(CreatePipe(&outRead, &outWrite, NULL, 0));
    CHECK(CreatePipe(&errRead, &errWrite, NULL, 0));
    if (!outRead || !errRead) goto done;
    wchar_t line[2048];
    swprintf(line, 2048, L"\"%ls\" --child %llu", self, (unsigned long long)(uintptr_t)event);
    PROCESS_INFORMATION pi;
    DWORD flagsBefore = 0, flagsAfter = 0;
    CHECK(GetHandleInformation(errWrite, &flagsBefore));
    CHECK(!(flagsBefore & HANDLE_FLAG_INHERIT));
    CHECK(SetStdHandle(STD_INPUT_HANDLE, NULL));
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, missing ? INVALID_HANDLE_VALUE : outWrite));
    CHECK(SetStdHandle(STD_ERROR_HANDLE, missing ? NULL : errWrite));
    BOOL started = Launch(self, line, &pi);
    CHECK(SetStdHandle(STD_INPUT_HANDLE, saved[0]));
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, saved[1]));
    CHECK(SetStdHandle(STD_ERROR_HANDLE, saved[2]));
    CHECK(started);
    CHECK(GetHandleInformation(errWrite, &flagsAfter) && flagsBefore == flagsAfter);
    CloseHandle(outWrite); outWrite = NULL;
    CloseHandle(errWrite); errWrite = NULL;
    if (started) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 5000), exitCode = 259;
        CHECK(wait == WAIT_OBJECT_0);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 4);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CHECK(GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0);
        CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    char outText[128] = {0}, errText[128] = {0};
    DWORD outCount = 0, errCount = 0;
    ReadFile(outRead, outText, sizeof outText - 1, &outCount, NULL);
    ReadFile(errRead, errText, sizeof errText - 1, &errCount, NULL);
    if (missing) { CHECK(outCount == 0 && errCount == 0); }
    else {
        CHECK(strcmp(outText, "child stdout\n") == 0);
        CHECK(strstr(errText, "child stderr") != NULL);
        CHECK(strstr(errText, "child native stderr\n") != NULL);
    }
done:
    if (outRead) CloseHandle(outRead);
    if (outWrite) CloseHandle(outWrite);
    if (errRead) CloseHandle(errRead);
    if (errWrite) CloseHandle(errWrite);
    CloseHandle(event);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && !wcscmp(argv[1], L"--child")) {
        HANDLE event = (HANDLE)(uintptr_t)_wcstoui64(argv[2], NULL, 10);
        DWORD written;
        BOOL ok = GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
        ok = ok && WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "child stdout\n", 13, &written, NULL);
        ok = ok && fprintf(stderr, "child stderr\n") > 0 && fflush(stderr) == 0;
        ok = ok && WriteFile(GetStdHandle(STD_ERROR_HANDLE), "child native stderr\n", 20, &written, NULL);
        ok = ok && SetEvent(event);
        CloseHandle(event);
        return ok ? 0 : 1;
    }
    wchar_t self[MAX_PATH];
    CHECK(GetModuleFileNameW(NULL, self, MAX_PATH) != 0);
    Capture(self, FALSE);
    Capture(self, TRUE);
    /* Warm the failed CreateProcess path before checking repeated cleanup. */
    PROCESS_INFORMATION warmProcess;
    wchar_t warmLine[] = L"missing";
    CHECK(!StartChildWithStandardHandles(L"?:\\missing.exe", warmLine, &warmProcess));
    DWORD handlesBefore = 0, handlesAfter = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));
    for (unsigned i = 0; i < 8; ++i) {
        PROCESS_INFORMATION pi;
        wchar_t line[] = L"missing";
        CHECK(!StartChildWithStandardHandles(L"?:\\missing.exe", line, &pi));
        CHECK(GetLastError() != ERROR_SUCCESS);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
    printf("failed launches: handles before %lu, after %lu\n", handlesBefore, handlesAfter);
    CHECK(handlesAfter == handlesBefore);
    printf("child standard handles: %u failures; CPU only\n", failures);
    return failures ? 1 : 0;
}
