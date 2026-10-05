#include <windows.h>
#include <cwchar>
#include <cstring>
#include <share.h>
#include "shared-log.h"
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) return 2;
    const bool old = !wcscmp(argv[1], L"old");
    if (!old && wcscmp(argv[1], L"shared")) return 3;
    if (old) {
        FILE *file = nullptr;
        if (_wfreopen_s(&file, argv[2], L"a", stderr) || !file) return 4;
        if (setvbuf(stderr, nullptr, _IONBF, 0)) return 5;
    } else if (!RedirectSharedLog(argv[2])) return 6;
    fputs("DWM CreateDevice hr=00000000\n", stderr);
    HANDLE reader = CreateFileW(argv[2], GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (old) {
        const DWORD error = GetLastError();
        if (reader != INVALID_HANDLE_VALUE) { CloseHandle(reader); return 7; }
        if (error != ERROR_SHARING_VIOLATION) return 8;
        puts("CRT secure writer negative: shared reader refused as expected");
        return 0;
    }
    if (reader == INVALID_HANDLE_VALUE) return 9;
    char text[128] = {}; DWORD count = 0;
    const BOOL ok = ReadFile(reader, text, sizeof(text)-1, &count, nullptr);
    CloseHandle(reader);
    if (!ok || !strstr(text, "DWM CreateDevice hr=00000000")) return 10;
    FILE *second = _wfsopen(argv[2], L"a", _SH_DENYNO);
    if (!second) return 11;
    fputs("Second logger live\n", second); fclose(second);
    puts("CRT shared writer positive: live reader content and second logger pass");
    return 0;
}
