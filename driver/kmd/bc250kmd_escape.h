// Private escape data shared between bc250kmd and lab tools (D3DKMTEscape, D3DKMT_ESCAPE_DRIVERPRIVATE).
// M3 answers one read-only query. Whether dxgkrnl routes escapes to a display-only driver at all is one of
// the things the M3 driver is there to measure (ADR 0006, open questions).
#pragma once

#define BC250_ESCAPE_MAGIC 0x30353242u      // "B250"
#define BC250_ESCAPE_GET_INFO 1u
#define BC250_KMD_VERSION 0x00030001u       // milestone 3, revision 1

typedef struct _BC250_ESCAPE {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in
    unsigned long Status;                   // out: 0 = done, 1 = unknown command
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long LastStage;                // out: BC250_STAGE
    unsigned long Width, Height, Pitch, ColorFormat;    // out: the firmware mode the driver runs on
    unsigned long Presents;                 // out: presents since start
    unsigned long Reserved[6];
} BC250_ESCAPE;
