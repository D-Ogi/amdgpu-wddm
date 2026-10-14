/* Host-only tests. Every adapter, token and registry call is a fake. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include "../../../driver/kmd/bc250kmd_escape.h"
#include "../../../driver/kmd/wave_snapshot.h"
#include "../../../driver/state/client.h"
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Status) == 8);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Version) == 12);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Snapshot) == 32);

#define BC250_DEFAULT_HWID L"fake-adapter"
static unsigned Checks, Failures, Opens, Closes, Escapes, Reads;
static BOOL Admin = TRUE, TokenOk = TRUE;
static int OpenFail;
static NTSTATUS EscapeResult;
static uint32_t ReplyStatus = BC250_ESCAPE_STATUS_DONE, ReplyAbi = BC250_WAVE_ABI_VERSION;
static int Oversized;
static int OldDriver;
static LSTATUS ReadResult;
static DWORD ReadType = REG_BINARY, ReadBytes;
static BC250_WAVE_SNAPSHOT Fixture;
static char Output[65536];
static size_t OutputUsed;

#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; fprintf(stderr, "FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static int Capture(const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(Output + OutputUsed, sizeof(Output) - OutputUsed, format, args);
    va_end(args);
    if (n > 0 && (size_t)n < sizeof(Output) - OutputUsed) OutputUsed += (size_t)n;
    return n;
}

static int OpenAdapterById(const WCHAR *id, D3DKMT_HANDLE *adapter, NTSTATUS *status)
{
    CHECK(wcscmp(id, BC250_DEFAULT_HWID) == 0);
    ++Opens;
    *status = OpenFail ? (NTSTATUS)0xC00000A3L : 0;
    *adapter = OpenFail ? 0 : 17;
    return OpenFail;
}
static void CloseAdapterHandle(D3DKMT_HANDLE adapter) { CHECK(adapter == 17); ++Closes; }
static void PrintStatus(const char *call, NTSTATUS status) { (void)call; (void)status; }

static NTSTATUS APIENTRY FakeEscape(const D3DKMT_ESCAPE *escape)
{
    BC250_ESCAPE_WAVES *r = (BC250_ESCAPE_WAVES *)escape->pPrivateDriverData;
    D3DDDI_ESCAPEFLAGS expected = { 0 };
    expected.NoAdapterSynchronization = 1;
    ++Escapes;
    CHECK(escape->hAdapter == 17);
    CHECK(escape->Type == D3DKMT_ESCAPE_DRIVERPRIVATE);
    CHECK(escape->Flags.Value == expected.Value);
    CHECK(escape->PrivateDriverDataSize == sizeof(*r));
    CHECK(r->Magic == BC250_ESCAPE_MAGIC && r->Command == BC250_ESCAPE_GET_WAVES);
    CHECK(r->AbiVersion == BC250_WAVE_ABI_VERSION && r->Op == 0 && r->Reserved == 0);
    if (OldDriver) {
        /* The old driver's generic refusal writes the common prefix only. */
        ((BC250_ESCAPE *)r)->Status = BC250_ESCAPE_STATUS_REFUSED;
        return 0;
    }
    r->Status = ReplyStatus;
    r->AbiVersion = ReplyAbi;
    r->Version = 0x00070001;
    r->Snapshot = Fixture;
    r->NtStatus = Fixture.Header.Result;
    if (Oversized) r->Snapshot.Header.Bytes = sizeof(r->Snapshot) + 1;
    return EscapeResult;
}

static DWORD WINAPI FakeStateOpen(AMDGPU_WINDOWS_STATE **state, DWORD writable)
{
    CHECK(!writable); *state = (AMDGPU_WINDOWS_STATE *)1; return ERROR_SUCCESS;
}
static void WINAPI FakeStateClose(AMDGPU_WINDOWS_STATE *state) { CHECK(state == (AMDGPU_WINDOWS_STATE *)1); }
static DWORD WINAPI FakeRead(AMDGPU_WINDOWS_STATE *state, const char *path, const char *name,
                             DWORD effective, AMDGPU_STATE_VALUE *value, void *data, DWORD bytes)
{
    ++Reads;
    CHECK(state == (AMDGPU_WINDOWS_STATE *)1);
    CHECK(strcmp(path, "HangRecovery\\Waves") == 0 && strcmp(name, "Snapshot") == 0);
    CHECK(!effective && bytes == sizeof(Fixture));
    memset(value, 0, sizeof(*value));
    value->type = ReadType; value->bytes = ReadBytes;
    if (ReadResult) { value->status = AMDGPU_STATE_IO; return ReadResult; }
    CHECK(ReadBytes <= sizeof(Fixture));
    if (ReadBytes <= sizeof(Fixture)) memcpy(data, &Fixture, ReadBytes);
    return ERROR_SUCCESS;
}

static BOOL WINAPI FakeSid(PSID_IDENTIFIER_AUTHORITY authority, BYTE count, DWORD a, DWORD b,
                           DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h, PSID *sid)
{
    (void)authority;
    CHECK(count == 2 && a == SECURITY_BUILTIN_DOMAIN_RID && b == DOMAIN_ALIAS_RID_ADMINS);
    CHECK(c == 0 && d == 0 && e == 0 && f == 0 && g == 0 && h == 0);
    *sid = (PSID)(ULONG_PTR)42;
    return TRUE;
}
static BOOL WINAPI FakeMember(HANDLE token, PSID sid, PBOOL member)
{
    CHECK(token == NULL && sid == (PSID)(ULONG_PTR)42);
    *member = Admin;
    return TokenOk;
}
static PVOID WINAPI FakeFreeSid(PSID sid) { CHECK(sid == (PSID)(ULONG_PTR)42); return NULL; }

#define AllocateAndInitializeSid FakeSid
#define CheckTokenMembership FakeMember
#define FreeSid FakeFreeSid
#define D3DKMTEscape FakeEscape
#define AmdgpuClientOpen FakeStateOpen
#define AmdgpuClientClose FakeStateClose
#define AmdgpuClientRead FakeRead
#define printf Capture
#include "waves_command.h"
#undef printf

static void Reset(unsigned waves)
{
    BC250_WAVE_HEADER *h = &Fixture.Header;
    memset(&Fixture, 0, sizeof(Fixture));
    h->AbiVersion = BC250_WAVE_ABI_VERSION;
    h->DriverVersion = 0x00070001;
    h->Bytes = (uint32_t)(sizeof(*h) + waves * sizeof(BC250_WAVE_RECORD));
    h->WaveCount = waves;
    h->Flags = BC250_WAVE_COMPLETE;
    h->SeCount = h->ShPerSe = h->WgpPerSh = 1;
    h->SimdPerWgp = 4;
    h->WaveSlotsPerSimd = 20;
    h->SlotsVisited = 80;
    h->WgpVisited = 1;
    h->WgpMask[0][0] = 1;
    h->StartTime100ns = 5;
    h->EndTime100ns = 10;
    if (waves) {
        Fixture.Waves[0].Raw[Bc250WaveStatus] = 0x10000;
        Fixture.Waves[0].StatusAfter = 0x10000;
        Fixture.Waves[0].Raw[Bc250WavePcLo] = 0x1234;
        Fixture.Waves[0].Raw[Bc250WavePcHi] = 2;
    }
    Opens = Closes = Escapes = Reads = 0;
    Admin = TokenOk = TRUE;
    OpenFail = Oversized = 0;
    EscapeResult = 0;
    ReplyStatus = BC250_ESCAPE_STATUS_DONE;
    ReplyAbi = BC250_WAVE_ABI_VERSION;
    ReadResult = 0;
    ReadType = REG_BINARY;
    ReadBytes = h->Bytes;
    OutputUsed = 0;
    Output[0] = 0;
}

static uint32_t Field(unsigned reg, const char *name, uint32_t raw)
{
    size_t i;
    for (i = 0; i < sizeof(WaveFields) / sizeof(WaveFields[0]); ++i)
        if (WaveFields[i].Register == reg && !strcmp(WaveFields[i].Name, name))
            return WaveFieldValue(&WaveFields[i], raw);
    CHECK(0);
    return 0xffffffffu;
}

static void DecodeTests(void)
{
    unsigned reg;
    Reset(1);
    CHECK(Field(Bc250WaveStatus, "VALID", 0x00013002) == 1);
    CHECK(Field(Bc250WaveStatus, "HALT", 0x00013002) == 1);
    CHECK(Field(Bc250WaveStatus, "SPI_PRIO", 0x00013002) == 1);
    CHECK(Field(Bc250WaveStatus, "IN_BARRIER", 0x00013002) == 1);
    CHECK(Field(Bc250WavePcLo, "PC_LO", 0x89abcdef) == 0x89abcdef);
    CHECK(Field(Bc250WavePcHi, "PC_HI", 0xffff1234) == 0x1234);
    CHECK(Field(Bc250WaveExecLo, "EXEC_LO", 0x89abcdef) == 0x89abcdef);
    CHECK(Field(Bc250WaveExecHi, "EXEC_HI", 0xfedcba98) == 0xfedcba98);
    CHECK(Field(Bc250WaveHwId1, "WAVE_ID", 0x00051b15) == 21);
    CHECK(Field(Bc250WaveHwId1, "SIMD_ID", 0x00051b15) == 3);
    CHECK(Field(Bc250WaveHwId1, "WGP_ID", 0x00051b15) == 6);
    CHECK(Field(Bc250WaveHwId1, "SA_ID", 0x00051b15) == 1);
    CHECK(Field(Bc250WaveHwId1, "SE_ID", 0x00051b15) == 1);
    CHECK(Field(Bc250WaveHwId2, "VM_ID", 0x250a2317) == 5);
    CHECK(Field(Bc250WaveHwId2, "WG_ID", 0x250a2317) == 10);
    CHECK(Field(Bc250WaveHwId2, "COMPAT_LEVEL", 0x250a2317) == 1);
    CHECK(Field(Bc250WaveInstDw0, "INST_DW0", 0xbe801300) == 0xbe801300);
    CHECK(Field(Bc250WaveGprAlloc, "VGPR_BASE", 0x0c563412) == 0x12);
    CHECK(Field(Bc250WaveGprAlloc, "SGPR_SIZE", 0x0c563412) == 12);
    CHECK(Field(Bc250WaveLdsAlloc, "LDS_BASE", 0x070ab123) == 0x123);
    CHECK(Field(Bc250WaveLdsAlloc, "LDS_SIZE", 0x070ab123) == 0xab);
    CHECK(Field(Bc250WaveLdsAlloc, "VGPR_SHARED_SIZE", 0x070ab123) == 7);
    CHECK(Field(Bc250WaveTrapsts, "BUFFER_OOB", 0x90008001) == 1);
    CHECK(Field(Bc250WaveTrapsts, "DP_RATE", 0x90008001) == 4);
    CHECK(Field(Bc250WaveTrapsts, "XNACK_ERROR", 0x90008001) == 1);
    CHECK(Field(Bc250WaveIbSts, "VM_CNT", 0x01800589) == 9);
    CHECK(Field(Bc250WaveIbSts, "VM_CNT_HI", 0x01800589) == 2);
    CHECK(Field(Bc250WaveIbSts, "LGKM_CNT", 0x01800589) == 5);
    CHECK(Field(Bc250WaveIbSts, "LGKM_CNT_BIT4", 0x01800589) == 1);
    CHECK(Field(Bc250WaveIbSts, "LGKM_CNT_BIT5", 0x01800589) == 1);
    CHECK(Field(Bc250WaveIbSts2, "WAVE64", 0x3802) == 1);
    CHECK(Field(Bc250WaveIbSts2, "INST_PREFETCH", 0x3802) == 2);
    CHECK(Field(Bc250WaveIbDbg1, "WAVE_IDLE", 0x011c0bd9) == 1);
    CHECK(Field(Bc250WaveIbDbg1, "XNACK_ERROR", 0x011c0bd9) == 1);
    CHECK(Field(Bc250WaveM0, "M0", 0xfedcba98) == 0xfedcba98);
    CHECK(Field(Bc250WaveMode, "FP_ROUND", 0xa0800356) == 6);
    CHECK(Field(Bc250WaveMode, "FP_DENORM", 0xa0800356) == 5);
    CHECK(Field(Bc250WaveMode, "CSP", 0xa0800356) == 5);
    CHECK(WavePc(&Fixture.Waves[0]) == 0x200001234ull);
    WavePrint(&Fixture);
    for (reg = 0; reg < Bc250WaveRegisterCount; ++reg) CHECK(strstr(Output, WaveRegisterNames[reg]) != NULL);
    CHECK(strstr(Output, "SQ_WAVE_STATUS_AFTER raw=") != NULL);
    CHECK(strstr(Output, "SQ_WAVE_HW_ID2_AFTER raw=") != NULL);
    CHECK(strstr(Output, "not an atomic context image") != NULL);
}

static void BlobTests(void)
{
    const char *error;
    unsigned char *unaligned;
    unsigned i;
    Reset(1);
    CHECK(WaveValidateBlob(&Fixture, Fixture.Header.Bytes, &error));
    CHECK(!WaveValidateBlob(NULL, Fixture.Header.Bytes, &error));
    CHECK(!WaveValidateBlob(&Fixture, sizeof(BC250_WAVE_HEADER) - 1, &error));
    CHECK(!WaveValidateBlob(&Fixture, Fixture.Header.Bytes - 1, &error));
    CHECK(!WaveValidateBlob(&Fixture, Fixture.Header.Bytes + 1, &error));
    unaligned = (unsigned char *)malloc(Fixture.Header.Bytes + 1);
    CHECK(unaligned != NULL);
    if (unaligned) {
        memcpy(unaligned + 1, &Fixture, Fixture.Header.Bytes);
        CHECK(WaveValidateBlob(unaligned + 1, Fixture.Header.Bytes, &error));
        free(unaligned);
    }
#define BAD_HEADER(member, value) do { Reset(1); Fixture.Header.member = (value); CHECK(!WaveValidateBlob(&Fixture, ReadBytes, &error)); } while (0)
    BAD_HEADER(AbiVersion, 0);
    BAD_HEADER(Bytes, 0);
    BAD_HEADER(WaveCount, BC250_WAVE_MAX_RECORDS + 1);
    BAD_HEADER(WaveCount, 0xffffffffu);
    BAD_HEADER(Kind, 2);
    BAD_HEADER(Flags, 0x80000000);
    BAD_HEADER(Reserved0, 1);
    BAD_HEADER(EndTime100ns, 4);
    BAD_HEADER(SeCount, 3);
    BAD_HEADER(ShPerSe, 3);
    BAD_HEADER(WgpPerSh, 17);
    BAD_HEADER(SimdPerWgp, 5);
    BAD_HEADER(WaveSlotsPerSimd, 33);
    BAD_HEADER(SeCount, 0);
    BAD_HEADER(SlotsVisited, 0);
    BAD_HEADER(SlotsVisited, 81);
    BAD_HEADER(WgpVisited, 2);
    BAD_HEADER(WgpMask[0][0], 2);
    BAD_HEADER(WgpMask[1][0], 1);
    BAD_HEADER(WgpMask[0][0], 0);
    for (i = 0; i < 4; ++i) { Reset(1); Fixture.Header.Reserved[i] = 1; CHECK(!WaveValidateBlob(&Fixture, ReadBytes, &error)); }
#undef BAD_HEADER
#define BAD_RECORD(member, value) do { Reset(1); Fixture.Waves[0].member = (value); CHECK(!WaveValidateBlob(&Fixture, ReadBytes, &error)); } while (0)
    BAD_RECORD(Se, 1);
    BAD_RECORD(Sh, 1);
    BAD_RECORD(Wgp, 1);
    BAD_RECORD(Simd, 4);
    BAD_RECORD(Slot, 20);
    BAD_RECORD(Raw[Bc250WaveStatus], 0);
#undef BAD_RECORD
}

static void CommandTests(void)
{
    WCHAR *plain[] = { L"cli", L"waves" };
    WCHAR *saved[] = { L"cli", L"waves", L"saved" };
    WCHAR *idle[] = { L"cli", L"waves", L"--expect-idle" };
    WCHAR *pc[] = { L"cli", L"waves", L"--expect-pc", L"0x200001200", L"0x200001300" };
    WCHAR *extra[] = { L"cli", L"waves", L"saved", L"--expect-idle", L"extra" };
    uint64_t address;
    Reset(0);
    OldDriver = 1;
    CHECK(WavesCommand(2, plain) == 1 && Escapes == 1 && Closes == 1);
    OldDriver = 0;
    Reset(0);
    CHECK(WavesCommand(3, idle) == 0 && Escapes == 1 && Closes == 1);
    CHECK(strstr(Output, "expect-idle: PASS") != NULL);
    Reset(1);
    CHECK(WavesCommand(2, plain) == 0 && Escapes == 1 && Closes == 1);
    Reset(1);
    CHECK(WavesCommand(5, pc) == 0 && strstr(Output, "stable_matches=1") != NULL);
    Reset(0); CHECK(WavesCommand(5, pc) == 1);
    Reset(1); CHECK(WavesCommand(3, idle) == 1);
    Reset(1); Fixture.Waves[0].Raw[Bc250WavePcLo] = 0x1300; CHECK(WavesCommand(5, pc) == 1);
    Reset(1); Fixture.Waves[0].HwId1After = 1; CHECK(WavesCommand(5, pc) == 1);
    Reset(1); Fixture.Waves[0].HwId2After = 1; CHECK(WavesCommand(5, pc) == 1);
    Reset(1); Fixture.Waves[0].StatusAfter = 0; CHECK(WavesCommand(5, pc) == 1);
    Reset(0); Fixture.Header.Flags = BC250_WAVE_BUDGET_EXPIRED; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); Fixture.Header.Flags |= BC250_WAVE_TRUNCATED; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); Fixture.Header.InvalidReads = 1; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); Fixture.Header.Result = (int32_t)0xc0000001; CHECK(WavesCommand(3, idle) == 1);
    Reset(1); Fixture.Header.Result = (int32_t)0xc00000b5; Fixture.Header.Flags = BC250_WAVE_BUDGET_EXPIRED;
    ReplyStatus = BC250_ESCAPE_STATUS_REFUSED;
    CHECK(WavesCommand(2, plain) == 1 && strstr(Output, "wave[0]") != NULL);
    Reset(0); Fixture.Header.SlotsVisited = 0; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); Fixture.Header.WgpVisited = 0; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); Fixture.Header.WgpMask[0][0] = 0; CHECK(WavesCommand(3, idle) == 1);
    Reset(0); OpenFail = 1; CHECK(WavesCommand(2, plain) == 1 && !Escapes && !Closes);
    Reset(0); EscapeResult = (NTSTATUS)0xc0000022; CHECK(WavesCommand(2, plain) == 1 && Escapes == 1 && Closes == 1);
    Reset(0); ReplyStatus = BC250_ESCAPE_STATUS_REFUSED; CHECK(WavesCommand(2, plain) == 1 && Escapes == 1);
    Reset(0); ReplyAbi = 99; CHECK(WavesCommand(2, plain) == 1);
    Reset(0); Oversized = 1; CHECK(WavesCommand(2, plain) == 1);
    Reset(0); Admin = FALSE; CHECK(WavesCommand(2, plain) == 3 && !Opens && !Reads);
    Reset(0); TokenOk = FALSE; CHECK(WavesCommand(3, saved) == 3 && !Opens && !Reads);
    Reset(1); Fixture.Header.Kind = BC250_WAVE_KIND_PRE_KILL; CHECK(WavesCommand(3, saved) == 0 && Reads == 1 && !Opens && !Escapes);
    CHECK(strstr(Output, "historical snapshot; not verified against latest recovery record") != NULL);
    Reset(0); CHECK(WavesCommand(3, saved) == 1);
    Reset(0); ReadBytes = 16; CHECK(WavesCommand(3, saved) == 1);
    Reset(0); ReadResult = ERROR_MORE_DATA; CHECK(WavesCommand(3, saved) == 1);
    Reset(0); ReadResult = ERROR_FILE_NOT_FOUND; CHECK(WavesCommand(3, saved) == 1);
    Reset(0); ReadResult = ERROR_ACCESS_DENIED; CHECK(WavesCommand(3, saved) == 1);
    Reset(0); ReadType = REG_SZ; CHECK(WavesCommand(3, saved) == 1);
    Reset(0); CHECK(WavesCommand(5, extra) == 2 && !Reads && !Opens);
    CHECK(!WaveParseAddress(L"-1", &address));
    CHECK(!WaveParseAddress(L"+1", &address));
    CHECK(!WaveParseAddress(L" 1", &address));
    CHECK(!WaveParseAddress(L"0x", &address));
    CHECK(!WaveParseAddress(L"10000000000000000", &address));
    CHECK(!WaveParseAddress(L"12z", &address));
    CHECK(WaveParseAddress(L"FFFFFFFFFFFFFFFF", &address) && address == UINT64_MAX);
    CHECK(WaveParseAddress(L"0", &address) && address == 0);
}

int main(void)
{
    DecodeTests();
    BlobTests();
    CommandTests();
    printf("wave decode, blob and command tests: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
