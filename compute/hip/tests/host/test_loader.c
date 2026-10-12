/* test_loader.c - the code object loader, against the committed code object and against
 * a synthetic object built in this file.
 *
 * Section 5.4 of docs/design/m16-hip-route-b.md states what this test must catch: a
 * segment that is not copied, a .bss that is not zeroed, a relocation type that is
 * accepted wrongly, a wrong descriptor address (the measured 0xC80 and 0x1E00) and a
 * wrong ABI version check.
 *
 * The committed object answers the first, second, fourth and fifth of those. It holds
 * no relocation, because clang resolved every address at the device link step, so the
 * relocation rules are tested on a synthetic object: a 0x1600-byte code object with one
 * PT_LOAD, one metadata note that declares no kernel, one dynamic symbol and one
 * relocation section whose single record this test rewrites per case.
 */
#include "test_common.h"

#include "co_internal.h"
#include "internal.h"

/* The measured numbers of tests/data/m16_kernels.gfx1013.co. PROVENANCE.txt beside it
 * names the compiler revision and the command that produced it. */
#define IMAGE_SPAN      0x5000u
#define VADD_KD         0x0C80u
#define VADD_ENTRY      0x1E00u
#define REDUCE_KD       0x0CC0u
#define REDUCE_ENTRY    0x1F00u
#define WRITEGRID_KD    0x0D00u
#define WRITEGRID_ENTRY 0x2100u
#define CUID_VADDR      0x4270u /* the .bss byte, the highest p_vaddr of the object */
#define BSS_HOLE        0x3400u /* inside p_memsz of the third segment, past p_filesz */
#define OBJECT_BYTES    7128u

typedef struct expected_kernel {
    const char* name;
    uint32_t    descriptor_offset;
    uint32_t    entry_offset;
    uint32_t    kernarg_bytes;
    uint32_t    group_segment_bytes;
    uint16_t    sgpr_count;
    uint16_t    vgpr_count;
    uint32_t    arg_count;
    uint32_t    explicit_args;
    uint32_t    hidden_args;
} expected_kernel;

static const expected_kernel g_expected[3] = {
    { "vadd",          VADD_KD,      VADD_ENTRY,      28u, 0u,    9u, 8u,  4u, 4u, 0u },
    { "reduce256",     REDUCE_KD,    REDUCE_ENTRY,    16u, 1024u, 10u, 4u, 2u, 2u, 0u },
    { "writeGridSize", WRITEGRID_KD, WRITEGRID_ENTRY, 264u, 0u,   9u, 4u, 14u, 1u, 13u }
};

static void check_committed_object(const char* dir)
{
    bc250hsa_allocator      alloc;
    struct bc250hsa_module* mod = NULL;
    void*                   image;
    size_t                  image_bytes = 0;
    uint64_t                range_va = 0;
    uint64_t                range_bytes = 0;
    uint64_t                symbol_va = 0;
    uint64_t                symbol_bytes = 0;
    const uint8_t*          host;
    uint32_t                i;

    image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
    if (image == NULL) {
        return;
    }
    CHECK_U64(image_bytes, OBJECT_BYTES);
    test_allocator(&alloc);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod), BC250HSA_OK);
    if (mod == NULL) {
        free(image);
        return;
    }

    /* One range that covers every PT_LOAD, 4096-aligned. */
    CHECK_STATUS(bc250hsa_module_range(mod, &range_va, &range_bytes), BC250HSA_OK);
    CHECK_U64(range_bytes, IMAGE_SPAN);
    CHECK_U64(range_va % 4096u, 0u);

    CHECK_U64(bc250hsa_module_kernel_count(mod), 3u);
    for (i = 0; i < 3u; i++) {
        const expected_kernel* e = &g_expected[i];
        const bc250hsa_kernel* k = bc250hsa_module_kernel_at(mod, i);

        /* The metadata order is the order of the kernels. */
        CHECK(k != NULL);
        if (k == NULL) {
            continue;
        }
        CHECK(strcmp(k->name, e->name) == 0);
        CHECK(bc250hsa_module_kernel_by_name(mod, e->name) == k);

        CHECK_U64(k->descriptor_va, range_va + e->descriptor_offset);
        CHECK_U64(k->entry_va, range_va + e->entry_offset);
        CHECK_U64(k->descriptor_va % 64u, 0u);
        CHECK_U64(k->entry_va % 256u, 0u);

        CHECK_U64(k->kernarg_bytes, e->kernarg_bytes);
        /* Decision 4 of section 2: the metadata says 8, the floor is 16. */
        CHECK_U64(k->kernarg_align, 16u);
        CHECK_U64(k->group_segment_bytes, e->group_segment_bytes);
        CHECK_U64(k->private_segment_bytes, 0u);
        CHECK_U64(k->max_flat_workgroup_size, 1024u);
        CHECK_U64(k->sgpr_count, e->sgpr_count);
        CHECK_U64(k->vgpr_count, e->vgpr_count);
        CHECK_U64(k->wave_size, 32u);
        CHECK_U64(k->workgroup_processor_mode, 1u);
        CHECK_U64(k->uses_dynamic_stack, 0u);

        /* The descriptor of all three kernels of this object, as section 3.5
         * measured it. USER_SGPR is 6: a private segment buffer and a kernel
         * argument pointer. LDS_SIZE is 0 even for reduce256, which has 1024 bytes
         * of local memory; that is why decision 3 computes the field in the host. */
        CHECK_U64(k->compute_pgm_rsrc1, 0xE0AF0000u);
        CHECK_U64(k->compute_pgm_rsrc2, 0x0000008Cu);
        CHECK_U64(k->compute_pgm_rsrc3, 0u);
        CHECK_U64(k->kernel_code_properties, 0x0409u);
        CHECK_U64(k->user_sgpr_count, 6u);
        CHECK_U64((k->compute_pgm_rsrc2 >> 15) & 0x1FFu, 0u);

        CHECK_U64(k->arg_count, e->arg_count);
        CHECK_U64(k->explicit_arg_count, e->explicit_args);
        CHECK_U64(k->hidden_arg_count, e->hidden_args);
        CHECK(k->args != NULL);
    }
    CHECK(bc250hsa_module_kernel_by_name(mod, "vadd2") == NULL);
    CHECK(bc250hsa_module_kernel_at(mod, 3u) == NULL);

    /* A device symbol, which is what __hipRegisterVar needs. The kernel function
     * symbol is the one whose address this test knows. */
    CHECK_STATUS(bc250hsa_module_symbol(mod, "vadd", &symbol_va, &symbol_bytes), BC250HSA_OK);
    CHECK_U64(symbol_va, range_va + VADD_ENTRY);
    CHECK_U64(symbol_bytes, 136u);
    CHECK_STATUS(bc250hsa_module_symbol(mod, "reduce256", &symbol_va, &symbol_bytes),
                 BC250HSA_OK);
    CHECK_U64(symbol_va, range_va + REDUCE_ENTRY);
    CHECK_U64(symbol_bytes, 512u);
    CHECK_STATUS(bc250hsa_module_symbol(mod, "no_such_symbol", &symbol_va, &symbol_bytes),
                 BC250HSA_ENOTFOUND);

    /* The copy and the zero fill. The entry point must hold instructions, the hole
     * between p_filesz and p_memsz of the third segment must be zero, and so must
     * everything past the highest p_vaddr up to the aligned end. */
    host = (const uint8_t*)((uintptr_t)range_va);
    {
        uint32_t nonzero = 0;
        uint32_t at;
        for (at = 0; at < 64u; at++) {
            if (host[VADD_ENTRY + at] != 0u) {
                nonzero++;
            }
        }
        CHECK(nonzero > 8u);   /* the first instructions of vadd were copied */
        for (at = 0; at < 256u; at++) {
            CHECK_U64(host[BSS_HOLE + at], 0u);
        }
        for (at = CUID_VADDR; at < IMAGE_SPAN; at++) {
            CHECK_U64(host[at], 0u);
        }
    }

    bc250hsa_module_unload(mod);
    free(image);
}

/* A copy of the committed object with one byte changed, so that each header rule is
 * tested against a real object and not against a hand-made one. */
static void check_header_rules(const char* dir)
{
    void*  image;
    size_t image_bytes = 0;
    uint32_t i;

    static const struct {
        const char*     what;
        uint32_t        at;         /* the byte to change */
        uint8_t         value;
        bc250hsa_status expected;
    } cases[] = {
        { "e_ident[EI_CLASS] 1 is 32-bit",   BC250HSA_EI_CLASS, 1u, BC250HSA_EBADELF },
        { "e_ident[EI_DATA] 2 is big endian", BC250HSA_EI_DATA, 2u, BC250HSA_EBADELF },
        { "e_ident[EI_OSABI] 0 is not AMDGPU HSA", BC250HSA_EI_OSABI, 0u, BC250HSA_EBADELF },
        { "e_machine 0x3E is x86-64",        18u, 0x3Eu, BC250HSA_EBADELF },
        { "EI_ABIVERSION 2 is code object v4", BC250HSA_EI_ABIVERSION, 2u,
          BC250HSA_EBADABIVERSION },
        { "EI_ABIVERSION 5 is a version this build does not know",
          BC250HSA_EI_ABIVERSION, 5u, BC250HSA_EBADABIVERSION },
        { "e_flags mach 0x36 is gfx1030",    48u, 0x36u, BC250HSA_EWRONGTARGET }
    };

    for (i = 0; i < TEST_COUNT(cases); i++) {
        bc250hsa_allocator      alloc;
        struct bc250hsa_module* mod = NULL;

        image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
        if (image == NULL) {
            return;
        }
        ((uint8_t*)image)[cases[i].at] = cases[i].value;
        test_allocator(&alloc);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod),
                     cases[i].expected);
        CHECK(mod == NULL);
        free(image);
    }

    /* gfx10-1-generic is admitted as well: one binary for every gfx10.1 part. */
    image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
    if (image != NULL) {
        bc250hsa_allocator      alloc;
        struct bc250hsa_module* mod = NULL;
        ((uint8_t*)image)[48] = (uint8_t)BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX10_1_GENERIC;
        test_allocator(&alloc);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod), BC250HSA_OK);
        bc250hsa_module_unload(mod);
        free(image);
    }

    /* A truncated object, and the null parameters. */
    image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
    if (image != NULL) {
        bc250hsa_allocator      alloc;
        struct bc250hsa_module* mod = NULL;
        test_allocator(&alloc);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, 16u, &mod), BC250HSA_EBADELF);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, NULL, image_bytes, &mod),
                     BC250HSA_EINVAL);
        CHECK_STATUS(bc250hsa_module_load_alloc(NULL, image, image_bytes, &mod),
                     BC250HSA_EINVAL);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, NULL),
                     BC250HSA_EINVAL);
        free(image);
    }

    /* The accessors never fault on a null module. */
    CHECK_U64(bc250hsa_module_kernel_count(NULL), 0u);
    CHECK(bc250hsa_module_kernel_at(NULL, 0u) == NULL);
    CHECK(bc250hsa_module_kernel_by_name(NULL, "vadd") == NULL);
    bc250hsa_module_unload(NULL);
    {
        uint64_t a = 0, b = 0;
        CHECK_STATUS(bc250hsa_module_range(NULL, &a, &b), BC250HSA_EINVAL);
        CHECK_STATUS(bc250hsa_module_symbol(NULL, "vadd", &a, &b), BC250HSA_EINVAL);
    }
}

/* --------------------------------------------------------------------------------
 * The synthetic object, for the relocation rules
 * ------------------------------------------------------------------------------ */

#define SYN_BYTES     0x1600u
#define SYN_LOAD_OFF  0x1000u
#define SYN_LOAD_VA   0x1000u
#define SYN_FILESZ    0x0100u
#define SYN_MEMSZ     0x0200u
#define SYN_NOTE_OFF  0x1200u
#define SYN_SYM_OFF   0x1300u
#define SYN_STR_OFF   0x1400u
#define SYN_RELA_OFF  0x1500u
#define SYN_PATCH_VA  0x1080u   /* where the relocation writes */
#define SYN_SPAN      0x1000u

static void put16(uint8_t* b, uint32_t at, uint16_t v) { memcpy(b + at, &v, 2); }
static void put32(uint8_t* b, uint32_t at, uint32_t v) { memcpy(b + at, &v, 4); }
static void put64(uint8_t* b, uint32_t at, uint64_t v) { memcpy(b + at, &v, 8); }

/* The metadata note of a code object with no kernel: a one-pair map whose only key is
 * "amdhsa.kernels" and whose value is an empty array. */
static uint32_t synthetic_note(uint8_t* out)
{
    static const char kKey[] = "amdhsa.kernels";
    const uint32_t    key_bytes = (uint32_t)(sizeof(kKey) - 1u);
    uint32_t          at = 0;

    out[at++] = 0x81u;                            /* fixmap, one pair */
    out[at++] = (uint8_t)(0xA0u | key_bytes);     /* fixstr */
    memcpy(out + at, kKey, key_bytes);
    at += key_bytes;
    out[at++] = 0x90u;                            /* fixarray, no element */
    return at;
}

static void build_synthetic(uint8_t* b, uint32_t rela_count)
{
    static const char kOwner[] = "AMDGPU";
    uint8_t           note[64];
    const uint32_t    note_bytes = synthetic_note(note);
    const uint32_t    note_area = 12u + 8u + note_bytes; /* namesz 7 padded to 8 */
    const uint32_t    phoff = 64u;
    const uint32_t    shoff = 64u + 2u * 56u;

    memset(b, 0, SYN_BYTES);
    memcpy(b, "\177ELF", 4);
    b[BC250HSA_EI_CLASS] = BC250HSA_ELFCLASS64;
    b[BC250HSA_EI_DATA] = BC250HSA_ELFDATA2LSB;
    b[6] = 1u; /* EI_VERSION */
    b[BC250HSA_EI_OSABI] = BC250HSA_ELFOSABI_AMDGPU_HSA;
    b[BC250HSA_EI_ABIVERSION] = BC250HSA_ELFABIVERSION_AMDGPU_HSA_V5;
    put16(b, 16u, 3u);                                  /* e_type ET_DYN */
    put16(b, 18u, BC250HSA_EM_AMDGPU);
    put32(b, 20u, 1u);                                  /* e_version */
    put64(b, 32u, phoff);                               /* e_phoff */
    put64(b, 40u, shoff);                               /* e_shoff */
    put32(b, 48u, BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX1013);
    put16(b, 52u, 64u);                                 /* e_ehsize */
    put16(b, 54u, 56u);                                 /* e_phentsize */
    put16(b, 56u, 2u);                                  /* e_phnum */
    put16(b, 58u, 64u);                                 /* e_shentsize */
    put16(b, 60u, 5u);                                  /* e_shnum */

    /* PT_LOAD: executable, p_filesz below p_memsz so the zero fill is exercised. */
    put32(b, phoff + 0u, BC250HSA_PT_LOAD);
    put32(b, phoff + 4u, 5u);                           /* R and X */
    put64(b, phoff + 8u, SYN_LOAD_OFF);
    put64(b, phoff + 16u, SYN_LOAD_VA);
    put64(b, phoff + 32u, SYN_FILESZ);
    put64(b, phoff + 40u, SYN_MEMSZ);
    put64(b, phoff + 48u, 4096u);
    /* PT_NOTE */
    put32(b, phoff + 56u + 0u, BC250HSA_PT_NOTE);
    put32(b, phoff + 56u + 4u, 4u);                     /* R */
    put64(b, phoff + 56u + 8u, SYN_NOTE_OFF);
    put64(b, phoff + 56u + 32u, note_area);

    /* Section 1: the note. */
    put32(b, shoff + 64u + 4u, BC250HSA_SHT_NOTE);
    put64(b, shoff + 64u + 24u, SYN_NOTE_OFF);
    put64(b, shoff + 64u + 32u, note_area);
    /* Section 2: .dynsym, two entries, linked to section 3. */
    put32(b, shoff + 128u + 4u, BC250HSA_SHT_DYNSYM);
    put64(b, shoff + 128u + 24u, SYN_SYM_OFF);
    put64(b, shoff + 128u + 32u, 2u * 24u);
    put32(b, shoff + 128u + 40u, 3u);                   /* sh_link */
    put64(b, shoff + 128u + 56u, 24u);                  /* sh_entsize */
    /* Section 3: .dynstr. */
    put32(b, shoff + 192u + 4u, BC250HSA_SHT_STRTAB);
    put64(b, shoff + 192u + 24u, SYN_STR_OFF);
    put64(b, shoff + 192u + 32u, 3u);
    /* Section 4: .rela, rela_count records. */
    put32(b, shoff + 256u + 4u, BC250HSA_SHT_RELA);
    put64(b, shoff + 256u + 24u, SYN_RELA_OFF);
    put64(b, shoff + 256u + 32u, (uint64_t)rela_count * 24u);
    put64(b, shoff + 256u + 56u, 24u);

    /* The loadable bytes: a pattern, so that a missing copy is visible. */
    memset(b + SYN_LOAD_OFF, 0xA5, SYN_FILESZ);

    /* The note. */
    put32(b, SYN_NOTE_OFF + 0u, (uint32_t)(sizeof(kOwner)));   /* namesz, with the NUL */
    put32(b, SYN_NOTE_OFF + 4u, note_bytes);
    put32(b, SYN_NOTE_OFF + 8u, BC250HSA_NT_AMDGPU_METADATA);
    memcpy(b + SYN_NOTE_OFF + 12u, kOwner, sizeof(kOwner));
    memcpy(b + SYN_NOTE_OFF + 20u, note, note_bytes);

    /* .dynsym entry 1: the global "g" at the start of the load segment. */
    put32(b, SYN_SYM_OFF + 24u + 0u, 1u);               /* st_name */
    b[SYN_SYM_OFF + 24u + 4u] = 0x11u;                  /* GLOBAL OBJECT */
    put16(b, SYN_SYM_OFF + 24u + 6u, 1u);               /* st_shndx */
    put64(b, SYN_SYM_OFF + 24u + 8u, SYN_LOAD_VA);
    put64(b, SYN_SYM_OFF + 24u + 16u, 8u);
    /* .dynstr */
    b[SYN_STR_OFF + 0u] = '\0';
    b[SYN_STR_OFF + 1u] = 'g';
    b[SYN_STR_OFF + 2u] = '\0';
}

static void set_rela(uint8_t* b, uint32_t index, uint64_t offset, uint32_t sym,
                     uint32_t type, int64_t addend)
{
    const uint32_t at = SYN_RELA_OFF + index * 24u;
    put64(b, at + 0u, offset);
    put64(b, at + 8u, ((uint64_t)sym << 32) | (uint64_t)type);
    put64(b, at + 16u, (uint64_t)addend);
}

static void check_relocations(void)
{
    uint8_t*                b = (uint8_t*)malloc(SYN_BYTES);
    bc250hsa_allocator      alloc;
    struct bc250hsa_module* mod = NULL;
    uint64_t                va = 0;
    uint64_t                bytes = 0;

    if (b == NULL) {
        g_failures++;
        return;
    }
    test_allocator(&alloc);

    /* The one relocation type a device-linked object holds: B + A. */
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_PATCH_VA, 0u, BC250HSA_R_AMDGPU_RELATIVE64, 0x40);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_OK);
    if (mod != NULL) {
        uint64_t patched = 0;
        CHECK_STATUS(bc250hsa_module_range(mod, &va, &bytes), BC250HSA_OK);
        CHECK_U64(bytes, SYN_SPAN);
        CHECK_U64(bc250hsa_module_kernel_count(mod), 0u);
        memcpy(&patched, (const uint8_t*)((uintptr_t)va) + (SYN_PATCH_VA - SYN_LOAD_VA), 8);
        /* B is the difference between the loaded address and the ELF address. */
        CHECK_U64(patched, va - SYN_LOAD_VA + 0x40u);
        /* p_filesz was copied and the rest of p_memsz was zeroed. */
        CHECK_U64(((const uint8_t*)((uintptr_t)va))[0], 0xA5u);
        CHECK_U64(((const uint8_t*)((uintptr_t)va))[SYN_FILESZ], 0u);
        CHECK_U64(((const uint8_t*)((uintptr_t)va))[SYN_MEMSZ - 1u], 0u);
        /* The dynamic symbol is found through the copied tables. */
        CHECK_STATUS(bc250hsa_module_symbol(mod, "g", &va, &bytes), BC250HSA_OK);
        CHECK_U64(bytes, 8u);
        bc250hsa_module_unload(mod);
        mod = NULL;
    }

    /* Any other relocation type is refused. R_AMDGPU_ABS64 is 3 and R_AMDGPU_REL64
     * is 4; neither may be applied as B + A. */
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_PATCH_VA, 0u, 3u, 0x40);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADRELOC);
    CHECK(mod == NULL);
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_PATCH_VA, 0u, BC250HSA_R_AMDGPU_RELATIVE64 + 1u, 0x40);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADRELOC);

    /* A record that names a symbol is an unresolved external of a -fgpu-rdc build
     * with no device link step. It is refused, never resolved to zero. */
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_PATCH_VA, 1u, BC250HSA_R_AMDGPU_RELATIVE64, 0x40);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADRELOC);

    /* A target outside the loaded range, below and above. */
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_LOAD_VA - 8u, 0u, BC250HSA_R_AMDGPU_RELATIVE64, 0);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADRELOC);
    build_synthetic(b, 1u);
    set_rela(b, 0u, SYN_LOAD_VA + SYN_SPAN - 4u, 0u, BC250HSA_R_AMDGPU_RELATIVE64, 0);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADRELOC);

    /* Several records in one section are all applied. */
    build_synthetic(b, 2u);
    set_rela(b, 0u, SYN_PATCH_VA, 0u, BC250HSA_R_AMDGPU_RELATIVE64, 0x10);
    set_rela(b, 1u, SYN_PATCH_VA + 8u, 0u, BC250HSA_R_AMDGPU_RELATIVE64, 0x20);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_OK);
    if (mod != NULL) {
        uint64_t first = 0;
        uint64_t second = 0;
        CHECK_STATUS(bc250hsa_module_range(mod, &va, &bytes), BC250HSA_OK);
        memcpy(&first, (const uint8_t*)((uintptr_t)va) + (SYN_PATCH_VA - SYN_LOAD_VA), 8);
        memcpy(&second, (const uint8_t*)((uintptr_t)va) + (SYN_PATCH_VA - SYN_LOAD_VA) + 8u, 8);
        CHECK_U64(first, va - SYN_LOAD_VA + 0x10u);
        CHECK_U64(second, va - SYN_LOAD_VA + 0x20u);
        bc250hsa_module_unload(mod);
        mod = NULL;
    }

    /* No metadata note at all. */
    build_synthetic(b, 0u);
    put32(b, 64u + 2u * 56u + 64u + 4u, 0u);  /* the note section becomes SHT_NULL */
    put32(b, 64u + 56u + 0u, 0u);             /* and PT_NOTE becomes PT_NULL */
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod),
                 BC250HSA_EBADMETADATA);

    /* No .dynsym, which a loaded object always has. */
    build_synthetic(b, 0u);
    put32(b, 64u + 2u * 56u + 128u + 4u, 0u);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADELF);

    /* No PT_LOAD: nothing to run. */
    build_synthetic(b, 0u);
    put32(b, 64u + 0u, 0u);
    CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, b, SYN_BYTES, &mod), BC250HSA_EBADELF);

    free(b);
}


/* BD-110: a sibling's 141-byte descriptor symbol rejected a whole real module.
 * Test the production metadata parser, including partial-failure teardown. */
static size_t metadata_string(uint8_t* b, size_t at, const char* text, size_t n)
{
    b[at++] = 0xDAu; /* str16, enough to exercise names beyond any old fixed cap */
    b[at++] = (uint8_t)(n >> 8);
    b[at++] = (uint8_t)n;
    memcpy(b + at, text, n);
    return at + n;
}

static void check_metadata_strings(void)
{
    static const size_t lengths[] = { 127u, 128u, 145u, 512u, 4096u };
    uint8_t b[8192];
    char symbol[4096];
    uint32_t i;
    memset(symbol, 's', sizeof(symbol));
    for (i = 0; i < TEST_COUNT(lengths); i++) {
        struct bc250hsa_module* mod = (struct bc250hsa_module*)calloc(1, sizeof(*mod));
        size_t at = 0;
        CHECK(mod != NULL);
        if (mod == NULL) { return; }
        b[at++] = 0x81u;
        at = metadata_string(b, at, "amdhsa.kernels", 14u);
        b[at++] = 0x91u;
        b[at++] = 0x82u;
        at = metadata_string(b, at, ".name", 5u);
        at = metadata_string(b, at, "test", 4u);
        at = metadata_string(b, at, ".symbol", 7u);
        at = metadata_string(b, at, symbol, lengths[i]);
        const bc250hsa_status status = bc250hsa_metadata_parse(b, at, mod);
        CHECK_STATUS(status, BC250HSA_OK);
        if (status == BC250HSA_OK) {
            CHECK_U64(strlen(mod->kernels[0].symbol), lengths[i]);
            CHECK(memcmp(mod->kernels[0].symbol, symbol, lengths[i]) == 0);
        }
        bc250hsa_module_unload(mod);
    }
    /* Both identifying strings reject duplicate keys, embedded NUL, non-string
     * values and a declared str32 length that does not fit the note. */
    for (i = 0; i < 10u; i++) {
        struct bc250hsa_module* mod = (struct bc250hsa_module*)calloc(1, sizeof(*mod));
        const char* key = (i & 1u) ? ".name" : ".symbol";
        const size_t key_bytes = strlen(key);
        const uint32_t mode = i / 2u;
        size_t at = 0;
        CHECK(mod != NULL);
        if (mod == NULL) { return; }
        b[at++] = 0x81u;
        at = metadata_string(b, at, "amdhsa.kernels", 14u);
        b[at++] = 0x91u;
        b[at++] = mode == 0u ? 0x83u : 0x82u;
        at = metadata_string(b, at, (i & 1u) ? ".symbol" : ".name", (i & 1u) ? 7u : 5u);
        at = metadata_string(b, at, "good", 4u);
        at = metadata_string(b, at, key, key_bytes);
        if (mode == 0u) {
            at = metadata_string(b, at, "first", 5u);
            at = metadata_string(b, at, key, key_bytes);
            at = metadata_string(b, at, "second", 6u);
        } else if (mode == 1u) {
            at = metadata_string(b, at, "prefix\0suffix", 13u);
        } else if (mode == 2u) {
            b[at++] = 0x01u; /* integer is not a name */
        } else if (mode == 3u) {
            b[at++] = 0xDBu;
            memset(b + at, 0xFF, 4u); at += 4u;
        } else {
            at = metadata_string(b, at, "", 0u);
        }
        CHECK_STATUS(bc250hsa_metadata_parse(b, at, mod), BC250HSA_EBADMETADATA);
        bc250hsa_module_unload(mod);
    }
    /* Unknown container sizes must not wrap children=count*2 to zero. */
    {
        struct bc250hsa_module* mod = (struct bc250hsa_module*)calloc(1, sizeof(*mod));
        size_t at = 0;
        CHECK(mod != NULL);
        if (mod == NULL) { return; }
        b[at++] = 0x81u;
        at = metadata_string(b, at, "amdhsa.kernels", 14u);
        b[at++] = 0x91u; b[at++] = 0x83u;
        at = metadata_string(b, at, ".name", 5u);
        at = metadata_string(b, at, "test", 4u);
        at = metadata_string(b, at, ".symbol", 7u);
        at = metadata_string(b, at, "test.kd", 7u);
        at = metadata_string(b, at, ".future", 7u);
        b[at++] = 0xDFu; b[at++] = 0x80u;
        b[at++] = 0u; b[at++] = 0u; b[at++] = 0u;
        CHECK_STATUS(bc250hsa_metadata_parse(b, at, mod), BC250HSA_EBADMETADATA);
        bc250hsa_module_unload(mod);
    }
}


/* Note offsets belong to the ELF image, not its host allocation address or its
 * containing PE/bundle. The same note also remains valid through PT_NOTE. */
static void check_note_locations(const char* dir)
{
    static const size_t offsets[] = { 0u, 1u, 7u, 4096u };
    size_t bytes = 0;
    uint8_t* original = (uint8_t*)test_read_file(dir, "m16_kernels.gfx1013.co", &bytes);
    uint32_t pass, i;
    if (original == NULL) { return; }
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < TEST_COUNT(offsets); i++) {
            uint8_t* raw = (uint8_t*)malloc(bytes + offsets[i]);
            struct bc250hsa_module* mod = NULL;
            bc250hsa_allocator alloc;
            uint8_t* image;
            CHECK(raw != NULL);
            if (raw == NULL) { free(original); return; }
            image = raw + offsets[i];
            memcpy(image, original, bytes);
            if (pass != 0u) {
                bc250hsa_elf64_ehdr eh;
                uint32_t j, changed = 0;
                memcpy(&eh, image, sizeof(eh));
                for (j = 0; j < eh.e_shnum; j++) {
                    bc250hsa_elf64_shdr sh;
                    uint8_t* entry = image + eh.e_shoff + (uint64_t)j * eh.e_shentsize;
                    memcpy(&sh, entry, sizeof(sh));
                    if (sh.sh_type == BC250HSA_SHT_NOTE) {
                        sh.sh_type = 0u;
                        memcpy(entry, &sh, sizeof(sh));
                        changed++;
                    }
                }
                CHECK(changed != 0u);
            }
            test_allocator(&alloc);
            CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, bytes, &mod), BC250HSA_OK);
            CHECK_U64(bc250hsa_module_kernel_count(mod), 3u);
            bc250hsa_module_unload(mod);
            free(raw);
        }
    }
    free(original);
}

int main(int argc, char** argv)
{
    const char* dir = test_data_dir(argc, argv);

    check_committed_object(dir);
    check_header_rules(dir);
    check_relocations();
    check_metadata_strings();
    check_note_locations(dir);
    return test_report("test_loader");
}
