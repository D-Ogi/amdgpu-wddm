// SPDX-License-Identifier: MIT
#pragma once
#define BC250_UMA_BLOCK_BYTES 28u
/* Offsets are relative to the fixed board block; never a user-supplied port. */
struct bc250_uma_io {
    void* context;
    int (*read)(void* context, unsigned int offset, unsigned char* value);
    int (*write)(void* context, unsigned int offset, unsigned char value);
};
enum bc250_uma_status {
    BC250_UMA_OK = 0,
    BC250_UMA_NO_CHANGE = 1,
    BC250_UMA_INVALID = -1,
    BC250_UMA_READ_FAILED = -2,
    BC250_UMA_UNSTABLE = -3,
    BC250_UMA_STALE = -4,
    BC250_UMA_RESTORED = -5,
    BC250_UMA_ROLLBACK_UNCONFIRMED = -6
};
struct bc250_uma_result {
    unsigned int writes_attempted;
    unsigned int rollback_writes_attempted;
    int operation_failed; /* write or verification failed, even when restored */
};
/* Caller serializes the whole read/apply operation against every other writer.
   Callbacks return nonzero on success. A failed write may still have changed a byte.
   Stable means two equal full reads, not protection against unrelated firmware.
   The caller durably backs up expected before apply. Power loss is not atomic.
   Current UMA must be 256..14320 MiB, aligned to 16 MiB; requested targets are
   restricted to 8192/12288. Failed signature invalidation stops all data writes.
   Rollback never publishes a valid signature after an earlier failed write. */
int bc250_uma_validate(const unsigned char block[BC250_UMA_BLOCK_BYTES]);
int bc250_uma_read(const struct bc250_uma_io* io,
                   unsigned char block[BC250_UMA_BLOCK_BYTES]);
int bc250_uma_apply(const struct bc250_uma_io* io,
                    const unsigned char expected[BC250_UMA_BLOCK_BYTES],
                    unsigned int target_mib, struct bc250_uma_result* result);
