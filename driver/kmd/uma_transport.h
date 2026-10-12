// Fixed BC-250 ABL block transport; this is not a provider authorization gate.
#pragma once
#include "bc250_uma.h"
enum bc250_uma_mapping {
    BC250_UMA_MAPPING_NONE = 0,
    BC250_UMA_MAPPING_SLOT = 1,
    BC250_UMA_MAPPING_OFFSET = 2,
    BC250_UMA_MAPPING_EXTENDED_PORT = 3
};
struct bc250_uma_transport { unsigned int mapping; };
/* Caller must select a positively matched board provider and a mapping from the
   runtime self-check, remain at PASSIVE_LEVEL, and serialize the entire transaction.
   Only EXTENDED_PORT is usable; legacy HAL mappings are retained as identifiers
   so their stored backups cannot be confused with the new transport. The caller's
   lock covers all driver users of ports 0x72/0x73, not firmware or other drivers.
   NONE/legacy/unknown mappings issue no port I/O. No arbitrary address is exposed. */
void bc250_uma_transport_io(struct bc250_uma_transport* transport, struct bc250_uma_io* io);
/* Two exact whole-block reads plus byte-access equivalence. On failure output
   is zeroed. Stability is sampled, not exclusion of firmware or power loss. */
int bc250_uma_transport_read_block(struct bc250_uma_transport* transport,
                                  unsigned char block[BC250_UMA_BLOCK_BYTES]);
