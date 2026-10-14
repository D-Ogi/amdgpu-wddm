// Bounded typed handoff, not an arbitrary registry export. All integers are LE.
// Only the installer may supply this file. ACL/owner/path checks are performed
// by the OS adapter; the checksum detects damage and does not grant authority.
#pragma once
#include "state.h"
#define AMDGPU_TRANSFER_MAGIC 0x32545341u
#define AMDGPU_TRANSFER_VERSION 1u
#define AMDGPU_TRANSFER_MAX_BYTES 1048576u
#define AMDGPU_TRANSFER_FRESH 1u
#define AMDGPU_TRANSFER_LEGACY 2u
#define AMDGPU_TRANSFER_CURRENT 3u
// Final stopped-driver snapshot retained outside SCM-owned state for reinstall.
#define AMDGPU_TRANSFER_ARCHIVE 4u
#define AMDGPU_BOARD_UNPROVEN 0u
#define AMDGPU_BOARD_FRESH 1u
#define AMDGPU_BOARD_LEGACY 2u
void AmdgpuStateBootToken(unsigned int boot_id,unsigned char token[16]);

typedef struct amdgpu_transfer_header {
    unsigned int magic,version,bytes,count;
    unsigned int origin,source_schema,board_kind,checksum;
    unsigned char transaction[16],source_generation[16],device_hash[32],source_boot[16];
    char catalog[64],source_version[32],destination_version[32];
    unsigned char board_record[88];
    unsigned int reserved[8];
} AMDGPU_TRANSFER_HEADER;
typedef struct amdgpu_transfer_entry {
    unsigned int index,present,type,bytes;
} AMDGPU_TRANSFER_ENTRY;

// Input metadata is copied; capture fills magic/version/bytes/count/checksum.
// One entry for every persistent catalog value, including explicit absence.
// Values which fail access/I/O must abort capture, not become absent.
unsigned int AmdgpuTransferCapture(const AMDGPU_TRANSFER_HEADER *metadata,
    AMDGPU_STATE_FETCH raw_fetch,void *context,void *buffer,unsigned int capacity,unsigned int *bytes);
unsigned int AmdgpuTransferCheck(const void *buffer,unsigned int bytes,AMDGPU_TRANSFER_HEADER *header);
// Final stopped-driver records replace the early records. Choices must still
// match the frozen snapshot byte for byte; a conflict requires a fresh capture.
// The output must not overlap the input. Never copy volatile boot markers.
unsigned int AmdgpuTransferMergeFinal(const void *snapshot,unsigned int snapshot_bytes,
    AMDGPU_STATE_FETCH stopped_fetch,void *context,void *out,unsigned int capacity,
    unsigned int *bytes,unsigned int *conflict_index);
unsigned int AmdgpuTransferFind(const void *buffer,unsigned int bytes,unsigned int index,
    AMDGPU_STATE_VALUE *value,const void **data);
// Detect stale transaction replay and a migration without the required restart.
unsigned int AmdgpuTransferBind(const AMDGPU_TRANSFER_HEADER *header,const unsigned char device_hash[32],
    const unsigned char transaction[16],const unsigned char current_boot[16],const char *destination_version);
enum amdgpu_transfer_operation {
    AMDGPU_TRANSFER_BEGIN=1,AMDGPU_TRANSFER_PUT,AMDGPU_TRANSFER_FLUSH_DATA,
    AMDGPU_TRANSFER_READY,AMDGPU_TRANSFER_FLUSH_READY,AMDGPU_TRANSFER_ACTIVATE,
    AMDGPU_TRANSFER_FLUSH_ACTIVE,AMDGPU_TRANSFER_RELEASE
};
// BEGIN opens only an unpublished generation bound to the transaction. PUT of an
// absent value removes a previous partial copy; no unknown children are imported.
// The adapter compares the expected active generation before ACTIVATE. RELEASE
// closes resources, including after BEGIN failed; it never deletes evidence.
typedef unsigned int (*AMDGPU_TRANSFER_STORE)(void *context,unsigned int operation,
    const AMDGPU_TRANSFER_HEADER *header,const AMDGPU_STATE_SPEC *spec,
    const AMDGPU_STATE_VALUE *value,const void *data);
unsigned int AmdgpuTransferApply(const void *buffer,unsigned int bytes,AMDGPU_TRANSFER_STORE store,void *context,
    unsigned int *invalid_groups,unsigned int *failed_index);
