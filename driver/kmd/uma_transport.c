// Fixed-address HAL transport. No raw ports and no implicit mapping selection.
#include <ntddk.h>
#include "uma_transport.h"
#include <string.h>

static int valid_mapping(const struct bc250_uma_transport* transport)
{
    return transport && (transport->mapping == BC250_UMA_MAPPING_SLOT ||
                         transport->mapping == BC250_UMA_MAPPING_OFFSET);
}
static ULONG transfer(struct bc250_uma_transport* transport, unsigned int offset,
                       void* buffer, ULONG bytes, int write)
{
    ULONG slot, position, count;
    if (!valid_mapping(transport) || !buffer || offset >= BC250_UMA_BLOCK_BYTES ||
        bytes == 0 || bytes > BC250_UMA_BLOCK_BYTES - offset ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) return 0;
    slot = transport->mapping == BC250_UMA_MAPPING_SLOT ? 0x90u + offset : 0;
    position = transport->mapping == BC250_UMA_MAPPING_OFFSET ? 0x90u + offset : 0;
    /* WDK 26100 documents Cmos as the exception to these deprecated bus APIs.
       Exact extended-CMOS mapping is qualified separately by the caller. */
#pragma warning(push)
#pragma warning(disable:4996)
    count = write ? HalSetBusDataByOffset(Cmos, 1, slot, buffer, position, bytes) :
                    HalGetBusDataByOffset(Cmos, 1, slot, buffer, position, bytes);
#pragma warning(pop)
    return count;
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
