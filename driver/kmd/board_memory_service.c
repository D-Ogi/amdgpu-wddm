#include "board_memory_service.h"
#include <string.h>

static unsigned size_mib(const unsigned char* b) { return b[26] | ((unsigned)b[27] << 8); }
static int allowed(const unsigned char* b)
{ return bc250_uma_validate(b) && (size_mib(b) == 8192 || size_mib(b) == 12288); }
static int stored(const struct board_memory_store* s, const unsigned char* backup, unsigned pending)
{
    unsigned char check[28]; unsigned state = 2;
    return s->save(s->context, backup, pending) && s->load(s->context, check, &state) == 1 &&
           state == pending && !memcmp(check, backup, 28);
}
void BoardMemoryServiceStart(struct board_memory_state* state, const struct bc250_uma_io* io,
                             const struct board_memory_store* store)
{
    unsigned pending = 0; int loaded;
    memset(state, 0, sizeof(*state));
    loaded = store->load(store->context, state->backup, &pending);
    if (loaded < 0 || (loaded && (!allowed(state->backup) || pending != 0))) {
        state->blocked = 1; return;
    }
    if (bc250_uma_read(io, state->block) != BC250_UMA_OK || !allowed(state->block)) return;
    if (loaded && memcmp(state->block + 6, state->backup + 6, 20)) { state->blocked = 1; return; }
    state->backup_valid = loaded == 1;
    state->ready = 1;
}
int BoardMemoryServiceChange(struct board_memory_state* state, const struct bc250_uma_io* io,
                             const struct board_memory_store* store, const unsigned char expected[28],
                             unsigned target, int restore)
{
    struct bc250_uma_result result;
    unsigned char current[28], saved[28]; unsigned pending = 0;
    int loaded, status;
    if (!state->ready || state->blocked || !allowed(expected) ||
        (restore ? target != 0 : (target != 8192 && target != 12288))) return BC250_UMA_INVALID;
    status = bc250_uma_read(io, current);
    if (status != BC250_UMA_OK) return status;
    if (memcmp(current, expected, 28) || memcmp(current, state->block, 28)) return BC250_UMA_STALE;
    loaded = store->load(store->context, saved, &pending);
    if (loaded < 0 || pending || (loaded && (!allowed(saved) || memcmp(saved + 6, current + 6, 20))) ||
        (state->backup_valid && (loaded != 1 || memcmp(saved, state->backup, 28)))) {
        state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED;
    }
    if (restore && !loaded) return BC250_UMA_INVALID;
    if ((!restore && size_mib(current) == target) || (restore && !memcmp(current, saved, 28))) {
        state->result = BC250_UMA_NO_CHANGE; return BC250_UMA_NO_CHANGE;
    }
    if (!loaded) {
        memcpy(saved, current, 28);
        if (!stored(store, saved, 0)) { state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED; }
    }
    memcpy(state->backup, saved, 28); state->backup_valid = 1;
    // Durable pending marker precedes the first hardware write. A crash leaves a lockout.
    if (!stored(store, saved, 1)) { state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED; }
    status = restore ? bc250_uma_restore(io, expected, saved, &result) :
                       bc250_uma_apply(io, expected, target, &result);
    state->result = status;
    if ((status != BC250_UMA_OK && status != BC250_UMA_NO_CHANGE && status != BC250_UMA_RESTORED) ||
        bc250_uma_read(io, current) != BC250_UMA_OK || !allowed(current) ||
        memcmp(current + 6, saved + 6, 20)) {
        state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED;
    }
    // Do not remove the pending marker without verifying the intended result once more.
    if ((status == BC250_UMA_RESTORED && memcmp(current, expected, 28)) ||
        (status >= 0 && (restore ? memcmp(current, saved, 28) != 0 : size_mib(current) != target))) {
        state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED;
    }
    memcpy(state->block, current, 28);
    if (!stored(store, saved, 0)) { state->blocked = 1; return BC250_UMA_ROLLBACK_UNCONFIRMED; }
    return status;
}
