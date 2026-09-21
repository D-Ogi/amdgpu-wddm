// Private escape data shared between bc250kmd and lab tools (D3DKMTEscape, D3DKMT_ESCAPE_DRIVERPRIVATE).
// dxgkrnl does route them to a display-only driver (facts M29), so this is the control channel for the
// bring-up experiments (ADR 0007). Register commands work only for an administrator, only while the registry
// gates of mmio.c are open, and only on offsets of the generated tables.
#pragma once

#define BC250_ESCAPE_MAGIC 0x30353242u      // "B250"
#define BC250_ESCAPE_GET_INFO 1u
#define BC250_ESCAPE_READ_REG 2u            // in: RegOffset (BAR5 byte offset); out: RegValue
#define BC250_ESCAPE_WRITE_REG 3u           // in: RegOffset, RegValue; out: RegValue read back after the write
#define BC250_KMD_VERSION 0x00040001u       // milestone 4 work, revision 1

#define BC250_ESCAPE_STATUS_DONE 0u
#define BC250_ESCAPE_STATUS_UNKNOWN_COMMAND 1u
#define BC250_ESCAPE_STATUS_REFUSED 2u      // NtStatus says why: gate closed, offset not in the table
#define BC250_ESCAPE_STATUS_NOT_ADMIN 3u

#define BC250_ESCAPE_FLAG_MMIO_MAPPED 1u
#define BC250_ESCAPE_FLAG_MMIO_WRITE 2u

typedef struct _BC250_ESCAPE {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long LastStage;                // out: BC250_STAGE
    unsigned long Width, Height, Pitch, ColorFormat;    // out: the firmware mode the driver runs on
    unsigned long Presents;                 // out: presents since start
    unsigned long RegOffset, RegValue;      // register commands
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Reserved[2];
} BC250_ESCAPE;
