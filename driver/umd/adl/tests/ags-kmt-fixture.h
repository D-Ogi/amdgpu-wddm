// Test-only boundary for the real AGS DLL's dynamically resolved KMT queries.
// No system DLL, registry value or physical adapter is changed. ADL discovery
// uses the separately injected AMD snapshot. This reproduces the private caps
// response of our KMD, which an NVIDIA-host-only test otherwise never exercises.
#pragma once
#include "../../../contract/bc250_caps_query.h"
#include "../../../kmd/umd_caps.c"

namespace ags_kmt_fixture {
struct Adapter { UINT handle; LUID luid; UINT sources; BOOL present; };
struct Adapters { UINT count; Adapter adapters[16]; };
struct Query { UINT handle; UINT type; void* data; UINT bytes; };
static bool legacy;
static unsigned private_queries;
static unsigned foreign_refusals;
static unsigned segment_queries;

static LONG WINAPI Enumerate(Adapters* a)
{
    memset(a, 0, sizeof(*a));
    a->count = 1;
    a->adapters[0].handle = 0x1234;
    a->adapters[0].luid.LowPart = 0x1234;
    a->adapters[0].sources = 1;
    a->adapters[0].present = TRUE;
    return 0;
}

static LONG WINAPI QueryInfo(Query* q)
{
    if (q->type == 3 && q->bytes == 24) {
        ++segment_queries;
        unsigned long long sizes[3] = {8ull << 30, 0, 8ull << 30};
        memcpy(q->data, sizes, sizeof(sizes));
        return 0;
    }
    if (q->type != 0 || !q->data) return (LONG)0xc000000d;
    ++private_queries;
    printf("kmt fixture: private bytes=%u policy=%s\n", q->bytes, legacy ? "legacy" : "bounded");
    fflush(stdout);
    if (!legacy && !Bc250CapsQueryExtentSupported(q->bytes)) {
        ++foreign_refusals;
        return (LONG)0xc000000d;
    }
    if (q->bytes < UMD_CAPS_BYTES) return (LONG)0xc0000023;
    memcpy(q->data, umd_caps_blob, UMD_CAPS_BYTES);
    return 0;
}

static FARPROC WINAPI Resolve(HMODULE module, LPCSTR name)
{
    if ((ULONG_PTR)name > 0xffff) {
        if (!strcmp(name, "D3DKMTEnumAdapters")) return (FARPROC)(void*)&Enumerate;
        if (!strcmp(name, "D3DKMTQueryAdapterInfo")) return (FARPROC)(void*)&QueryInfo;
    }
    return GetProcAddress(module, name);
}

static bool Install(HMODULE ags, bool old_policy)
{
    legacy = old_policy;
    BYTE* base = (BYTE*)ags;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto imports = (IMAGE_IMPORT_DESCRIPTOR*)(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imports->OriginalFirstThunk);
        auto addresses = (IMAGE_THUNK_DATA*)(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto name = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)name->Name, "GetProcAddress")) continue;
            DWORD protection, ignored;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &protection)) return false;
            addresses->u1.Function = (ULONG_PTR)&Resolve;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(ULONG_PTR), protection, &ignored)) return false;
            return true;
        }
    }
    return false;
}
}
