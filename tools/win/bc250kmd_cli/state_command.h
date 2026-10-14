/* Bounded JSON protocol for scripts. Only catalog identities cross the ABI. */
#pragma once
#include <errno.h>
#include <stdlib.h>
#include "../../../driver/state/client.h"

static const char *StateStatusName(unsigned status)
{
    static const char *const names[] = { "ok", "absent", "unknown", "type", "range", "access",
        "io", "schema", "incomplete", "buffer", "invalid-argument" };
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "invalid-status";
}
static const char *StateSourceName(unsigned source)
{
    static const char *const names[] = { "none", "override", "factory", "code" };
    return source < sizeof(names) / sizeof(names[0]) ? names[source] : "invalid-source";
}
static void StateJsonAscii(const char *s)
{
    printf("\"");
    for (; *s; ++s) {
        unsigned c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32 || c >= 127) printf("\\u%04x", c);
        else printf("%c", c);
    }
    printf("\"");
}
static void StateJsonWide(const WCHAR *s)
{
    printf("\"");
    for (; *s; ++s) {
        unsigned c = *s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32 || c >= 127) printf("\\u%04x", c);
        else printf("%c", c);
    }
    printf("\"");
}
static int StateAscii(const WCHAR *wide, char *out, size_t capacity)
{
    size_t i;
    for (i = 0; wide[i]; ++i) {
        if (i + 1 >= capacity || wide[i] < 32 || wide[i] > 126) return 0;
        out[i] = (char)wide[i];
    }
    out[i] = 0;
    return 1;
}
static int StateFindSpec(const char *path, const char *name, AMDGPU_CLIENT_SPEC *spec)
{
    DWORD i;
    for (i = 0; i < AmdgpuClientCount(); ++i) {
        if (AmdgpuClientSpec(i, spec, sizeof(*spec))) return 0;
        if (!_stricmp(path, spec->path) && !_stricmp(name, spec->name)) return 1;
    }
    return 0;
}
static void StateJsonSpec(const AMDGPU_CLIENT_SPEC *spec)
{
    printf("\"path\":"); StateJsonAscii(spec->path);
    printf(",\"name\":"); StateJsonAscii(spec->name);
    printf(",\"kind\":\"%s\",\"type\":%u,\"max_bytes\":%u,\"access\":%u,\"private_read\":%u,\"group\":%u",
        spec->kind == AMDGPU_STATE_SETTING ? "setting" : spec->kind == AMDGPU_STATE_RECORD ? "record" : "volatile",
        spec->type, spec->max_bytes, spec->access, spec->private_read, spec->group);
}
static DWORD StateJsonRead(AMDGPU_WINDOWS_STATE *state, const AMDGPU_CLIENT_SPEC *spec, DWORD effective,
                           unsigned *status)
{
    AMDGPU_STATE_VALUE v = { 0 };
    DWORD error;
    unsigned char *data = (unsigned char *)calloc(1, spec->max_bytes ? spec->max_bytes : 1);
    if (!data) {
        v.status = AMDGPU_STATE_IO; v.os_error = ERROR_NOT_ENOUGH_MEMORY;
        error = ERROR_NOT_ENOUGH_MEMORY;
    } else error = AmdgpuClientRead(state, spec->path, spec->name, effective, &v, data, spec->max_bytes);
    *status = v.status;
    printf("{\"status\":\"%s\",\"os_error\":%u,\"error\":%lu,\"source\":\"%s\",\"type\":%u,\"bytes\":%u,\"value\":",
        StateStatusName(v.status), v.os_error, error, StateSourceName(v.source), v.type, v.bytes);
    if (error || v.status != AMDGPU_STATE_OK || !data) printf("null");
    else if (v.type == AMDGPU_STATE_DWORD) { DWORD n; memcpy(&n, data, sizeof(n)); printf("%lu", n); }
    else if (v.type == AMDGPU_STATE_QWORD) { ULONGLONG n; memcpy(&n, data, sizeof(n)); printf("\"%llu\"", n); }
    else if (v.type == AMDGPU_STATE_SZ) StateJsonWide((WCHAR *)data);
    else if (v.type == AMDGPU_STATE_MULTI_SZ) {
        const WCHAR *p = (WCHAR *)data; int comma = 0;
        printf("[");
        for (; *p; p += wcslen(p) + 1) { if (comma++) printf(","); StateJsonWide(p); }
        printf("]");
    } else {
        unsigned i; printf("\"");
        for (i = 0; i < v.bytes; ++i) printf("%02x", data[i]);
        printf("\"");
    }
    printf("}");
    free(data);
    return error;
}
static int StateParseDword(const WCHAR *text, DWORD *value)
{
    WCHAR *end; unsigned __int64 number; const WCHAR *p = text; int base = 10;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) { p += 2; base = 16; }
    if (!*p) return 0;
    for (; *p; ++p)
        if (!(*p >= L'0' && *p <= L'9') && !(base == 16 &&
            ((*p >= L'a' && *p <= L'f') || (*p >= L'A' && *p <= L'F')))) return 0;
    errno = 0; number = _wcstoui64(text, &end, base);
    if (errno == ERANGE || *end || number > MAXDWORD) return 0;
    *value = (DWORD)number; return 1;
}
static int StateCommand(int argc, WCHAR **argv)
{
    AMDGPU_WINDOWS_STATE *state = NULL;
    AMDGPU_CLIENT_SPEC spec;
    char path[128] = "", name[128] = "";
    const char *command = "invalid";
    DWORD error = ERROR_INVALID_PARAMETER, value = 0, i;
    unsigned status = AMDGPU_STATE_INVALID_ARGUMENT;
    int catalog = 0, snapshot = 0, get = 0, set = 0, clear = 0, effective = 1, end = argc;
    if (argc < 3) goto invalid;
    if (!_wcsicmp(argv[2], L"paths")) {
        WCHAR logs[32768] = { 0 };
        if (argc != 3) { command = "paths"; goto invalid; }
        error = AmdgpuClientLogDirectory(logs, (DWORD)(sizeof(logs) / sizeof(logs[0])));
        if (error) logs[0] = 0;
        printf("{\"abi\":1,\"command\":\"paths\",\"error\":%lu,\"logs\":", error);
        StateJsonWide(logs); printf("}\n");
        return error ? 1 : 0;
    }
    catalog = !_wcsicmp(argv[2], L"catalog"); snapshot = !_wcsicmp(argv[2], L"snapshot");
    get = !_wcsicmp(argv[2], L"get"); set = !_wcsicmp(argv[2], L"set"); clear = !_wcsicmp(argv[2], L"clear");
    command = catalog ? "catalog" : snapshot ? "snapshot" : get ? "get" : set ? "set" : clear ? "clear" : "invalid";
    if (catalog || snapshot) { if (argc != 3) goto invalid; }
    else {
        int pathIndex = set ? 5 : 4;
        if (!get && !set && !clear) goto invalid;
        if (get && argc > 4 && !_wcsicmp(argv[argc - 1], L"--override")) { effective = 0; --end; }
        if (end < pathIndex || end > pathIndex + 1) goto invalid;
        if (!StateAscii(argv[3], name, sizeof(name))) goto invalid;
        if (end > pathIndex && !StateAscii(argv[pathIndex], path, sizeof(path))) goto invalid;
        if (!StateFindSpec(path, name, &spec)) goto invalid;
        if (set && (spec.type != AMDGPU_STATE_DWORD || !StateParseDword(argv[4], &value))) goto invalid;
    }
    if (!catalog) {
        error = AmdgpuClientOpen(&state, set || clear);
        if (error) goto invalid;
    }
    if (catalog || snapshot) {
        printf("{\"abi\":1,\"command\":\"%s\",\"error\":0,\"schema\":%u,\"catalog_id\":", command, AmdgpuStateSchema());
        StateJsonAscii(AmdgpuStateCatalogId()); printf(",\"items\":[");
        for (i = 0; i < AmdgpuClientCount(); ++i) {
            error = AmdgpuClientSpec(i, &spec, sizeof(spec));
            if (error) break;
            if (i) printf(",");
            printf("{"); StateJsonSpec(&spec);
            if (snapshot) {
                if (spec.kind == AMDGPU_STATE_SETTING) {
                    printf(",\"override\":"); (void)StateJsonRead(state, &spec, 0, &status);
                    printf(",\"effective\":"); (void)StateJsonRead(state, &spec, 1, &status);
                } else { printf(",\"record\":"); (void)StateJsonRead(state, &spec, 0, &status); }
            }
            printf("}");
        }
        printf("]}\n"); AmdgpuClientClose(state); return error ? 1 : 0;
    }
    if (get) {
        printf("{\"abi\":1,\"command\":\"get\","); StateJsonSpec(&spec);
        printf(",\"result\":"); error = StateJsonRead(state, &spec, effective, &status);
        printf(",\"error\":%lu}\n", error); AmdgpuClientClose(state);
        return status == AMDGPU_STATE_ABSENT ? 3 : error ? 1 : 0;
    }
    error = set ? AmdgpuClientWrite(state, spec.path, spec.name, AMDGPU_STATE_DWORD, &value, sizeof(value))
                : AmdgpuClientClear(state, spec.path, spec.name);
    AmdgpuClientClose(state);
    printf("{\"abi\":1,\"command\":\"%s\",\"error\":%lu,", command, error); StateJsonSpec(&spec);
    printf("}\n"); return error ? 1 : 0;
invalid:
    printf("{\"abi\":1,\"command\":\"%s\",\"error\":%lu,\"service_installed\":%s}\n",
           command, error, error == ERROR_SERVICE_DOES_NOT_EXIST ? "false" : "null");
    return error == ERROR_INVALID_PARAMETER ? 2 : 1;
}
