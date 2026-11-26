/* SPDX-License-Identifier: MIT */
#ifndef BC250_DMA_PROVIDER_C096_H
#define BC250_DMA_PROVIDER_C096_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define BC250_DMA_ABI 1
#define BC250_DMA_BYTES 4096
struct bc250_dma_request {
    __u32 abi;
    __u32 slot;
    __u32 reserved[2];
};
struct bc250_dma_descriptor {
    struct bc250_dma_request request;
    __u64 dma_address;
    __u32 bytes;
    __u32 canary;
};
struct bc250_dma_snapshot {
    struct bc250_dma_request request;
    __u8 bytes[BC250_DMA_BYTES];
};
/* FILL is once per slot, before any address exposure. No refill/free/reset. */
#define BC250_DMA_FILL _IOW('V', 0x60, struct bc250_dma_request)
#define BC250_DMA_EXPOSE _IOWR('V', 0x61, struct bc250_dma_descriptor)
#define BC250_DMA_SNAPSHOT _IOWR('V', 0x62, struct bc250_dma_snapshot)
#endif
