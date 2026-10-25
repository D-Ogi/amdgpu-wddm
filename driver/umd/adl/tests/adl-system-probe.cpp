// Bare-name loading is deliberate: do not mask application-directory or PATH shadowing with load flags.
// This client is copied WITHOUT any ADL DLL into the system-package acceptance kit.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static int Fail(const wchar_t* message, DWORD error = 0)
{
    fwprintf(stderr, L"FAIL adl-system-probe: %ls error=%lu\n", message, error);
    return 1;
}

int wmain(int argc, wchar_t** argv)
{
    const bool absent = argc == 2 && wcscmp(argv[1], L"--expect-absent") == 0;
    if (!absent && !(argc == 2 && wcscmp(argv[1], L"--expect-system") == 0))
        return Fail(L"usage: --expect-system | --expect-absent");
#ifdef _WIN64
    const wchar_t* name = L"atiadlxx.dll";
    const wchar_t* architecture = L"x64";
    const wchar_t* system = L"System32";
#else
    const wchar_t* name = L"atiadlxy.dll";
    const wchar_t* architecture = L"x86";
    BOOL wow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &wow64)) return Fail(L"IsWow64Process", GetLastError());
    const wchar_t* system = wow64 ? L"SysWOW64" : L"System32";
#endif
    wchar_t windows[MAX_PATH] = {}, expected[MAX_PATH] = {}, application[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!n || n >= MAX_PATH) return Fail(L"GetWindowsDirectory", GetLastError());
    if (swprintf_s(expected, L"%ls\\%ls\\%ls", windows, system, name) < 0) return Fail(L"system path too long");
    DWORD count = GetModuleFileNameW(nullptr, application, MAX_PATH);
    if (!count || count >= MAX_PATH) return Fail(L"GetModuleFileName application", GetLastError());
    wchar_t* slash = wcsrchr(application, L'\\');
    if (!slash) return Fail(L"application directory missing");
    slash[1] = 0;
    const wchar_t* localNames[] = {L"atiadlxx.dll", L"atiadlxy.dll"};
    for (const wchar_t* local : localNames) {
        wchar_t path[MAX_PATH] = {};
        if (swprintf_s(path, L"%ls%ls", application, local) < 0) return Fail(L"local path too long");
        DWORD attrs = GetFileAttributesW(path);
        DWORD err = GetLastError();
        if (attrs != INVALID_FILE_ATTRIBUTES) return Fail(L"app-local ADL present");
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) return Fail(L"cannot inspect app-local ADL", err);
    }
    wprintf(L"probe arch=%ls name=%ls expected=%ls\n", architecture, name, expected);
    if (absent) {
        DWORD attrs = GetFileAttributesW(expected);
        DWORD err = GetLastError();
        if (attrs != INVALID_FILE_ATTRIBUTES) return Fail(L"system ADL still present");
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) return Fail(L"cannot inspect system ADL", err);
    }
    // Plain LoadLibrary, rather than an absolute path or a restricted-search flag, tests actual client behavior.
    HMODULE module = LoadLibraryW(name);
    if (!module) {
        DWORD err = GetLastError();
        if (absent && err == ERROR_MOD_NOT_FOUND) {
            wprintf(L"PASS adl-system-probe absent name=%ls error=%lu\n", name, err);
            return 0;
        }
        return Fail(L"LoadLibrary", err);
    }
    wchar_t loaded[MAX_PATH] = {};
    count = GetModuleFileNameW(module, loaded, MAX_PATH);
    if (!count || count >= MAX_PATH) {
        DWORD err = GetLastError(); FreeLibrary(module); return Fail(L"GetModuleFileName loaded", err);
    }
    wprintf(L"resolved=%ls\n", loaded);
    bool valid = !absent && _wcsicmp(loaded, expected) == 0;
    if (!valid) { FreeLibrary(module); return Fail(L"resolved outside expected system path or expected absent"); }
    const char* required[] = {"ADL_Main_Control_Create", "ADL_Main_Control_Destroy",
                             "ADL2_Main_Control_Create", "ADL2_Main_Control_Destroy"};
    for (const char* symbol : required) {
        if (!GetProcAddress(module, symbol)) { FreeLibrary(module); return Fail(L"required ADL export missing"); }
    }
    if (GetProcAddress(module, "Bc250AdlTest_SetSnapshot")) {
        FreeLibrary(module); return Fail(L"test hook in installed ADL");
    }
    FreeLibrary(module);
    wprintf(L"PASS adl-system-probe system arch=%ls\n", architecture);
    return 0;
}
