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
    // Keep UCRT's default stderr mode. Explicit _IONBF disables its temporary
    // per-call buffer and makes redirected fprintf write character by character.
    // Do not substitute full buffering: checkpoint readers need each completed
    // record visible before process teardown. test-shared-log.cpp checks this
    // through an independent file handle, including an abrupt TerminateProcess.
    return true;
}
