// bc250rd_cli - reads the registers listed in reglist.txt through bc250rd.sys and prints them in the format
// of the Linux sweep logs ("<ip>.<name> <offset> <value>"), so that the two can be compared line by line.
//
//   bc250rd_cli info
//   bc250rd_cli temp [count [interval_s]]     SoC temperature (Tctl), the sensor Linux' k10temp reads
//   bc250rd_cli clock <MHz> <mV>                typed KMD transaction, no reader mailbox IOCTL
//   bc250rd_cli sweep reglist.txt [regex-free prefix filter, e.g. GC.]
//
// Like sweep_stream.py it prints the name and offset, flushes, and only then reads: if a read ever hangs the
// machine, the last, unfinished line on the other end of the SSH connection names the register.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bc250rd_ioctl.h"
#include "../../../../driver/kmd/bc250kmd_escape.h"
__declspec(dllimport) LONG WINAPI Bc250ClockControl(ULONG Op,ULONG MHz,ULONG Mv,BC250_ESCAPE_CLOCK* Data,ULONG Bytes);

static HANDLE OpenReader(void)
{
    HANDLE h = CreateFileA(BC250RD_DEVICE_USER, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fprintf(stderr, "cannot open %s (error %lu): is bc250rd.sys loaded, is this an elevated prompt?\n",
                BC250RD_DEVICE_USER, GetLastError());
    return h;
}

static int Attach(HANDLE h, BC250RD_INFO *info)
{
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_BC250RD_ATTACH, NULL, 0, info, sizeof(*info), &got, NULL)) {
        fprintf(stderr, "attach failed, error %lu\n", GetLastError());
        return 0;
    }
    return 1;
}

// Compatibility command names, one typed KMD backend shared with the monitor.
static int Clock(ULONG mhz,ULONG mv,int checkOnly)
{
    BC250_ESCAPE_CLOCK data;
    LONG status=Bc250ClockControl(checkOnly?BC250_CLOCK_OP_READ:BC250_CLOCK_OP_SET,
        checkOnly?0:mhz,checkOnly?0:mv,&data,sizeof(data));
    if(status<0) { fprintf(stderr,"KMD clock failed: 0x%08lX\n",(ULONG)status);return 0; }
    if(data.ObservedMHz!=mhz || data.ObservedVid!=BC250RD_VID_FROM_MV(mv)) {
        fprintf(stderr,"clock readback mismatch: MHz=%lu VID=%lu\n",data.ObservedMHz,data.ObservedVid);return 0;
    }
    printf("clock verified: %lu MHz, VID %lu (request %lu mV), backend=kmd-smu\n",mhz,data.ObservedVid,mv);
    return 1;
}

// THM_TCON_CUR_TMP: CUR_TEMP in bits 31:21, 0.125 C per step; bit 19 selects the -49 C range (as in k10temp).
static int ReadTemp(HANDLE h, double *celsius, ULONG *raw)
{
    ULONG v = BC250RD_SMN_THM_TCON_CUR_TMP;
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_BC250RD_SMN_READ, &v, sizeof(v), &v, sizeof(v), &got, NULL)) return 0;
    *raw = v;
    *celsius = (v >> 21) * 0.125 - ((v & (1u << 19)) ? 49.0 : 0.0);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "info") && strcmp(argv[1], "sweep") && strcmp(argv[1], "temp") &&
                     strcmp(argv[1], "clock") && strcmp(argv[1], "clock-check")) ||
        (!strcmp(argv[1], "sweep") && argc < 3) ||
        ((!strcmp(argv[1], "clock") || !strcmp(argv[1], "clock-check")) && argc < 4)) {
        fprintf(stderr, "usage: bc250rd_cli info | temp [count [interval_s]] | clock <MHz> <mV> | clock-check <MHz> <mV> | sweep <reglist.txt> [prefix]\n");
        return 2;
    }
    if (!strcmp(argv[1], "clock") || !strcmp(argv[1], "clock-check")) {
        char *end;
        ULONG mhz=strtoul(argv[2],&end,10);if(*end || argv[2][0]=='-')return 2;
        ULONG mv=strtoul(argv[3],&end,10);if(*end || argv[3][0]=='-')return 2;
        if(mhz<BC250RD_SCLK_MIN_MHZ || mhz>BC250RD_SCLK_MAX_MHZ ||
           mv<BC250RD_VDDC_MIN_MV || mv>BC250RD_VDDC_MAX_MV)return 2;
        return Clock(mhz,mv,!strcmp(argv[1],"clock-check"))?0:1;
    }
    HANDLE h = OpenReader();
    if (h == INVALID_HANDLE_VALUE) return 1;
    if (!strcmp(argv[1], "temp")) {
        int count = argc > 2 ? atoi(argv[2]) : 1, interval = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < count; i++) {
            double c; ULONG raw; SYSTEMTIME t;
            if (!ReadTemp(h, &c, &raw)) { fprintf(stderr, "temperature read failed, error %lu\n", GetLastError()); return 1; }
            GetLocalTime(&t);
            printf("%02u:%02u:%02u Tctl %.1f C (raw %08lX)\n", t.wHour, t.wMinute, t.wSecond, c, raw);
            fflush(stdout);
            if (i + 1 < count) Sleep(interval * 1000);
        }
        return 0;
    }
    BC250RD_INFO info;
    if (!Attach(h, &info)) return 1;
    printf("# bc250rd pci=%02lx:%02lx.%lx id=%04x:%04x rev=%02x command=%04x status=%04x bar5=0x%llx size=0x%lx allow=%lu\n",
           info.Bus, info.Device, info.Function, info.VendorId, info.DeviceId, info.RevisionId, info.Command,
           info.Status, info.Bar5Physical, info.Bar5Size, info.AllowCount);
    printf("# bars %08lx %08lx %08lx %08lx %08lx %08lx\n", info.Bars[0], info.Bars[1], info.Bars[2],
           info.Bars[3], info.Bars[4], info.Bars[5]);
    if (!strcmp(argv[1], "info")) return 0;

    FILE *f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    const char *prefix = argc > 3 ? argv[3] : "";
    char line[256], name[200];
    unsigned long off;
    int denied = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%199s %lx", name, &off) != 2 || strncmp(name, prefix, strlen(prefix))) continue;
        printf("%s 0x%05lx ", name, off);
        fflush(stdout);
        Sleep(4);
        ULONG value = off;
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_BC250RD_READ, &value, sizeof(value), &value, sizeof(value), &got, NULL)) {
            printf("%08lX\n", value);
        } else {
            printf("ERROR %lu\n", GetLastError());
            denied++;
        }
    }
    printf("# complete%s\n", denied ? " (with errors)" : "");
    fflush(stdout);
    return denied ? 1 : 0;
}
