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
   Payload bytes are read and written only when different; signature invalidation
   and final commit remain mandatory; byte 0 must read back zero before payload writes. Rollback never publishes a valid signature
   after an earlier failed read or write. */
int bc250_uma_validate(const unsigned char block[BC250_UMA_BLOCK_BYTES]);
int bc250_uma_read(const struct bc250_uma_io* io,
                   unsigned char block[BC250_UMA_BLOCK_BYTES]);
int bc250_uma_apply(const struct bc250_uma_io* io,
                    const unsigned char expected[BC250_UMA_BLOCK_BYTES],
                    unsigned int target_mib, struct bc250_uma_result* result);

/* Restore the exact original signature/checksum/UMA from a durable backup.
   Backup UMA is restricted to 8192/12288; timing bytes 6..25 must agree.
   Caller authenticates and retrieves the backup; never pass a client substitute. */
int bc250_uma_restore(const struct bc250_uma_io* io,
                      const unsigned char expected[BC250_UMA_BLOCK_BYTES],
                      const unsigned char backup[BC250_UMA_BLOCK_BYTES],
                      struct bc250_uma_result* result);
