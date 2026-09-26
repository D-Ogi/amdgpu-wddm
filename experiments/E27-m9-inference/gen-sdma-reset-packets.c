// PROVENANCE: AMD navi10_sdma_pkt_open.h and libdrm deadlock_tests.c, MIT.
#include <stdio.h>
#include "navi10_sdma_pkt_open.h"
int main(void)
{
 printf("{\"poll_header\":%u,\"poll_interval_retry\":%u,\"write_header\":%u,\"nop\":%u}\n",
  (unsigned)(SDMA_PKT_POLL_REGMEM_HEADER_OP(SDMA_OP_POLL_REGMEM) |
   SDMA_PKT_POLL_REGMEM_HEADER_FUNC(4) | SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1)),
  (unsigned)(SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4) | SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff)),
  (unsigned)(SDMA_PKT_WRITE_UNTILED_HEADER_OP(SDMA_OP_WRITE) |
   SDMA_PKT_WRITE_UNTILED_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR)),
  (unsigned)SDMA_OP_NOP);
 return 0;
}
