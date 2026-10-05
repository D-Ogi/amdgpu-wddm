#pragma once
#include <cstdio>

// Reinitialize stderr even in DWM, which has no console or initial descriptor.
// Non-secure freopen intentionally uses shared access; the path is trial-owned.
static bool RedirectSharedLog(const wchar_t *path)
{
#pragma warning(suppress: 4996)
    FILE *file = _wfreopen(path, L"a", stderr);
    if (!file)
        return false;
    return setvbuf(stderr, nullptr, _IONBF, 0) == 0;
}
