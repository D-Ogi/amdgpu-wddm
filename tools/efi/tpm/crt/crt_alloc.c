/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The arena allocator of the stage C runtime (crt.h).
 *
 * The arena is a row of blocks. Each block starts with a 16-byte header: the size of the block with its header (a
 * multiple of 16, bit 0 set when the block is in use) and the size of the block before it (0 for the first block).
 * The part of the arena after the last block is the top; it grows when no free block fits and shrinks when the last
 * block is freed.
 *
 * Free blocks are in doubly linked lists, one list for each power of two of the block size (a bin). The link fields
 * are in the payload of the free block, so the smallest block is 32 bytes. malloc takes the first block of a fitting
 * size in the bin of the size, then the first block of a larger bin, then the top. free joins a block with free
 * neighbours. This is deterministic and needs no memory outside the arena.
 *
 * A header that does not make sense (a size outside the arena, a block that is free already) stops the call with
 * crt_fail: memory damage must not continue in silence.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "crt.h"
#include "crt_internal.h"

#define ALIGN 16u
#define HEADER 16u
#define MIN_BLOCK 32u
#define USED 1u
#define BINS 48

struct block {
	size_t size;		/* bytes of this block with the header, USED in bit 0 */
	size_t previous_size;	/* bytes of the block before, 0 for the first block */
	/* the payload starts here; a free block keeps its list links in it */
	struct block *next_free;
	struct block *previous_free;
};

static unsigned char *g_base;	/* first block */
static unsigned char *g_top;	/* the first byte after the last block */
static unsigned char *g_end;	/* the end of the arena */
static struct block *g_last;	/* the last block, or NULL */
static struct block *g_bins[BINS];
static int g_wiped;
static struct crt_arena_stats g_stats;

static size_t block_size(const struct block *b) { return b->size & ~(size_t)USED; }
static int block_used(const struct block *b) { return (b->size & USED) != 0; }

static unsigned bin_of(size_t size)
{
	unsigned bin = 0;
	while (size > MIN_BLOCK && bin < BINS - 1) {
		size >>= 1;
		bin++;
	}
	return bin;
}

static void list_remove(struct block *b)
{
	unsigned bin = bin_of(block_size(b));
	if (b->previous_free) b->previous_free->next_free = b->next_free;
	else g_bins[bin] = b->next_free;
	if (b->next_free) b->next_free->previous_free = b->previous_free;
}

static void list_insert(struct block *b)
{
	unsigned bin = bin_of(block_size(b));
	b->previous_free = NULL;
	b->next_free = g_bins[bin];
	if (g_bins[bin]) g_bins[bin]->previous_free = b;
	g_bins[bin] = b;
}

static struct block *next_block(const struct block *b)
{
	unsigned char *next = (unsigned char *)b + block_size(b);
	return next < g_top ? (struct block *)next : NULL;
}

static struct block *previous_block(const struct block *b)
{
	return b->previous_size ? (struct block *)((unsigned char *)b - b->previous_size) : NULL;
}

static void set_previous_size_of_next(struct block *b)
{
	struct block *next = next_block(b);
	if (next) next->previous_size = block_size(b);
}

/* Checks a block that a caller gives back. */
static struct block *checked_block(void *payload)
{
	unsigned char *p = payload;
	struct block *b;
	if ((uintptr_t)p % ALIGN != 0 || p < g_base + HEADER || p >= g_top) crt_fail("free or realloc of a pointer outside the arena");
	b = (struct block *)(p - HEADER);
	if (!block_used(b)) crt_fail("free or realloc of a block that is free");
	if (block_size(b) < MIN_BLOCK || block_size(b) % ALIGN != 0 || (unsigned char *)b + block_size(b) > g_top)
		crt_fail("damaged block header in the arena");
	return b;
}

static size_t request_size(size_t bytes)
{
	size_t size;
	if (bytes > (size_t)(g_end - g_base)) return 0;
	size = (bytes + HEADER + ALIGN - 1) & ~(size_t)(ALIGN - 1);
	return size < MIN_BLOCK ? MIN_BLOCK : size;
}

/* Gives the end of b back to the free lists when it is large enough to be a block. b is in use. */
static void split(struct block *b, size_t size)
{
	size_t rest = block_size(b) - size;
	struct block *tail;
	if (rest < MIN_BLOCK) return;
	b->size = size | USED;
	tail = (struct block *)((unsigned char *)b + size);
	tail->size = rest;
	tail->previous_size = size;
	if (g_last == b) g_last = tail;
	set_previous_size_of_next(tail);
	/* the tail can touch a free block after it */
	{
		struct block *next = next_block(tail);
		if (next && !block_used(next)) {
			list_remove(next);
			tail->size = rest + block_size(next);
			if (g_last == next) g_last = tail;
			set_previous_size_of_next(tail);
		}
	}
	if (g_last == tail && (unsigned char *)tail + block_size(tail) == g_top) {
		/* the last block is free: give it back to the top */
		g_top = (unsigned char *)tail;
		g_last = b;
		return;
	}
	list_insert(tail);
}

static void account_alloc(size_t size)
{
	g_stats.in_use += size;
	g_stats.live_blocks++;
	g_stats.allocations++;
	if (g_stats.in_use > g_stats.peak_in_use) g_stats.peak_in_use = g_stats.in_use;
	if ((size_t)(g_top - g_base) > g_stats.high_water) g_stats.high_water = (size_t)(g_top - g_base);
}

static struct block *take(size_t size)
{
	unsigned bin;
	struct block *b;

	for (bin = bin_of(size); bin < BINS; bin++) {
		for (b = g_bins[bin]; b; b = b->next_free) {
			if (block_size(b) >= size) {
				list_remove(b);
				b->size |= USED;
				split(b, size);
				return b;
			}
		}
	}
	if ((size_t)(g_end - g_top) < size) return NULL;
	b = (struct block *)g_top;
	b->size = size | USED;
	b->previous_size = g_last ? block_size(g_last) : 0;
	g_top += size;
	g_last = b;
	return b;
}

void *malloc(size_t bytes)
{
	size_t size;
	struct block *b;

	if (!g_base || g_wiped) return NULL;
	size = request_size(bytes);
	b = size ? take(size) : NULL;
	if (!b) {
		g_stats.failures++;
		errno_set_nomem();
		return NULL;
	}
	account_alloc(block_size(b));
	return (unsigned char *)b + HEADER;
}

void *calloc(size_t count, size_t bytes)
{
	void *p;
	if (bytes && count > (size_t)-1 / bytes) {
		g_stats.failures++;
		errno_set_nomem();
		return NULL;
	}
	p = malloc(count * bytes);
	if (p) memset(p, 0, count * bytes);
	return p;
}

void free(void *payload)
{
	struct block *b, *neighbour;

	if (!payload) return;
	if (!g_base || g_wiped) return;
	b = checked_block(payload);
	g_stats.in_use -= block_size(b);
	g_stats.live_blocks--;
	b->size &= ~(size_t)USED;

	neighbour = next_block(b);
	if (neighbour && !block_used(neighbour)) {
		list_remove(neighbour);
		if (g_last == neighbour) g_last = b;
		b->size += block_size(neighbour);
	}
	neighbour = previous_block(b);
	if (neighbour && !block_used(neighbour)) {
		list_remove(neighbour);
		if (g_last == b) g_last = neighbour;
		neighbour->size += block_size(b);
		b = neighbour;
	}
	if (g_last == b) {
		/* the last block: back to the top */
		g_top = (unsigned char *)b;
		g_last = previous_block(b);
		return;
	}
	set_previous_size_of_next(b);
	list_insert(b);
}

void *realloc(void *payload, size_t bytes)
{
	struct block *b, *next;
	size_t size, old;
	void *moved;

	if (!payload) return malloc(bytes);
	if (bytes == 0) {
		free(payload);
		return NULL;
	}
	if (!g_base || g_wiped) return NULL;
	b = checked_block(payload);
	size = request_size(bytes);
	if (!size) {
		g_stats.failures++;
		errno_set_nomem();
		return NULL;
	}
	old = block_size(b);
	if (size <= old) {
		split(b, size);
		g_stats.in_use -= old - block_size(b);
		g_stats.allocations++;
		return payload;
	}
	next = next_block(b);
	if (next && !block_used(next) && old + block_size(next) >= size) {
		list_remove(next);
		if (g_last == next) g_last = b;
		b->size = (old + block_size(next)) | USED;
		set_previous_size_of_next(b);
		split(b, size);
		g_stats.in_use += block_size(b) - old;
		g_stats.live_blocks--;
		account_alloc(0);
		return payload;
	}
	if (g_last == b && (size_t)(g_end - (unsigned char *)b) >= size) {
		g_top = (unsigned char *)b + size;
		b->size = size | USED;
		g_stats.in_use += size - old;
		g_stats.live_blocks--;
		account_alloc(0);
		return payload;
	}
	moved = malloc(bytes);
	if (!moved) return NULL;
	memcpy(moved, payload, old - HEADER);
	free(payload);
	return moved;
}

int crt_alloc_init(void *arena, size_t bytes)
{
	uintptr_t start = ((uintptr_t)arena + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
	uintptr_t end = ((uintptr_t)arena + bytes) & ~(uintptr_t)(ALIGN - 1);

	if (!arena || bytes < CRT_ARENA_MIN || end <= start || end - start < CRT_ARENA_MIN) return 0;
	g_base = (unsigned char *)start;
	g_top = g_base;
	g_end = (unsigned char *)end;
	g_last = NULL;
	memset(g_bins, 0, sizeof(g_bins));
	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.size = (size_t)(g_end - g_base);
	g_wiped = 0;
	return 1;
}

void crt_arena_stats(struct crt_arena_stats *out)
{
	*out = g_stats;
}

void crt_arena_wipe(void)
{
	if (!g_base) return;
	/* volatile: the compiler must not drop a store to memory that nobody reads after it */
	{
		volatile unsigned char *p = g_base;
		size_t n = (size_t)(g_end - g_base);
		while (n--) *p++ = 0;
	}
	g_wiped = 1;
	g_top = g_base;
	g_last = NULL;
	memset(g_bins, 0, sizeof(g_bins));
	g_stats.in_use = 0;
	g_stats.live_blocks = 0;
}
