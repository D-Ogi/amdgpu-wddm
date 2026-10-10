/* Included by the CLI and the host test with adapter/registry calls replaced. */
#ifndef BC250_CLI_WAVES_COMMAND_H
#define BC250_CLI_WAVES_COMMAND_H
#include <errno.h>
#include <stdlib.h>
#include "wave_decode.h"

static int WavesIsAdministrator(void)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID sid = NULL;
    BOOL member = FALSE;
    BOOL checked;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &sid)) return 0;
    checked = CheckTokenMembership(NULL, sid, &member);
    FreeSid(sid);
    return checked && member;
}

static int WaveParseAddress(const WCHAR *text, uint64_t *value)
{
    WCHAR *end;
    const WCHAR *p = text;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
    if (!*p) return 0;
    for (; *p; ++p)
        if (!((*p >= L'0' && *p <= L'9') || (*p >= L'a' && *p <= L'f') || (*p >= L'A' && *p <= L'F')))
            return 0;
    errno = 0;
    *value = _wcstoui64(text, &end, 16);
    return errno != ERANGE && *end == 0;
}

static int WavesCommand(int argc, WCHAR **argv)
{
    int saved = 0, idle = 0, pc = 0, arg = 2, result = 1, requestFailed = 0;
    uint64_t first = 0, end = 0;
    uint32_t matches = 0;
    BC250_ESCAPE_WAVES *request;
    BC250_WAVE_SNAPSHOT *snapshot;
    const char *error = NULL;
    DWORD bytes;
    if (arg < argc && !_wcsicmp(argv[arg], L"saved")) { saved = 1; ++arg; }
    if (arg < argc && !_wcsicmp(argv[arg], L"--expect-idle")) { idle = 1; ++arg; }
    else if (arg + 2 < argc && !_wcsicmp(argv[arg], L"--expect-pc")) {
        pc = 1;
        if (!WaveParseAddress(argv[arg + 1], &first) || !WaveParseAddress(argv[arg + 2], &end) || first >= end)
            goto usage;
        arg += 3;
    }
    if (arg != argc) goto usage;
    if (!WavesIsAdministrator()) {
        fprintf(stderr, "waves requires an elevated administrator token\n");
        return 3;
    }
    request = (BC250_ESCAPE_WAVES *)calloc(1, sizeof(*request));
    if (!request) { fprintf(stderr, "waves: out of memory\n"); return 1; }
    snapshot = &request->Snapshot;
    bytes = sizeof(*snapshot);
    if (saved) {
        DWORD type = 0;
        LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, BC250_PARAMETERS L"\\HangRecovery\\Waves", L"Snapshot",
                                      RRF_RT_REG_BINARY, &type, snapshot, &bytes);
        if (status != ERROR_SUCCESS || type != REG_BINARY) {
            fprintf(stderr, "waves saved: RegGetValue Waves/Snapshot error=%ld type=%lu bytes=%lu\n", status, type, bytes);
            goto out;
        }
    } else {
        D3DKMT_HANDLE adapter = 0;
        D3DKMT_ESCAPE escape = { 0 };
        NTSTATUS status = (NTSTATUS)0xC00000A3L;
        request->Magic = BC250_ESCAPE_MAGIC;
        request->Command = BC250_ESCAPE_GET_WAVES;
        request->AbiVersion = BC250_WAVE_ABI_VERSION;
        request->Op = 0;
        if (OpenAdapterById(BC250_DEFAULT_HWID, &adapter, &status)) goto out;
        escape.hAdapter = adapter;
        escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        /* The KMD serializes register access. These exact flags request no
         * adapter synchronization; measured scheduler behavior remains a lab gate.
         * Never fall back to flags which request adapter synchronization. */
        escape.Flags.HardwareAccess = 1;
        escape.Flags.NoAdapterSynchronization = 1;
        escape.pPrivateDriverData = request;
        escape.PrivateDriverDataSize = sizeof(*request);
        status = D3DKMTEscape(&escape);
        CloseAdapterHandle(adapter);
        PrintStatus("D3DKMTEscape(waves)", status);
        if (!NT_SUCCESS(status)) goto out;
        if (request->Status != BC250_ESCAPE_STATUS_DONE || request->NtStatus != 0) {
            fprintf(stderr, "waves: request status=%u ntstatus=0x%08X\n", request->Status, (unsigned)request->NtStatus);
            requestFailed = 1; /* Retain the partial record returned before a timeout. */
        }
        if (request->Magic != BC250_ESCAPE_MAGIC || request->Command != BC250_ESCAPE_GET_WAVES ||
            request->AbiVersion != BC250_WAVE_ABI_VERSION || request->Op || request->Reserved) {
            fprintf(stderr, "waves: invalid escape response header\n");
            goto out;
        }
        bytes = snapshot->Header.Bytes;
        if (bytes > sizeof(*snapshot)) { fprintf(stderr, "waves: oversized reply\n"); goto out; }
    }
    if (!WaveValidateBlob(snapshot, bytes, &error)) {
        fprintf(stderr, "waves: invalid snapshot: %s\n", error);
        goto out;
    }
    if (snapshot->Header.Kind != (saved ? BC250_WAVE_KIND_PRE_KILL : BC250_WAVE_KIND_LIVE)) {
        fprintf(stderr, "waves: capture kind does not match the requested source\n");
        goto out;
    }
    if (!saved && (snapshot->Header.Result != request->NtStatus || snapshot->Header.DriverVersion != request->Version)) {
        fprintf(stderr, "waves: snapshot and escape response headers disagree\n");
        goto out;
    }
    if (saved) printf("WARNING: historical snapshot; not verified against latest recovery record.\n");
    WavePrint(snapshot);
    result = WaveExpectation(snapshot, idle, pc, first, end, &matches) && !requestFailed ? 0 : 1;
    if (idle) printf("expect-idle: %s\n", result ? "FAIL" : "PASS");
    if (pc) printf("expect-pc: %s stable_matches=%u range=[0x%llX,0x%llX)\n", result ? "FAIL" : "PASS",
                   matches, (unsigned long long)first, (unsigned long long)end);
    if (result) fprintf(stderr, "waves: incomplete/failed capture or expectation did not match\n");
out:
    free(request);
    return result;
usage:
    fprintf(stderr, "usage: bc250kmd_cli waves [saved] [--expect-idle | --expect-pc <hex-start> <hex-end>]\n");
    return 2;
}
#endif
