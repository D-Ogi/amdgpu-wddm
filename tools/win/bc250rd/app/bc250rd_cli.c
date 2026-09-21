// bc250rd_cli - reads the registers listed in reglist.txt through bc250rd.sys and prints them in the format
// of the Linux sweep logs ("<ip>.<name> <offset> <value>"), so that the two can be compared line by line.
//
//   bc250rd_cli info
//   bc250rd_cli sweep reglist.txt [regex-free prefix filter, e.g. GC.]
//
// Like sweep_stream.py it prints the name and offset, flushes, and only then reads: if a read ever hangs the
// machine, the last, unfinished line on the other end of the SSH connection names the register.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bc250rd_ioctl.h"

static HANDLE OpenReader(void)
{
    HANDLE h = CreateFileA(BC250RD_DEVICE_USER, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
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

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "info") && strcmp(argv[1], "sweep")) || (!strcmp(argv[1], "sweep") && argc < 3)) {
        fprintf(stderr, "usage: bc250rd_cli info | sweep <reglist.txt> [name prefix]\n");
        return 2;
    }
    HANDLE h = OpenReader();
    if (h == INVALID_HANDLE_VALUE) return 1;
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
