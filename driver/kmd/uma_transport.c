// Fixed BC-250 extended-CMOS ports, not the RTC bank used by the x64 HAL.
// PROVENANCE: fanoush/bc250_memcfg, MIT, 222420fb: ports and ABL block indices.
#include <ntddk.h>
#include "uma_transport.h"
#include <string.h>

#define BC250_ABL_CMOS_INDEX_PORT 0x72u
#define BC250_ABL_CMOS_DATA_PORT 0x73u
#define BC250_ABL_CMOS_FIRST_INDEX 0x90u

static int valid_mapping(const struct bc250_uma_transport* transport)
{
    return transport && transport->mapping == BC250_UMA_MAPPING_EXTENDED_PORT;
}
static ULONG transfer(struct bc250_uma_transport* transport, unsigned int offset,
                       void* buffer, ULONG bytes, int write)
{
    ULONG i;
    unsigned char* data = buffer;
    if (!valid_mapping(transport) || !buffer || offset >= BC250_UMA_BLOCK_BYTES ||
        bytes == 0 || bytes > BC250_UMA_BLOCK_BYTES - offset ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) return 0;
    /* Every production caller holds board_memory_write.c's platform-wide lock
       for the full transaction, including each index/data pair. This pair is
       separate from the HAL's RTC ports 0x70/0x71; no RTC/NMI bit is changed.
       Port I/O has no success status. Stability and write read-back are checked
       above this adapter. Neither the lock nor read-back excludes SMM access. */
    for (i = 0; i < bytes; ++i) {
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)BC250_ABL_CMOS_INDEX_PORT,
                         (unsigned char)(BC250_ABL_CMOS_FIRST_INDEX + offset + i));
        if (write)
            WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)BC250_ABL_CMOS_DATA_PORT, data[i]);
        else
            data[i] = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)BC250_ABL_CMOS_DATA_PORT);
    }
    return bytes;
}
static int read_byte(void* context, unsigned int offset, unsigned char* value)
{
    unsigned char temporary = 0;
    if (!value) return 0;
    *value = 0;
    if (transfer(context, offset, &temporary, 1, 0) != 1) return 0;
    *value = temporary;
    return 1;
}
static int write_byte(void* context, unsigned int offset, unsigned char value)
{
    /* Defense in depth: timing bytes are never writable through this adapter. */
    if (offset >= 6 && offset != 26 && offset != 27) return 0;
    return transfer(context, offset, &value, 1, 1) == 1;
}
void bc250_uma_transport_io(struct bc250_uma_transport* transport, struct bc250_uma_io* io)
{
    if (!io) return;
    io->context = transport;
    io->read = read_byte;
    io->write = write_byte;
}
int bc250_uma_transport_read_block(struct bc250_uma_transport* transport, unsigned char* block)
{
    unsigned char first[BC250_UMA_BLOCK_BYTES], second[BC250_UMA_BLOCK_BYTES];
    unsigned char byte_view[BC250_UMA_BLOCK_BYTES];
    struct bc250_uma_io io;
    int status;
    if (!block) return BC250_UMA_INVALID;
    memset(block, 0, BC250_UMA_BLOCK_BYTES);
    if (!valid_mapping(transport)) return BC250_UMA_INVALID;
    if (transfer(transport, 0, first, sizeof(first), 0) != sizeof(first) ||
        transfer(transport, 0, second, sizeof(second), 0) != sizeof(second))
        return BC250_UMA_READ_FAILED;
    if (memcmp(first, second, sizeof(first))) return BC250_UMA_UNSTABLE;
    bc250_uma_transport_io(transport, &io);
    status = bc250_uma_read(&io, byte_view);
    if (status != BC250_UMA_OK) return status;
    if (memcmp(first, byte_view, sizeof(first))) return BC250_UMA_UNSTABLE;
    memcpy(block, first, sizeof(first));
    return BC250_UMA_OK;
}
