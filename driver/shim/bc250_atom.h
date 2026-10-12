/* Checked ATOM execution, independent of kernel APIs. The KMD owns serialization and rundown. */
#ifndef BC250_ATOM_H
#define BC250_ATOM_H
#include "bc250_atom_preflight.h"

typedef struct BC250_ATOM_CALLBACKS {
    void *Context;
    int (*Owned)(void *context); /* PASSIVE owner and power/rundown state still valid. */
    int (*Allowed)(void *context, uint32_t dword, int write); /* Generated per-ASIC policy. */
    int (*Read)(void *context, uint32_t dword, uint32_t *value);
    int (*Write)(void *context, uint32_t dword, uint32_t value);
    uint64_t (*NowUs)(void *context); /* Monotonic, must advance independently of interpreter activity. */
    int (*DelayUs)(void *context, uint32_t microseconds); /* Can sleep; never under an ISR spin lock. */
} BC250_ATOM_CALLBACKS;

typedef struct BC250_ATOM_LIMITS {
    uint32_t Instructions, DelayUs, ElapsedUs;
} BC250_ATOM_LIMITS;

typedef struct BC250_ATOM_RESULT {
    BC250_ATOM_STATUS Status;
    uint32_t Table, Offset, Register, Writes, Reads, Instructions, DelayUs;
    uint64_t ElapsedUs;
} BC250_ATOM_RESULT;

/* Context is a caller-owned pool/heap block of Bc250AtomContextSize() bytes.
 * image and plan must stay immutable through the call. Parameters are copied
 * into a private, zero-filled 64-DWORD frame; the call tree sets its required size.
 * A failure after a write requires the caller's complete modeset rollback.
 * Limits apply to the whole nested execution. No field is a per-table budget. */
size_t Bc250AtomContextSize(void);
BC250_ATOM_STATUS Bc250AtomExecute(void *workspace, size_t workspaceSize,
    const BC250_ATOM_IMAGE *image, unsigned root, const BC250_ATOM_PLAN *plan,
    const BC250_ATOM_CALLBACKS *io, const BC250_ATOM_LIMITS *limits,
    void *parameters, size_t parameterBytes, BC250_ATOM_RESULT *result);

#define BC250_ATOM_BATCH_COMMANDS 16u
typedef struct BC250_ATOM_COMMAND {
    unsigned Root;
    void *Parameters;
    size_t ParameterBytes;
} BC250_ATOM_COMMAND;

/* Validate every command and its call tree before any register callback. The same
 * instruction, delay and elapsed budgets cover all entries and both validation
 * passes. plan is caller-owned pool/heap scratch, reused after the first pass.
 * The caller holds the display owner and keeps image and commands immutable.
 * Completed commands may have changed hardware and parameters when a later one
 * fails; this is not rollback. The modeset owner must then restore the full mode. */
BC250_ATOM_STATUS Bc250AtomExecuteBatch(void *workspace, size_t workspaceSize,
    const BC250_ATOM_IMAGE *image, BC250_ATOM_PLAN *plan,
    const BC250_ATOM_CALLBACKS *io, const BC250_ATOM_LIMITS *limits,
    const BC250_ATOM_COMMAND *commands, unsigned count, BC250_ATOM_RESULT *result);
#endif
