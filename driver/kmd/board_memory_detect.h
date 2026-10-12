#pragma once
#include "uma_transport.h"
/* PASSIVE_LEVEL, serialized by the provider. Reads only. Clears outputs on failure.
   Only the fixed extended-CMOS port route is admitted. Requested sleep <= 1.5 seconds; HAL call
   duration and scheduler latency are outside this software iteration bound. */
int BoardMemoryDetect(struct bc250_uma_transport* transport,
                      unsigned char block[BC250_UMA_BLOCK_BYTES]);
