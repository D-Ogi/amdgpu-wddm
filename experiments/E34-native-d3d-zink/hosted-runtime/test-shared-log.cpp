#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <cstring>
#include "shared-log.h"

// Read through an independent handle before fflush/normal process teardown.
// Run with /MD, as the router and Mesa do. No UI or GPU use.
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) return 2;
    const bool legacy = wcscmp(argv[1], L"legacy") == 0;
    const bool buffered = wcscmp(argv[1], L"buffered") == 0;
    const bool closed = wcscmp(argv[1], L"closed") == 0;
    if (!legacy && !buffered && !closed && wcscmp(argv[1], L"default")) return 2;
    if (closed) fclose(stderr);
    if (!RedirectSharedLog(argv[2])) return 3;
    static char buffer[65536];
    if (legacy && setvbuf(stderr, nullptr, _IONBF, 0)) return 4;
    if (buffered && setvbuf(stderr, buffer, _IOFBF, sizeof(buffer))) return 4;
    HANDLE reader = CreateFileW(argv[2], GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return 5;
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    unsigned visible = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        char line[512], expected[514], actual[514];
        int length = sprintf_s(line, "BC250 audit store event=begin seq=%u store=%u map=14 time_ns=123456789 writer=zink_descriptors_update_masked_buffer kind=get_descriptor offset=10160 bytes=16 capacity=24000 mapped_offset=0 valid=1", i, i);
        if (length < 0) return 6;
        int bytes = sprintf_s(expected, "%s\r\n", line);
        if (bytes < 0 || fprintf(stderr, "%s\n", line) < 0) return 7;
        DWORD got = 0;
        if (!ReadFile(reader, actual, static_cast<DWORD>(bytes), &got, nullptr)) return 8;
        if (got != static_cast<DWORD>(bytes) || memcmp(actual, expected, got)) {
            printf("not_visible mode=%ls record=%u got=%lu expected=%d\n", argv[1], i, got, bytes);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 10); // Skip CRT/DLL teardown.
            return 99;
        }
        ++visible;
    }
    QueryPerformanceCounter(&end);
    printf("mode=%ls visible=%u elapsed_us=%.3f\n", argv[1], visible,
        (end.QuadPart-start.QuadPart)*1000000.0/frequency.QuadPart);
    fflush(stdout);
    CloseHandle(reader);
    TerminateProcess(GetCurrentProcess(), 0); // Skip CRT/DLL teardown.
    return 99;
}
