#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
typedef void (__stdcall *VoidFn)(void);
typedef VoidFn (__stdcall *GetProcFn)(void *, const char *);
static GetProcFn icd_get;
static BOOL CALLBACK initialize(PINIT_ONCE o, PVOID p, PVOID *c) {
    HMODULE self = NULL, icd;
    wchar_t path[32768], *slash;
    DWORD n;
    (void)o; (void)p; (void)c;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&once, &self)) return FALSE;
    n = GetModuleFileNameW(self, path, 32768);
    if (!n || n >= 32768) return FALSE;
    slash = wcsrchr(path, L'\\');
    if (!slash || wcscpy_s(slash + 1, 32768 - (size_t)(slash + 1 - path), L"amdgpu_wddm_radv.dll")) return FALSE;
    icd = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!icd) return FALSE;
    icd_get = (GetProcFn)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
    if (!icd_get) { FreeLibrary(icd); return FALSE; }
    return TRUE;
}
__declspec(dllexport) VoidFn __stdcall vkGetInstanceProcAddr(void *instance, const char *name) {
    if (!InitOnceExecuteOnce(&once, initialize, NULL, NULL)) return NULL;
    return icd_get(instance, name);
}
