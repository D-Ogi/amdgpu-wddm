#include <windows.h>
#include <cstdio>
#include <io.h>
#include <cstdlib>
#ifdef TEST_OLD_REDIRECT
#include "shared-log-old.h"
#else
#include "shared-log.h"
#endif
static unsigned invalidCalls;
static void Invalid(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t)
{
    ++invalidCalls;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Observe the rejected CRT call without invoking the default fail-fast handler.
    _set_invalid_parameter_handler(Invalid);
    const int before = _fileno(stderr);
    const bool redirected = RedirectSharedLog(argv[1]);
    const int after = _fileno(stderr);
    bool content = false;
    if (redirected) {
        fputs("DWM CreateDevice hr=00000000\n", stderr);
        HANDLE reader = CreateFileW(argv[1], GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reader != INVALID_HANDLE_VALUE) {
            char data[128] = {}; DWORD bytes = 0;
            content = ReadFile(reader, data, sizeof(data)-1, &bytes, nullptr) &&
                strstr(data, "DWM CreateDevice hr=00000000");
            CloseHandle(reader);
        }
    }
    FILE *receipt = nullptr;
    if (_wfopen_s(&receipt, argv[2], L"w") || !receipt) return 3;
    fprintf(receipt, "before=%d after=%d redirected=%u content=%u invalid_calls=%u\n",
            before, after, redirected ? 1u : 0u, content ? 1u : 0u, invalidCalls);
    fclose(receipt);
#ifdef TEST_OLD_REDIRECT
    return before == -2 && !redirected ? 0 : 4;
#else
    return before == -2 && redirected && content && invalidCalls == 0 ? 0 : 5;
#endif
}
