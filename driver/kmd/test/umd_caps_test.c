// The bytes baked into the driver must be the filler's honest blob, not a second transcription.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../umd_caps.h"
#include "../../contract/bc250_umd_private.h"
#include "bc250_caps_test.h"

static int g_failures;

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

int main(void)
{
    uint32_t size = 0;
    const unsigned char* live = (const unsigned char*)bc250_unitA_blob(BC250_MUTATE_NONE, &size);
    const struct bc250_umd_private* baked = (const struct bc250_umd_private*)umd_caps_blob;

    CHECK(size == UMD_CAPS_BYTES);
    CHECK(sizeof(struct bc250_umd_firmware)==64);
    CHECK(offsetof(struct bc250_umd_private,firmware)==UMD_CAPS_FIRMWARE_OFFSET);
    CHECK(sizeof(struct bc250_umd_private) == UMD_CAPS_BYTES);
    CHECK(live != NULL);
    CHECK(memcmp(live, umd_caps_blob, UMD_CAPS_BYTES) == 0);
    CHECK(baked->magic == BC250_UMD_PRIVATE_MAGIC);
    CHECK(baked->version == BC250_UMD_PRIVATE_VERSION);
    CHECK(baked->size == UMD_CAPS_BYTES);
    CHECK(baked->submittable_node_mask == 0x1u);
    CHECK(offsetof(struct bc250_umd_private, submittable_node_mask) == BC250_UMD_PRIVATE_SIZE_V2);

    // The CU fields cumode.c patches at QueryAdapterInfo, and the stock the template holds there: what
    // bc250_cu_info_from_wgps() yields for WGPs 0-2 of every shader array (cu_mode_test.c checks that side).
    CHECK(offsetof(struct bc250_umd_private, device.cu_active_number) == UMD_CAPS_CU_ACTIVE_OFFSET);
    CHECK(offsetof(struct bc250_umd_private, device.cu_ao_mask) == UMD_CAPS_CU_AO_MASK_OFFSET);
    CHECK(offsetof(struct bc250_umd_private, device.cu_bitmap) == UMD_CAPS_CU_BITMAP_OFFSET);
    CHECK(offsetof(struct bc250_umd_private, device.cu_ao_bitmap) == UMD_CAPS_CU_AO_BITMAP_OFFSET);
    CHECK(sizeof(baked->device.cu_bitmap) == 64 && sizeof(baked->device.cu_ao_bitmap) == 64);
    CHECK(baked->device.cu_active_number == 24u);
    CHECK(baked->device.cu_ao_mask == 0x3F3F3F3Fu);
    {
        unsigned i, j;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++) {
                CHECK(baked->device.cu_bitmap[i][j] == (i < 2 && j < 2 ? 0x3Fu : 0u));
                CHECK(baked->device.cu_ao_bitmap[i][j] == (i < 2 && j < 2 ? 0x3Fu : 0u));
            }
    }

    if (g_failures == 0) { printf("umd_caps_test: all cases passed\n"); return 0; }
    fprintf(stderr, "umd_caps_test: %d case(s) failed\n", g_failures);
    return 1;
}
