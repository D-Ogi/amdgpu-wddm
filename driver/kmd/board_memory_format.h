// Fixed little-endian records shared with the SYSTEM migration worker.
// A checksum detects corruption, never authority. The opened file ACL is checked.
#pragma once
// Include after the platform's base types (WDK or Windows SDK). The wire types
// below have the same widths in the x86 and x64 builds; no CRT is needed in KMD.

#define BOARD_STORE_MAGIC 0x314d4241u
#define BOARD_STORE_VERSION 1u
typedef struct BOARD_STORE_RECORD {
    unsigned int Magic, Version, Bytes, Mapping, BiosId;
    unsigned char MachineId[16], Backup[28];
    unsigned int Pending, Checksum, Reserved[4];
} BOARD_STORE_RECORD;

#define BOARD_JOURNAL_MAGIC 0x324a4241u
#define BOARD_JOURNAL_VERSION 1u
#define BOARD_JOURNAL_EMPTY 0u
#define BOARD_JOURNAL_CLEAR 1u
#define BOARD_JOURNAL_PENDING 2u
typedef struct BOARD_STORE_JOURNAL {
    unsigned int Magic, Version, Bytes, OriginalChecksum;
    unsigned long long Sequence;
    unsigned int State, Mapping, BiosId;
    unsigned char MachineId[16];
    unsigned int Checksum, Reserved[2];
} BOARD_STORE_JOURNAL;

static __inline unsigned int BoardRecordChecksum(const void *record, size_t bytes, size_t checksumOffset)
{
    const unsigned char *data = (const unsigned char *)record;
    unsigned int value = 2166136261u;
    size_t i;
    for (i = 0; i < bytes; ++i)
        value = (value ^ ((i >= checksumOffset && i < checksumOffset + 4) ? 0 : data[i])) * 16777619u;
    return value;
}
static __inline unsigned int StoreChecksum(const BOARD_STORE_RECORD *record)
{ return BoardRecordChecksum(record, sizeof(*record), FIELD_OFFSET(BOARD_STORE_RECORD, Checksum)); }
static __inline unsigned int JournalChecksum(const BOARD_STORE_JOURNAL *record)
{ return BoardRecordChecksum(record, sizeof(*record), FIELD_OFFSET(BOARD_STORE_JOURNAL, Checksum)); }
