/* bc250tdr: a controlled TDR on the BC-250 adapter, for the TDR recovery trials (docs/design/tdr-recovery-s1.md).
 *
 * It sends one D3DKMT_ESCAPE_TDRDBGCTRL escape to the adapter with PCI 1002:13FE (D3DKMTEnumAdapters2 and
 * KMTQAITYPE_PHYSICALADAPTERDEVICEIDS). dxgkrnl then runs its timeout path as if the GPU had hung:
 *   --force adapter  D3DKMT_TDRDBGCTRLTYPE_FORCETDR (0): a forced TDR, DXGK_TDR_TYPE_FORCED, an adapter-wide reset.
 *   --force engine   D3DKMT_TDRDBGCTRLTYPE_ENGINETDR (8) on --node N: an engine TDR. A refused ResetEngine is
 *                    promoted to an adapter-wide reset (tdr-changes-in-windows-8.md, TdrReason 9).
 * The escape is "for testing purposes only" and dxgkrnl admits it only when
 * HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\TdrTestMode is 1 (d3dkmthk.h, D3DKMT_ESCAPETYPE). This
 * program reads that value and reports it; it never writes the registry. Without --yes it only lists and checks.
 *
 * The data of the escape has two documented forms: an int with the D3DKMT_TDRDBGCTRLTYPE value
 * (D3DKMT_ESCAPETYPE, PrivateDriverDataSize = sizeof(int)), and D3DKMT_TDRDBGCTRL_ESCAPE, which also carries the
 * node of an engine TDR. --form int|struct selects one; the default is int for adapter and struct for engine. Which
 * form the lab's dxgkrnl accepts is not measured yet: the status of the call is printed either way.
 *
 * Output: one RESULT= line, exit 0 only when the escape returned STATUS_SUCCESS (or --list found the adapter). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3dkmthk.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

#define BC250_VENDOR 0x1002u
#define BC250_DEVICE 0x13FEu

typedef struct options {
    int List;
    int Force;              /* 0 none, 1 adapter, 2 engine */
    int Form;               /* 0 default, 1 int, 2 struct */
    unsigned Node;
    int Yes;
} OPTIONS;

static void Usage(void)
{
    printf("usage: bc250tdr --list\n"
           "       bc250tdr --force adapter [--form int|struct] [--yes]\n"
           "       bc250tdr --force engine --node N [--form int|struct] [--yes]\n"
           "Without --yes nothing is sent. The escape needs GraphicsDrivers\\TdrTestMode = 1.\n");
}

static int ParseArgs(int argc, char** argv, OPTIONS* o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->Node = 0xFFFFFFFFu;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) o->List = 1;
        else if (strcmp(argv[i], "--yes") == 0) o->Yes = 1;
        else if (strcmp(argv[i], "--force") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "adapter") == 0) o->Force = 1;
            else if (strcmp(argv[i], "engine") == 0) o->Force = 2;
            else return 0;
        } else if (strcmp(argv[i], "--form") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "int") == 0) o->Form = 1;
            else if (strcmp(argv[i], "struct") == 0) o->Form = 2;
            else return 0;
        } else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            char* end;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (*end != 0 || v > 63) return 0;
            o->Node = (unsigned)v;
        } else return 0;
    }
    if (o->List == (o->Force != 0)) return 0;              /* exactly one of --list and --force */
    if (o->Force == 2 && o->Node == 0xFFFFFFFFu) return 0;  /* an engine TDR names its node */
    if (o->Force == 1 && o->Node != 0xFFFFFFFFu) return 0;  /* an adapter TDR has no node */
    if (o->Form == 0) o->Form = o->Force == 2 ? 2 : 1;
    if (o->Force == 2 && o->Form == 1) return 0;            /* the int form cannot carry the node */
    return 1;
}

/* GraphicsDrivers\TdrTestMode, read only. -1: absent or not readable. */
static long ReadTdrTestMode(void)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                              L"TdrTestMode", RRF_RT_REG_DWORD, &type, &value, &size);
    return st == ERROR_SUCCESS ? (long)value : -1;
}

/* The LUID of the first adapter with PCI 1002:13FE. Every handle EnumAdapters2 opened is closed. */
static int FindAdapter(LUID* Luid)
{
    D3DKMT_ENUMADAPTERS2 enumerate;
    D3DKMT_ADAPTERINFO* adapters;
    NTSTATUS status;
    ULONG i, count;
    int found = 0;

    memset(&enumerate, 0, sizeof(enumerate));
    status = D3DKMTEnumAdapters2(&enumerate);
    if (!NT_SUCCESS(status) || enumerate.NumAdapters == 0) {
        printf("D3DKMTEnumAdapters2 (count) 0x%08lX, %lu adapter(s)\n", (unsigned long)status,
               (unsigned long)enumerate.NumAdapters);
        return 0;
    }
    count = enumerate.NumAdapters;
    adapters = (D3DKMT_ADAPTERINFO*)calloc(count, sizeof(*adapters));
    if (adapters == NULL) return 0;
    enumerate.pAdapters = adapters;
    status = D3DKMTEnumAdapters2(&enumerate);
    if (!NT_SUCCESS(status)) {
        printf("D3DKMTEnumAdapters2 0x%08lX\n", (unsigned long)status);
        free(adapters);
        return 0;
    }
    for (i = 0; i < enumerate.NumAdapters; i++) {
        D3DKMT_QUERYADAPTERINFO query;
        D3DKMT_QUERY_DEVICE_IDS ids;
        D3DKMT_CLOSEADAPTER close;

        memset(&query, 0, sizeof(query));
        memset(&ids, 0, sizeof(ids));
        query.hAdapter = adapters[i].hAdapter;
        query.Type = KMTQAITYPE_PHYSICALADAPTERDEVICEIDS;
        query.pPrivateDriverData = &ids;
        query.PrivateDriverDataSize = sizeof(ids);
        status = D3DKMTQueryAdapterInfo(&query);
        printf("adapter %lu: LUID %08lX:%08lX PCI %04X:%04X (query 0x%08lX)\n", (unsigned long)i,
               (unsigned long)adapters[i].AdapterLuid.HighPart, (unsigned long)adapters[i].AdapterLuid.LowPart,
               ids.DeviceIds.VendorID, ids.DeviceIds.DeviceID, (unsigned long)status);
        if (!found && NT_SUCCESS(status) && ids.DeviceIds.VendorID == BC250_VENDOR &&
            ids.DeviceIds.DeviceID == BC250_DEVICE) {
            *Luid = adapters[i].AdapterLuid;
            found = 1;
            printf("  ^ selected\n");
        }
        memset(&close, 0, sizeof(close));
        close.hAdapter = adapters[i].hAdapter;
        (void)D3DKMTCloseAdapter(&close);
    }
    free(adapters);
    return found;
}

int main(int argc, char** argv)
{
    OPTIONS o;
    LUID luid;
    D3DKMT_OPENADAPTERFROMLUID open;
    D3DKMT_CLOSEADAPTER close;
    D3DKMT_ESCAPE escape;
    D3DKMT_TDRDBGCTRL_ESCAPE control;
    int value;
    long testMode;
    NTSTATUS status;
    SYSTEMTIME now;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!ParseArgs(argc, argv, &o)) {
        Usage();
        printf("RESULT=USAGE\n");
        return 2;
    }
    testMode = ReadTdrTestMode();
    printf("TdrTestMode %ld%s\n", testMode, testMode == 1 ? "" : " (the escape needs 1; this program never writes it)");
    if (!FindAdapter(&luid)) {
        printf("RESULT=NO-ADAPTER\n");
        return 3;
    }
    if (o.List) {
        printf("RESULT=LISTED\n");
        return 0;
    }
    if (o.Force == 2)
        printf("plan: ENGINETDR on node %u, %s form\n", o.Node, o.Form == 2 ? "struct" : "int");
    else
        printf("plan: FORCETDR (adapter), %s form\n", o.Form == 2 ? "struct" : "int");
    if (!o.Yes) {
        printf("RESULT=DRY-RUN (add --yes to send)\n");
        return 0;
    }

    memset(&open, 0, sizeof(open));
    open.AdapterLuid = luid;
    status = D3DKMTOpenAdapterFromLuid(&open);
    if (!NT_SUCCESS(status)) {
        printf("D3DKMTOpenAdapterFromLuid 0x%08lX\nRESULT=OPEN-FAILED\n", (unsigned long)status);
        return 4;
    }
    memset(&escape, 0, sizeof(escape));
    memset(&control, 0, sizeof(control));
    escape.hAdapter = open.hAdapter;
    escape.Type = D3DKMT_ESCAPE_TDRDBGCTRL;
    if (o.Form == 2) {
        control.TdrControl = o.Force == 2 ? D3DKMT_TDRDBGCTRLTYPE_ENGINETDR : D3DKMT_TDRDBGCTRLTYPE_FORCETDR;
        control.NodeOrdinal = o.Force == 2 ? o.Node : 0;
        escape.pPrivateDriverData = &control;
        escape.PrivateDriverDataSize = sizeof(control);
    } else {
        value = (int)D3DKMT_TDRDBGCTRLTYPE_FORCETDR;
        escape.pPrivateDriverData = &value;
        escape.PrivateDriverDataSize = sizeof(value);
    }
    GetSystemTime(&now);
    printf("sent at %04u-%02u-%02uT%02u:%02u:%02u.%03uZ, %u bytes\n", now.wYear, now.wMonth, now.wDay, now.wHour,
           now.wMinute, now.wSecond, now.wMilliseconds, escape.PrivateDriverDataSize);
    status = D3DKMTEscape(&escape);
    printf("D3DKMTEscape(TDRDBGCTRL) 0x%08lX\n", (unsigned long)status);
    memset(&close, 0, sizeof(close));
    close.hAdapter = open.hAdapter;
    (void)D3DKMTCloseAdapter(&close);
    if (!NT_SUCCESS(status)) {
        printf("RESULT=ESCAPE-REFUSED status=0x%08lX\n", (unsigned long)status);
        return 5;
    }
    printf("RESULT=TDR-REQUESTED\n");
    return 0;
}
