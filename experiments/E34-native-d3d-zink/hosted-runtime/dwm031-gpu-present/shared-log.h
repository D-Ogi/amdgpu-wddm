#pragma once
#include <cstdio>
#include <io.h>
#include <share.h>

// Allow readiness readers and Mesa logging while retaining stderr for diagnostics.
static bool RedirectSharedLog(const wchar_t *path)
{
    FILE *file = _wfsopen(path, L"a", _SH_DENYNO);
    if (!file)
        return false;
    const int result = _dup2(_fileno(file), _fileno(stderr));
    fclose(file);
    if (result != 0)
        return false;
    return setvbuf(stderr, nullptr, _IONBF, 0) == 0;
}
