#pragma once
#include <windows.h>
#include <cwchar>
namespace m14_probe {
inline constexpr wchar_t client[]=LR"(C:\BC250\m14\runtime001\d3d11bench.exe)";
inline bool select(const wchar_t *path,const wchar_t *flag,ULONGLONG now,ULONGLONG enabled) {
    return path && flag && !_wcsicmp(path,client) && !wcscmp(flag,L"1") &&
        now>=enabled && now-enabled<600000000ULL;
}
}
