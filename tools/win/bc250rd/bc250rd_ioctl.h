// Interface between bc250rd.sys and its command line tool. Read-only by construction: there is no write IOCTL.
#pragma once

#define BC250RD_DEVICE_NT   L"\\Device\\Bc250Rd"
#define BC250RD_DEVICE_DOS  L"\\DosDevices\\Bc250Rd"
#define BC250RD_DEVICE_USER "\\\\.\\Bc250Rd"

#define BC250RD_VENDOR_ID   0x1002
#define BC250RD_DEVICE_ID   0x13FE
#define BC250RD_BAR5_SIZE   0x80000u

// Find the GPU on the PCI bus, check its ID, map BAR5 read-only. Output: BC250RD_INFO.
#define IOCTL_BC250RD_ATTACH CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input: array of BAR5 byte offsets (ULONG). Output: array of values (ULONG), same order.
// Every offset must be in the driver's allow-list, otherwise the whole request fails and nothing is read.
#define IOCTL_BC250RD_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#define BC250RD_MAX_READS 1024

typedef struct _BC250RD_INFO {
    unsigned long Bus, Device, Function;
    unsigned short VendorId, DeviceId, Command, Status;
    unsigned char RevisionId, Reserved[3];
    unsigned long Bars[6];              // raw BAR values from configuration space
    unsigned long long Bar5Physical;
    unsigned long Bar5Size;
    unsigned long AllowCount;
} BC250RD_INFO;
