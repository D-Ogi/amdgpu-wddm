// Interface between bc250rd.sys and its command line tool. Read-only towards the GPU by construction:
// there is no IOCTL that writes to it.
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

// Input: one SMN address (ULONG). Output: its value (ULONG). Only addresses in the driver's SMN allow-list.
// Goes through the root complex's SMN index/data pair in PCI configuration space (0x60/0x64), the way the
// Linux k10temp driver reads the same sensor. This is the only write the driver ever issues, and it goes
// to the host bridge's index register, not to the GPU.
#define IOCTL_BC250RD_SMN_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

#define BC250RD_HOST_BRIDGE_VENDOR 0x1022
#define BC250RD_HOST_BRIDGE_DEVICE 0x13E0      // Ariel Root Complex, 00:00.0
#define BC250RD_SMN_THM_TCON_CUR_TMP 0x00059800u

// Legacy SMU ABI retained so old diagnostic clients fail explicitly. Current
// bc250rd builds return STATUS_NOT_SUPPORTED and have no mailbox write mapping.
// Migrate clock/control callers to the native KMD interface before deployment.
#define IOCTL_BC250RD_SMU_MSG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define BC250RD_MP1_C2PMSG_66 0x58A08u   // message
#define BC250RD_MP1_C2PMSG_82 0x58A48u   // parameter / returned value
#define BC250RD_MP1_C2PMSG_90 0x58A68u   // response

#define BC250RD_SMU_TestMessage      0x01
#define BC250RD_SMU_GetSmuVersion    0x02
#define BC250RD_SMU_RequestGfxclk    0x0E   // parameter: MHz, 1000..2000
#define BC250RD_SMU_QueryGfxclk      0x0F
#define BC250RD_SMU_GetGfxFrequency  0x37
#define BC250RD_SMU_GetGfxVid        0x38
#define BC250RD_SMU_ForceGfxVid      0x3B   // parameter: vid = (1550 - mV) * 160 / 1000, for 700..900 mV (project ceiling)
#define BC250RD_SMU_UnforceGfxVid    0x3C

#define BC250RD_SCLK_MIN_MHZ 1000
#define BC250RD_SCLK_MAX_MHZ 1500
#define BC250RD_VDDC_MIN_MV  700
#define BC250RD_VDDC_MAX_MV  900
#define BC250RD_VID_FROM_MV(mv) ((1550u - (mv)) * 160u / 1000u)

typedef struct _BC250RD_SMU_MSG {
    unsigned long Message;
    unsigned long Parameter;
    unsigned long Response;     // out: 1 = OK, 0xFF failed, 0xFE unknown, 0xFD rejected, 0xFC busy, 0 = timeout
    unsigned long Value;        // out: parameter register after the message
} BC250RD_SMU_MSG;

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
