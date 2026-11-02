// bd167probe - the bounded lab positive control of BD-167 fix 9 (scratch audit part 4): does the register write
// escape of bc250kmd reach the GPU only in the lab flavour?
//
//   bd167probe --expect release|lab [--pattern <hex32>] [--no-witness]
//   bd167probe --self-test
//
// The register is GC.SCRATCH_REG0. Its BAR5 offset is BC250_REG_GC_SCRATCH_REG0 of driver/kmd/regs.generated.h
// (gen_regs.py through tools/regcalc; build.ps1 compares it with `regcalc.py lookup mmSCRATCH_REG0`). It is the only
// entry of the KMD's write allow list (g_MmioWriteAllow, experiment E07), amdgpu's own ring-test target, and a value
// in it has no function. The two probe writes of tools/diagusb are a write and a restore of the same register.
//
// One run:
//   1. the adapter: every adapter of D3DKMTEnumAdapters2 gets BC250_ESCAPE_GET_INFO; ours answers with the magic.
//   2. the witness: bc250rd.sys (tools/win/bc250rd, a separate read-only BAR5 reader that does not depend on the
//      flavour of bc250kmd) reads the register before the write (w0), after it (w1) and after the restore (w2).
//   3. BC250_ESCAPE_READ_REG, then BC250_ESCAPE_WRITE_REG with the pattern, then (only when the write was done)
//      WRITE_REG with w0 again.
//   release: both escapes must answer UNKNOWN_COMMAND (display.c compiles them only into the lab flavour), and the
//            witness must read the same value before and after, not the pattern.
//   lab:     READ_REG must give w0, WRITE_REG must read the pattern back and the witness must see it, and the
//            restore must put w0 back (witness w2 == w0). The lab path needs LabDiagnostics = 1 and
//            EnableMmioWrite = 1 at the driver's start, and test signing on (lab_gate.c, mmio.c).
// The last line is
//   BD167 VERDICT result=<r> expect=<e> read=<status> write=<status> restore=<status> w0=.. w1=.. w2=.. pattern=..
// Exit 0 pass, 1 fail (a release that writes, a lab mismatch, a failed restore), 2 the lab path is closed,
// 3 no bc250 adapter or no witness, 4 usage.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../driver/kmd/bc250kmd_escape.h"     // shared with the driver, never copied
#include "../../../driver/kmd/regs.generated.h"      // BC250_REG_GC_SCRATCH_REG0 (build.ps1: on g_MmioWriteAllow)
#include "../bc250rd/bc250rd_ioctl.h"                // the witness

#define BD167_REG BC250_REG_GC_SCRATCH_REG0
#define BD167_DEFAULT_PATTERN 0xBC250F09ul          // not a value the shim's ring tests write (0xCAFEDEAD, 0xDEADBEEF)
#define BD167_NO_VALUE 0xFFFFFFFFul
#define BD167_STATUS_NOT_SENT 0xFFFFFFFFul

typedef char BD167_REG_IN_BAR5[(BD167_REG < BC250RD_BAR5_SIZE && (BD167_REG & 3u) == 0) ? 1 : -1];

enum { EXPECT_RELEASE = 1, EXPECT_LAB = 2 };

typedef struct {
    int Expect;
    int Witness;                  // 1: w0, w1, w2 were read
    unsigned long Pattern;
    unsigned long ReadStatus, ReadValue;
    unsigned long WriteStatus, WriteValue, WriteNt;
    unsigned long RestoreStatus, RestoreValue;
    unsigned long W0, W1, W2;
} BD167_OBS;

typedef struct {
    const char *Result;
    int Code;
} BD167_VERDICT;

static const char *StatusName(unsigned long s)
{
    switch (s) {
    case BC250_ESCAPE_STATUS_DONE: return "done";
    case BC250_ESCAPE_STATUS_UNKNOWN_COMMAND: return "unknown-command";
    case BC250_ESCAPE_STATUS_REFUSED: return "refused";
    case BC250_ESCAPE_STATUS_NOT_ADMIN: return "not-admin";
    case BD167_STATUS_NOT_SENT: return "not-sent";
    default: return "other";
    }
}

// Pure: the verdict of one run from what it saw. The self-test feeds it made-up runs.
static BD167_VERDICT Judge(const BD167_OBS *o)
{
    BD167_VERDICT v = { "fail", 1 };
    if (o->Expect == EXPECT_RELEASE) {
        if (o->ReadStatus == BC250_ESCAPE_STATUS_DONE || o->WriteStatus == BC250_ESCAPE_STATUS_DONE) {
            v.Result = "release-escape-open"; return v;            // the raw path is in a release driver
        }
        if (o->WriteStatus != BC250_ESCAPE_STATUS_UNKNOWN_COMMAND || o->ReadStatus != BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) {
            v.Result = "release-unexpected-status"; return v;      // refused or not-admin is a gate, not the absence
        }
        if (!o->Witness) { v.Result = "no-witness"; v.Code = 3; return v; }
        if (o->W1 == o->Pattern || o->W1 != o->W0) { v.Result = "release-register-changed"; return v; }
        v.Result = "pass"; v.Code = 0; return v;
    }
    if (o->ReadStatus == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND || o->WriteStatus == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND ||
        o->WriteStatus == BC250_ESCAPE_STATUS_REFUSED || o->ReadStatus == BC250_ESCAPE_STATUS_REFUSED ||
        o->WriteStatus == BC250_ESCAPE_STATUS_NOT_ADMIN) {
        if (o->WriteStatus == BC250_ESCAPE_STATUS_DONE) { v.Result = "lab-read-closed-write-open"; return v; }
        v.Result = "lab-path-closed"; v.Code = 2; return v;
    }
    if (o->ReadStatus != BC250_ESCAPE_STATUS_DONE || o->WriteStatus != BC250_ESCAPE_STATUS_DONE) {
        v.Result = "lab-unexpected-status"; return v;
    }
    if (o->RestoreStatus != BC250_ESCAPE_STATUS_DONE || o->RestoreValue != o->ReadValue) { v.Result = "lab-restore-failed"; return v; }
    if (o->WriteValue != o->Pattern) { v.Result = "lab-readback-mismatch"; return v; }
    if (!o->Witness) { v.Result = "no-witness"; v.Code = 3; return v; }
    if (o->ReadValue != o->W0) { v.Result = "lab-read-differs-from-witness"; return v; }
    if (o->W1 != o->Pattern) { v.Result = "lab-witness-misses-write"; return v; }
    if (o->W2 != o->W0) { v.Result = "lab-witness-misses-restore"; return v; }
    v.Result = "pass"; v.Code = 0; return v;
}

// ---- the adapter -----------------------------------------------------------------------------------------------

static NTSTATUS KmdEscape(D3DKMT_HANDLE adapter, BC250_ESCAPE *data)
{
    D3DKMT_ESCAPE e = { 0 };
    e.hAdapter = adapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.Flags.HardwareAccess = 1;     // as bc250kmd_cli sends READ_REG and WRITE_REG
    e.pPrivateDriverData = data;
    e.PrivateDriverDataSize = sizeof(*data);
    return D3DKMTEscape(&e);
}

static void Request(BC250_ESCAPE *d, unsigned long command)
{
    memset(d, 0, sizeof(*d));
    d->Magic = BC250_ESCAPE_MAGIC;
    d->Command = command;
}

// The PCI identity of an adapter, so that the private escape goes only to the BC-250 GPU (1002:13FE, the device
// the witness reads too), never to another vendor's driver.
static int IsBc250(D3DKMT_HANDLE adapter)
{
    D3DKMT_QUERY_DEVICE_IDS ids = { 0 };
    D3DKMT_QUERYADAPTERINFO q = { 0 };

    q.hAdapter = adapter;
    q.Type = KMTQAITYPE_PHYSICALADAPTERDEVICEIDS;
    q.pPrivateDriverData = &ids;
    q.PrivateDriverDataSize = sizeof(ids);
    if (!NT_SUCCESS(D3DKMTQueryAdapterInfo(&q))) return 0;
    return ids.DeviceIds.VendorID == BC250RD_VENDOR_ID && ids.DeviceIds.DeviceID == BC250RD_DEVICE_ID;
}

// Opens every adapter, keeps the first BC-250 one that answers GET_INFO with our magic. 0 when none does.
static D3DKMT_HANDLE FindOurs(BC250_ESCAPE *info)
{
    D3DKMT_ADAPTERINFO list[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    D3DKMT_HANDLE ours = 0;

    en.NumAdapters = 16;
    en.pAdapters = list;
    if (!NT_SUCCESS(D3DKMTEnumAdapters2(&en))) return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        D3DKMT_CLOSEADAPTER close = { 0 };
        if (!ours && IsBc250(list[i].hAdapter)) {
            Request(info, BC250_ESCAPE_GET_INFO);
            if (NT_SUCCESS(KmdEscape(list[i].hAdapter, info)) && info->Magic == BC250_ESCAPE_MAGIC &&
                info->Status == BC250_ESCAPE_STATUS_DONE) {
                ours = list[i].hAdapter;
                printf("BD167 ADAPTER luid=%08lX:%08lX version=0x%08lX flags=0x%08lX full_wddm=%d mmio_write_gate=%s\n",
                       (unsigned long)list[i].AdapterLuid.HighPart, list[i].AdapterLuid.LowPart, info->Version, info->Flags,
                       (info->Flags & BC250_ESCAPE_FLAG_FULL_WDDM) ? 1 : 0, (info->Flags & BC250_ESCAPE_FLAG_MMIO_WRITE) ? "on" : "off");
                continue;
            }
        }
        close.hAdapter = list[i].hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    return ours;
}

// ---- the witness -----------------------------------------------------------------------------------------------

static HANDLE OpenWitness(void)
{
    BC250RD_INFO info;
    DWORD got = 0;
    HANDLE h = CreateFileA(BC250RD_DEVICE_USER, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("BD167 WITNESS absent: %s does not open (error %lu)\n", BC250RD_DEVICE_USER, GetLastError());
        return NULL;
    }
    if (!DeviceIoControl(h, IOCTL_BC250RD_ATTACH, NULL, 0, &info, sizeof(info), &got, NULL)) {
        printf("BD167 WITNESS absent: attach failed (error %lu)\n", GetLastError());
        CloseHandle(h);
        return NULL;
    }
    printf("BD167 WITNESS bc250rd %04X:%04X bus %lu allow %lu\n", info.VendorId, info.DeviceId, info.Bus, info.AllowCount);
    return h;
}

static int WitnessRead(HANDLE h, unsigned long *value)
{
    ULONG v = BD167_REG;
    DWORD got = 0;
    if (!h || !DeviceIoControl(h, IOCTL_BC250RD_READ, &v, sizeof(v), &v, sizeof(v), &got, NULL) || got != sizeof(v)) return 0;
    *value = v;
    return 1;
}

// ---- the self-test ---------------------------------------------------------------------------------------------

static int SelfTest(void)
{
    struct { const char *Name; BD167_OBS O; const char *Want; } cases[] = {
        { "release refuses both, register unchanged",
          { EXPECT_RELEASE, 1, 0xBC250F09, 1, 0, 1, 0, 0, BD167_STATUS_NOT_SENT, 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, "pass" },
        { "release with the write path open",
          { EXPECT_RELEASE, 1, 0xBC250F09, 1, 0, 0, 0xBC250F09, 0, 0, 0xDEADBEEF, 0xDEADBEEF, 0xBC250F09, 0xDEADBEEF }, "release-escape-open" },
        { "release refuses, but the register holds the pattern",
          { EXPECT_RELEASE, 1, 0xBC250F09, 1, 0, 1, 0, 0, BD167_STATUS_NOT_SENT, 0, 0xDEADBEEF, 0xBC250F09, 0xBC250F09 }, "release-register-changed" },
        { "release answers refused (a runtime gate, not the compile-time absence)",
          { EXPECT_RELEASE, 1, 0xBC250F09, 2, 0, 2, 0, 0, BD167_STATUS_NOT_SENT, 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, "release-unexpected-status" },
        { "release without the witness",
          { EXPECT_RELEASE, 0, 0xBC250F09, 1, 0, 1, 0, 0, BD167_STATUS_NOT_SENT, 0, 0, 0, 0 }, "no-witness" },
        { "lab writes, the witness sees it, the restore holds",
          { EXPECT_LAB, 1, 0xBC250F09, 0, 0xDEADBEEF, 0, 0xBC250F09, 0, 0, 0xDEADBEEF, 0xDEADBEEF, 0xBC250F09, 0xDEADBEEF }, "pass" },
        { "lab with EnableMmioWrite off",
          { EXPECT_LAB, 1, 0xBC250F09, 0, 0xDEADBEEF, 2, 0, 0xC00000A3, BD167_STATUS_NOT_SENT, 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, "lab-path-closed" },
        { "lab with LabDiagnostics off (the escapes fall through)",
          { EXPECT_LAB, 1, 0xBC250F09, 1, 0, 1, 0, 0, BD167_STATUS_NOT_SENT, 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, "lab-path-closed" },
        { "lab whose restore fails",
          { EXPECT_LAB, 1, 0xBC250F09, 0, 0xDEADBEEF, 0, 0xBC250F09, 0, 0, 0xBC250F09, 0xDEADBEEF, 0xBC250F09, 0xBC250F09 }, "lab-restore-failed" },
        { "lab whose write the witness does not see",
          { EXPECT_LAB, 1, 0xBC250F09, 0, 0xDEADBEEF, 0, 0xBC250F09, 0, 0, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, "lab-witness-misses-write" },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        BD167_VERDICT v = Judge(&cases[i].O);
        int ok = !strcmp(v.Result, cases[i].Want);
        printf("%s %s: %s\n", ok ? "PASS" : "FAIL", cases[i].Name, v.Result);
        bad += !ok;
    }
    printf("%s: %u self-test checks\n", bad ? "FAIL" : "PASS", (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return bad ? 1 : 0;
}

static int Usage(const char *why)
{
    printf("USAGE %s\nbd167probe --expect release|lab [--pattern <hex32>] [--no-witness] | --self-test\n", why);
    return 4;
}

int main(int argc, char **argv)
{
    BD167_OBS o;
    BC250_ESCAPE d;
    int noWitness = 0;
    HANDLE witness = NULL;
    D3DKMT_HANDLE adapter;
    D3DKMT_CLOSEADAPTER close = { 0 };
    BD167_VERDICT v;

    memset(&o, 0, sizeof(o));
    o.Pattern = BD167_DEFAULT_PATTERN;
    o.ReadStatus = o.WriteStatus = o.RestoreStatus = BD167_STATUS_NOT_SENT;
    o.W0 = o.W1 = o.W2 = o.ReadValue = o.WriteValue = o.RestoreValue = BD167_NO_VALUE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return SelfTest();
        if (!strcmp(argv[i], "--no-witness")) { noWitness = 1; continue; }
        if (i + 1 >= argc) return Usage("unknown option or missing value");
        if (!strcmp(argv[i], "--expect")) {
            i++;
            if (!strcmp(argv[i], "release")) o.Expect = EXPECT_RELEASE;
            else if (!strcmp(argv[i], "lab")) o.Expect = EXPECT_LAB;
            else return Usage("--expect is release or lab");
        } else if (!strcmp(argv[i], "--pattern")) {
            char *end = NULL;
            o.Pattern = strtoul(argv[++i], &end, 16);
            if (!end || *end || argv[i][0] == '-') return Usage("--pattern is a hex number");
        } else return Usage("unknown option or missing value");
    }
    if (!o.Expect) return Usage("--expect is required");
    printf("BD167 START expect=%s reg=GC.SCRATCH_REG0 offset=0x%05lX pattern=0x%08lX\n",
           o.Expect == EXPECT_RELEASE ? "release" : "lab", (unsigned long)BD167_REG, o.Pattern);

    adapter = FindOurs(&d);
    if (!adapter) { printf("BD167 VERDICT result=no-adapter expect=%s\n", o.Expect == EXPECT_RELEASE ? "release" : "lab"); return 3; }
    if (!noWitness) witness = OpenWitness();
    o.Witness = witness && WitnessRead(witness, &o.W0);
    if (o.Witness && o.W0 == o.Pattern) o.Pattern = ~o.Pattern;    // a pattern the register does not hold already

    Request(&d, BC250_ESCAPE_READ_REG);
    d.RegOffset = BD167_REG;
    if (NT_SUCCESS(KmdEscape(adapter, &d))) { o.ReadStatus = d.Status; o.ReadValue = d.RegValue; }
    printf("BD167 READ status=%s value=0x%08lX nt=0x%08lX\n", StatusName(o.ReadStatus), o.ReadValue, d.NtStatus);

    Request(&d, BC250_ESCAPE_WRITE_REG);
    d.RegOffset = BD167_REG;
    d.RegValue = o.Pattern;
    if (NT_SUCCESS(KmdEscape(adapter, &d))) { o.WriteStatus = d.Status; o.WriteValue = d.RegValue; o.WriteNt = d.NtStatus; }
    printf("BD167 WRITE status=%s readback=0x%08lX nt=0x%08lX\n", StatusName(o.WriteStatus), o.WriteValue, o.WriteNt);
    if (o.Witness) o.Witness = WitnessRead(witness, &o.W1);

    if (o.WriteStatus == BC250_ESCAPE_STATUS_DONE) {
        // The restore: the value before the write, from the KMD's own read when it had one, else from the witness.
        unsigned long back = o.ReadStatus == BC250_ESCAPE_STATUS_DONE ? o.ReadValue : o.W0;
        Request(&d, BC250_ESCAPE_WRITE_REG);
        d.RegOffset = BD167_REG;
        d.RegValue = back;
        if (NT_SUCCESS(KmdEscape(adapter, &d))) { o.RestoreStatus = d.Status; o.RestoreValue = d.RegValue; }
        printf("BD167 RESTORE status=%s readback=0x%08lX wanted=0x%08lX\n", StatusName(o.RestoreStatus), o.RestoreValue, back);
        if (o.Witness) o.Witness = WitnessRead(witness, &o.W2);
    } else {
        o.W2 = o.W1;
    }
    if (witness) CloseHandle(witness);
    close.hAdapter = adapter;
    D3DKMTCloseAdapter(&close);

    v = Judge(&o);
    printf("BD167 VERDICT result=%s expect=%s read=%s write=%s restore=%s w0=0x%08lX w1=0x%08lX w2=0x%08lX pattern=0x%08lX\n",
           v.Result, o.Expect == EXPECT_RELEASE ? "release" : "lab", StatusName(o.ReadStatus), StatusName(o.WriteStatus),
           StatusName(o.RestoreStatus), o.W0, o.W1, o.W2, o.Pattern);
    return v.Code;
}
