/* test_descriptor.c - the 64-byte kernel descriptor reader.
 *
 * Section 5.4 of docs/design/m16-hip-route-b.md: this test must catch a field read from
 * the wrong byte, a reserved field that is accepted non-zero, a KERNEL_CODE_PROPERTIES
 * bit this build does not program that is accepted, and a wrong USER_SGPR extraction.
 *
 * It reads the three real descriptors out of the committed code object at their
 * measured file offsets, and then changes one field at a time in a buffer of its own.
 * The descriptor layout is the one of section 3.5:
 *
 *   0   group_segment_fixed_size      44  compute_pgm_rsrc3
 *   4   private_segment_fixed_size    48  compute_pgm_rsrc1
 *   8   kernarg_size                  52  compute_pgm_rsrc2
 *   12  reserved (4 bytes)            56  kernel_code_properties (2 bytes)
 *   16  kernel_code_entry_byte_offset 58  kernarg_preload (2 bytes)
 *   24  reserved (20 bytes)           60  reserved (4 bytes)
 */
#include "test_common.h"

#include "internal.h"

/* The committed object loads .rodata at virtual address 0 with file offset 0, so the
 * descriptor file offsets are the symbol addresses of section 3.5. */
#define VADD_KD_AT      0x0C80u
#define REDUCE_KD_AT    0x0CC0u
#define WRITEGRID_KD_AT 0x0D00u

typedef struct expected_descriptor {
    const char* name;
    uint32_t    file_offset;
    uint32_t    group_segment;
    uint32_t    kernarg_size;
    uint64_t    entry_offset;
} expected_descriptor;

static const expected_descriptor g_expected[3] = {
    { "vadd",          VADD_KD_AT,      0u,    28u,  0x1180u },
    { "reduce256",     REDUCE_KD_AT,    1024u, 16u,  0x1240u },
    { "writeGridSize", WRITEGRID_KD_AT, 0u,    264u, 0x1400u }
};

static void check_real_descriptors(const char* dir)
{
    void*    image;
    size_t   image_bytes = 0;
    uint32_t i;

    image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
    if (image == NULL) {
        return;
    }
    for (i = 0; i < 3u; i++) {
        const expected_descriptor* e = &g_expected[i];
        bc250hsa_kernel_internal   k;

        memset(&k, 0, sizeof(k));
        /* The metadata values the loader has already read. The reader cross-checks
         * them against the descriptor's own copies. */
        k.pub.name = e->name;
        k.pub.kernarg_bytes = e->kernarg_size;
        k.pub.group_segment_bytes = e->group_segment;
        k.pub.private_segment_bytes = 0u;

        CHECK(e->file_offset + 64u <= image_bytes);
        CHECK_STATUS(bc250hsa_descriptor_read((const uint8_t*)image + e->file_offset, 64u, &k),
                     BC250HSA_OK);
        CHECK_U64(k.descriptor.group_segment_fixed_size, e->group_segment);
        CHECK_U64(k.descriptor.private_segment_fixed_size, 0u);
        CHECK_U64(k.descriptor.kernarg_size, e->kernarg_size);
        CHECK_U64(k.descriptor.kernel_code_entry_byte_offset, e->entry_offset);
        CHECK_U64(k.descriptor.kernarg_preload, 0u);
        CHECK_U64(k.pub.compute_pgm_rsrc1, 0xE0AF0000u);
        CHECK_U64(k.pub.compute_pgm_rsrc2, 0x0000008Cu);
        CHECK_U64(k.pub.compute_pgm_rsrc3, 0u);
        /* PRIVATE_SEGMENT_BUFFER, KERNARG_SEGMENT_PTR and WAVEFRONT_SIZE32. */
        CHECK_U64(k.pub.kernel_code_properties, 0x0409u);
        CHECK_U64(k.pub.kernel_code_properties & BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER,
                  BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER);
        CHECK_U64(k.pub.kernel_code_properties & BC250HSA_KCP_KERNARG_SEGMENT_PTR,
                  BC250HSA_KCP_KERNARG_SEGMENT_PTR);
        CHECK_U64(k.pub.kernel_code_properties & BC250HSA_KCP_WAVEFRONT_SIZE32,
                  BC250HSA_KCP_WAVEFRONT_SIZE32);
        /* Four registers for the buffer and two for the pointer. */
        CHECK_U64(k.pub.user_sgpr_count, 6u);
        CHECK_U64(k.pub.uses_dynamic_stack, 0u);
        /* Decision 3 of section 2: the descriptor's LDS_SIZE is 0 even for the
         * kernel with 1024 bytes of local memory, so the host must compute it. */
        CHECK_U64((k.pub.compute_pgm_rsrc2 >> BC250HSA_RSRC2_LDS_SIZE_SHIFT) &
                      BC250HSA_RSRC2_LDS_SIZE_MASK,
                  0u);
        CHECK_U64(bc250hsa_lds_size_field(e->group_segment, 0u),
                  (e->group_segment == 1024u) ? 2u : 0u);
    }
    free(image);
}

/* A descriptor of known numbers, so that one field can change at a time. */
static void fill_descriptor(uint8_t* d)
{
    memset(d, 0, 64);
    d[0] = 0x00; d[1] = 0x04;                 /* group_segment_fixed_size 1024 */
    d[8] = 28u;                               /* kernarg_size 28 */
    d[16] = 0x80; d[17] = 0x11;               /* entry byte offset 0x1180 */
    d[48] = 0x00; d[49] = 0x00; d[50] = 0xAF; d[51] = 0xE0;  /* rsrc1 0xE0AF0000 */
    d[52] = 0x8C;                             /* rsrc2 0x8C */
    d[56] = 0x09; d[57] = 0x04;               /* properties 0x0409 */
}

static void prepare_kernel(bc250hsa_kernel_internal* k)
{
    memset(k, 0, sizeof(*k));
    k->pub.name = "synthetic";
    k->pub.kernarg_bytes = 28u;
    k->pub.group_segment_bytes = 1024u;
    k->pub.private_segment_bytes = 0u;
}

static void check_field_rules(void)
{
    uint8_t                  d[64];
    bc250hsa_kernel_internal k;
    uint32_t                 i;

    /* The baseline parses, and every field lands where section 3.5 says. */
    fill_descriptor(d);
    prepare_kernel(&k);
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_OK);
    CHECK_U64(k.descriptor.group_segment_fixed_size, 1024u);
    CHECK_U64(k.descriptor.kernarg_size, 28u);
    CHECK_U64(k.descriptor.kernel_code_entry_byte_offset, 0x1180u);
    CHECK_U64(k.pub.compute_pgm_rsrc1, 0xE0AF0000u);
    CHECK_U64(k.pub.compute_pgm_rsrc2, 0x8Cu);
    CHECK_U64(k.pub.kernel_code_properties, 0x0409u);
    CHECK_U64(k.pub.user_sgpr_count, 6u);

    /* A descriptor shorter than 64 bytes is not a descriptor. */
    CHECK_STATUS(bc250hsa_descriptor_read(d, 63u, &k), BC250HSA_EBADELF);
    CHECK_STATUS(bc250hsa_descriptor_read(d, 0u, &k), BC250HSA_EBADELF);

    /* Every reserved byte must be zero: a non-zero one means a shape this build does
     * not know, and running it would program registers from unknown bytes. */
    for (i = 12u; i < 16u; i++) {
        fill_descriptor(d);
        prepare_kernel(&k);
        d[i] = 1u;
        CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);
    }
    for (i = 24u; i < 44u; i++) {
        fill_descriptor(d);
        prepare_kernel(&k);
        d[i] = 0xFFu;
        CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);
    }
    for (i = 60u; i < 64u; i++) {
        fill_descriptor(d);
        prepare_kernel(&k);
        d[i] = 0x80u;
        CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);
    }

    /* Bytes 44 to 47 are COMPUTE_PGM_RSRC3 and not reserved: a non-zero value is
     * copied through, because gfx10 uses the register. */
    fill_descriptor(d);
    prepare_kernel(&k);
    d[44] = 0x05;
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_OK);
    CHECK_U64(k.pub.compute_pgm_rsrc3, 5u);

    /* Preloaded kernel arguments are a code object v6 feature this build does not
     * program. It is refused by name, not ignored. */
    fill_descriptor(d);
    prepare_kernel(&k);
    d[58] = 2u;
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EUNSUPPORTED);

    /* A KERNEL_CODE_PROPERTIES bit this build does not know. */
    fill_descriptor(d);
    prepare_kernel(&k);
    d[57] = 0x84u;   /* bit 15 set */
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EUNSUPPORTED);

    /* USES_DYNAMIC_STACK is known, and it reaches the kernel as a refusal reason
     * for the dispatch. */
    fill_descriptor(d);
    prepare_kernel(&k);
    d[57] = 0x0Cu;   /* 0x0C09: adds USES_DYNAMIC_STACK */
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_OK);
    CHECK_U64(k.pub.kernel_code_properties, 0x0C09u);
    CHECK_U64(k.pub.uses_dynamic_stack, 1u);

    /* The descriptor and the metadata must agree. They disagree only in a broken
     * object, so the reader refuses instead of choosing one of the two. */
    fill_descriptor(d);
    prepare_kernel(&k);
    k.pub.kernarg_bytes = 32u;
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);
    fill_descriptor(d);
    prepare_kernel(&k);
    k.pub.group_segment_bytes = 512u;
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);
    fill_descriptor(d);
    prepare_kernel(&k);
    k.pub.private_segment_bytes = 64u;
    CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_EBADELF);

    /* USER_SGPR is bits 5:1 of COMPUTE_PGM_RSRC2, which is the one field of the
     * descriptor that the user SGPR plan must match exactly. */
    {
        static const struct { uint8_t rsrc2_low; uint8_t expected; } sgpr_cases[] = {
            { 0x00u, 0u }, { 0x02u, 1u }, { 0x08u, 4u }, { 0x0Cu, 6u },
            { 0x8Cu, 6u }, { 0x3Eu, 31u }, { 0x01u, 0u }
        };
        for (i = 0; i < TEST_COUNT(sgpr_cases); i++) {
            fill_descriptor(d);
            prepare_kernel(&k);
            d[52] = sgpr_cases[i].rsrc2_low;
            CHECK_STATUS(bc250hsa_descriptor_read(d, 64u, &k), BC250HSA_OK);
            CHECK_U64(k.pub.user_sgpr_count, sgpr_cases[i].expected);
        }
    }
}

int main(int argc, char** argv)
{
    const char* dir = test_data_dir(argc, argv);

    check_real_descriptors(dir);
    check_field_rules();
    return test_report("test_descriptor");
}
