/* co_loader.c - the clang offload bundle reader and the ELF code object loader.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.4. Our own loader, not
 * amdhsaloader: the seven steps of the design and nothing else. It needs no global
 * offset table, no procedure linkage table, no lazy binding, no symbol versioning,
 * no DT_NEEDED and no initialiser array, because a linked gfx1013 code object has
 * none of them (measured: the dynamic table of the spike's object holds SYMTAB,
 * SYMENT, STRTAB, STRSZ, GNU_HASH, HASH and NULL, plus RELA when there are
 * relocations).
 *
 * The loader runs against bc250hsa_allocator, so tests/host/test_loader.c loads the
 * committed code objects on a machine with no BC-250 adapter.
 */
#include <stdlib.h>

#include "co_internal.h"
#include "internal.h"

/* --------------------------------------------------------------------------
 * The offload bundle
 * ------------------------------------------------------------------------ */

static int read_u64(const uint8_t* base, size_t base_bytes, size_t offset, uint64_t* out)
{
    if (offset + 8u > base_bytes) {
        return 0;
    }
    memcpy(out, base + offset, 8);
    return 1;
}

/* The target identifier inside a bundle entry name. The name is
 * "<kind>-<triple>-<target id>", the triple is "amdgcn-amd-amdhsa-" with an empty
 * environment, and a target id may carry feature suffixes after a colon. A reader
 * cannot split on '-', because "gfx10-1-generic" holds two of them. */
static int bundle_target(const char* id, uint32_t id_bytes, const char** target,
                         uint32_t* target_bytes)
{
    static const char kTriple[] = "amdgcn-amd-amdhsa";
    const uint32_t    triple_bytes = (uint32_t)(sizeof(kTriple) - 1u);
    uint32_t          i;

    if (id_bytes <= triple_bytes) {
        return 0;
    }
    for (i = 0; i + triple_bytes <= id_bytes; i++) {
        if (memcmp(id + i, kTriple, triple_bytes) == 0) {
            uint32_t at = i + triple_bytes;
            uint32_t end;
            while (at < id_bytes && id[at] == '-') {
                at++;
            }
            if (at >= id_bytes) {
                return 0;
            }
            end = at;
            while (end < id_bytes && id[end] != ':') {
                end++;
            }
            *target = id + at;
            *target_bytes = end - at;
            return 1;
        }
    }
    return 0;
}

static int id_starts_with_hip(const char* id, uint32_t id_bytes)
{
    static const char kHip[] = "hip-";
    static const char kHipV4[] = "hipv4-";
    if (id_bytes >= 4u && memcmp(id, kHip, 4) == 0) {
        return 1;
    }
    if (id_bytes >= 6u && memcmp(id, kHipV4, 6) == 0) {
        return 1;
    }
    return 0;
}

static int text_is(const char* text, uint32_t text_bytes, const char* wanted)
{
    const size_t wanted_bytes = strlen(wanted);
    return wanted_bytes == (size_t)text_bytes && memcmp(text, wanted, wanted_bytes) == 0;
}

bc250hsa_status bc250hsa_unbundle(const void* fatbin, size_t fatbin_bytes,
                                  const char* target_id, const void** image,
                                  size_t* image_bytes)
{
    const uint8_t* base = (const uint8_t*)fatbin;
    uint64_t       entry_count = 0;
    size_t         at;
    uint64_t       index;
    const uint8_t* best = NULL;
    uint64_t       best_bytes = 0;
    int            best_is_exact = 0;

    if (fatbin == NULL || image == NULL || image_bytes == NULL) {
        return BC250HSA_EINVAL;
    }
    if (target_id == NULL) {
        target_id = "gfx1013";
    }
    *image = NULL;
    *image_bytes = 0;

    if (fatbin_bytes >= 4u && memcmp(base, BC250HSA_BUNDLE_COMPRESSED_MAGIC, 4) == 0) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "compressed offload bundle (CCOB); rebuild without --offload-compress");
        return BC250HSA_ECOMPRESSEDBUNDLE;
    }
    if (fatbin_bytes < BC250HSA_BUNDLE_MAGIC_BYTES + 8u ||
        memcmp(base, BC250HSA_BUNDLE_MAGIC, BC250HSA_BUNDLE_MAGIC_BYTES) != 0) {
        return BC250HSA_EBADBUNDLE;
    }
    if (!read_u64(base, fatbin_bytes, BC250HSA_BUNDLE_MAGIC_BYTES, &entry_count)) {
        return BC250HSA_EBADBUNDLE;
    }
    /* One entry costs 24 header bytes at least, so a count that cannot fit is a
     * malformed header rather than a very large bundle. */
    if (entry_count > fatbin_bytes / 24u) {
        return BC250HSA_EBADBUNDLE;
    }

    at = BC250HSA_BUNDLE_MAGIC_BYTES + 8u;
    for (index = 0; index < entry_count; index++) {
        uint64_t    offset = 0;
        uint64_t    size = 0;
        uint64_t    id_bytes = 0;
        const char* id;
        const char* target;
        uint32_t    target_bytes;
        int         exact;

        if (!read_u64(base, fatbin_bytes, at + 0u, &offset) ||
            !read_u64(base, fatbin_bytes, at + 8u, &size) ||
            !read_u64(base, fatbin_bytes, at + 16u, &id_bytes)) {
            return BC250HSA_EBADBUNDLE;
        }
        at += 24u;
        if (id_bytes > 0xFFFFu || at + id_bytes > fatbin_bytes) {
            return BC250HSA_EBADBUNDLE;
        }
        id = (const char*)(base + at);
        at += (size_t)id_bytes;

        /* Trap 1 of the design: the host entry has size 0. Trap 2: it has the same
         * offset as the device entry, so nothing may key an entry by its offset.
         * Trap 3: its identifier names a fixed placeholder host triple even on a
         * Windows compile, so nothing may match on the host triple. */
        if (size == 0u) {
            continue;
        }
        if (offset > fatbin_bytes || size > fatbin_bytes - offset) {
            return BC250HSA_EBADBUNDLE;
        }
        if (!id_starts_with_hip(id, (uint32_t)id_bytes)) {
            continue;
        }
        if (!bundle_target(id, (uint32_t)id_bytes, &target, &target_bytes)) {
            continue;
        }
        exact = text_is(target, target_bytes, target_id);
        if (!exact && !text_is(target, target_bytes, "gfx10-1-generic")) {
            continue;
        }
        if (size < 4u || memcmp(base + offset, "\177ELF", 4) != 0) {
            return BC250HSA_EBADBUNDLE;
        }
        if (best == NULL || (exact && !best_is_exact)) {
            best = base + offset;
            best_bytes = size;
            best_is_exact = exact;
        }
    }
    if (best == NULL) {
        return BC250HSA_ENOTFOUND;
    }
    *image = best;
    *image_bytes = (size_t)best_bytes;
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The ELF code object
 * ------------------------------------------------------------------------ */

typedef struct elf_view {
    const uint8_t*             bytes;
    size_t                     byte_count;
    const bc250hsa_elf64_ehdr* eh;
} elf_view;

static int in_file(const elf_view* v, uint64_t offset, uint64_t length)
{
    return offset <= v->byte_count && length <= (uint64_t)v->byte_count - offset;
}

static const bc250hsa_elf64_phdr* phdr_at(const elf_view* v, uint32_t index)
{
    const uint64_t offset = v->eh->e_phoff + (uint64_t)index * v->eh->e_phentsize;
    if (!in_file(v, offset, sizeof(bc250hsa_elf64_phdr))) {
        return NULL;
    }
    return (const bc250hsa_elf64_phdr*)(v->bytes + offset);
}

static const bc250hsa_elf64_shdr* shdr_at(const elf_view* v, uint32_t index)
{
    const uint64_t offset = v->eh->e_shoff + (uint64_t)index * v->eh->e_shentsize;
    if (!in_file(v, offset, sizeof(bc250hsa_elf64_shdr))) {
        return NULL;
    }
    return (const bc250hsa_elf64_shdr*)(v->bytes + offset);
}

static bc250hsa_status check_header(const elf_view* v)
{
    const bc250hsa_elf64_ehdr* eh = v->eh;
    uint32_t                   mach;

    if (memcmp(eh->e_ident, "\177ELF", 4) != 0 ||
        eh->e_ident[BC250HSA_EI_CLASS] != BC250HSA_ELFCLASS64 ||
        eh->e_ident[BC250HSA_EI_DATA] != BC250HSA_ELFDATA2LSB ||
        eh->e_ident[BC250HSA_EI_OSABI] != BC250HSA_ELFOSABI_AMDGPU_HSA ||
        eh->e_machine != BC250HSA_EM_AMDGPU) {
        return BC250HSA_EBADELF;
    }
    if (eh->e_phentsize != (uint16_t)sizeof(bc250hsa_elf64_phdr) ||
        (eh->e_shnum != 0u && eh->e_shentsize != (uint16_t)sizeof(bc250hsa_elf64_shdr))) {
        return BC250HSA_EBADELF;
    }
    if (eh->e_ident[BC250HSA_EI_ABIVERSION] != BC250HSA_ELFABIVERSION_AMDGPU_HSA_V5 &&
        eh->e_ident[BC250HSA_EI_ABIVERSION] != BC250HSA_ELFABIVERSION_AMDGPU_HSA_V6) {
        return BC250HSA_EBADABIVERSION;
    }
    mach = eh->e_flags & BC250HSA_EF_AMDGPU_MACH;
    if (mach != BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX1013 &&
        mach != BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX10_1_GENERIC) {
        bc250hsa_log(BC250HSA_LOG_ERROR, "code object e_flags mach 0x%03x is not gfx1013", mach);
        return BC250HSA_EWRONGTARGET;
    }
    return BC250HSA_OK;
}

/* The one NT_AMDGPU_METADATA note, from the section headers when they exist and from
 * the PT_NOTE segment otherwise. */
static bc250hsa_status find_metadata(const elf_view* v, const uint8_t** note,
                                     size_t* note_bytes)
{
    static const char kOwner[] = BC250HSA_NOTE_OWNER_AMDGPU;
    uint32_t          pass;

    *note = NULL;
    *note_bytes = 0;
    for (pass = 0; pass < 2u; pass++) {
        uint64_t area_offset = 0;
        uint64_t area_bytes = 0;
        uint32_t index;
        uint32_t count = (pass == 0u) ? v->eh->e_shnum : v->eh->e_phnum;

        for (index = 0; index < count; index++) {
            uint64_t at;

            if (pass == 0u) {
                const bc250hsa_elf64_shdr* sh = shdr_at(v, index);
                if (sh == NULL || sh->sh_type != BC250HSA_SHT_NOTE) {
                    continue;
                }
                area_offset = sh->sh_offset;
                area_bytes = sh->sh_size;
            } else {
                const bc250hsa_elf64_phdr* ph = phdr_at(v, index);
                if (ph == NULL || ph->p_type != BC250HSA_PT_NOTE) {
                    continue;
                }
                area_offset = ph->p_offset;
                area_bytes = ph->p_filesz;
            }
            if (!in_file(v, area_offset, area_bytes)) {
                return BC250HSA_EBADELF;
            }
            at = 0;
            while (at + 12u <= area_bytes) {
                uint32_t name_bytes;
                uint32_t desc_bytes;
                uint32_t type;
                uint64_t name_at;
                uint64_t desc_at;

                memcpy(&name_bytes, v->bytes + area_offset + at + 0u, 4);
                memcpy(&desc_bytes, v->bytes + area_offset + at + 4u, 4);
                memcpy(&type, v->bytes + area_offset + at + 8u, 4);
                name_at = at + 12u;
                desc_at = name_at + bc250hsa_align_up_u64(name_bytes, 4u);
                if (desc_at + desc_bytes > area_bytes) {
                    break;
                }
                if (type == BC250HSA_NT_AMDGPU_METADATA &&
                    name_bytes >= sizeof(kOwner) - 1u &&
                    memcmp(v->bytes + area_offset + name_at, kOwner, sizeof(kOwner) - 1u) == 0) {
                    *note = v->bytes + area_offset + desc_at;
                    *note_bytes = desc_bytes;
                    return BC250HSA_OK;
                }
                at = desc_at + bc250hsa_align_up_u64(desc_bytes, 4u);
            }
        }
        if (*note != NULL) {
            break;
        }
    }
    return BC250HSA_EBADMETADATA;
}

static void module_free_contents(struct bc250hsa_module* mod)
{
    uint32_t i;

    if (mod->kernels != NULL) {
        for (i = 0; i < mod->kernel_count; i++) {
            free(mod->kernels[i].name);
            free(mod->kernels[i].symbol);
            free(mod->kernels[i].args);
        }
        free(mod->kernels);
        mod->kernels = NULL;
    }
    free(mod->dynsym);
    mod->dynsym = NULL;
    free(mod->dynstr);
    mod->dynstr = NULL;
    if (mod->image.opaque != NULL || mod->image.va != 0u) {
        if (mod->alloc.free != NULL) {
            mod->alloc.free(mod->alloc.ctx, &mod->image);
        }
        memset(&mod->image, 0, sizeof(mod->image));
    }
}

static bc250hsa_status copy_dynsym(const elf_view* v, struct bc250hsa_module* mod)
{
    uint32_t index;

    for (index = 0; index < v->eh->e_shnum; index++) {
        const bc250hsa_elf64_shdr* sh = shdr_at(v, index);
        const bc250hsa_elf64_shdr* str;

        if (sh == NULL || sh->sh_type != BC250HSA_SHT_DYNSYM) {
            continue;
        }
        if (sh->sh_entsize != sizeof(bc250hsa_elf64_sym) || !in_file(v, sh->sh_offset, sh->sh_size)) {
            return BC250HSA_EBADELF;
        }
        if (sh->sh_link >= v->eh->e_shnum) {
            return BC250HSA_EBADELF;
        }
        str = shdr_at(v, sh->sh_link);
        if (str == NULL || str->sh_type != BC250HSA_SHT_STRTAB ||
            !in_file(v, str->sh_offset, str->sh_size)) {
            return BC250HSA_EBADELF;
        }
        mod->dynsym = (uint8_t*)malloc((size_t)sh->sh_size);
        mod->dynstr = (char*)malloc((size_t)str->sh_size + 1u);
        if (mod->dynsym == NULL || mod->dynstr == NULL) {
            return BC250HSA_ENOMEM;
        }
        memcpy(mod->dynsym, v->bytes + sh->sh_offset, (size_t)sh->sh_size);
        memcpy(mod->dynstr, v->bytes + str->sh_offset, (size_t)str->sh_size);
        mod->dynstr[str->sh_size] = '\0';
        mod->dynsym_bytes = (size_t)sh->sh_size;
        mod->dynstr_bytes = (size_t)str->sh_size + 1u;
        return BC250HSA_OK;
    }
    return BC250HSA_EBADELF;
}

static int lookup_dynsym(const struct bc250hsa_module* mod, const char* name,
                         uint64_t* value, uint64_t* bytes)
{
    const size_t count = mod->dynsym_bytes / sizeof(bc250hsa_elf64_sym);
    size_t       i;

    for (i = 0; i < count; i++) {
        bc250hsa_elf64_sym sym;
        memcpy(&sym, mod->dynsym + i * sizeof(sym), sizeof(sym));
        if (sym.st_name == 0u || (size_t)sym.st_name >= mod->dynstr_bytes) {
            continue;
        }
        if (strcmp(mod->dynstr + sym.st_name, name) == 0) {
            *value = sym.st_value;
            *bytes = sym.st_size;
            return 1;
        }
    }
    return 0;
}

static bc250hsa_status apply_relocations(const elf_view* v, struct bc250hsa_module* mod,
                                        uint64_t bias)
{
    uint8_t* host = (uint8_t*)mod->image.host;
    uint32_t index;

    for (index = 0; index < v->eh->e_shnum; index++) {
        const bc250hsa_elf64_shdr* sh = shdr_at(v, index);
        uint64_t                   count;
        uint64_t                   i;

        if (sh == NULL || sh->sh_type != BC250HSA_SHT_RELA) {
            continue;
        }
        if (sh->sh_entsize != sizeof(bc250hsa_elf64_rela) ||
            !in_file(v, sh->sh_offset, sh->sh_size)) {
            return BC250HSA_EBADELF;
        }
        count = sh->sh_size / sizeof(bc250hsa_elf64_rela);
        for (i = 0; i < count; i++) {
            bc250hsa_elf64_rela rel;
            uint64_t            value;
            uint64_t            where;

            memcpy(&rel, v->bytes + sh->sh_offset + i * sizeof(rel), sizeof(rel));
            if (BC250HSA_ELF64_R_TYPE(rel.r_info) != BC250HSA_R_AMDGPU_RELATIVE64) {
                bc250hsa_log(BC250HSA_LOG_ERROR, "relocation type %u is not R_AMDGPU_RELATIVE64",
                             BC250HSA_ELF64_R_TYPE(rel.r_info));
                return BC250HSA_EBADRELOC;
            }
            /* A record that names a symbol is an unresolved external from a
             * -fgpu-rdc build with no device link step. */
            if (BC250HSA_ELF64_R_SYM(rel.r_info) != 0u) {
                bc250hsa_log(BC250HSA_LOG_ERROR,
                             "relocation names symbol %u: the object is not device linked",
                             BC250HSA_ELF64_R_SYM(rel.r_info));
                return BC250HSA_EBADRELOC;
            }
            if (rel.r_offset < mod->lowest_vaddr) {
                return BC250HSA_EBADRELOC;
            }
            where = rel.r_offset - mod->lowest_vaddr;
            if (where + 8u > mod->image.bytes) {
                return BC250HSA_EBADRELOC;
            }
            value = bias + (uint64_t)rel.r_addend;   /* B + A */
            memcpy(host + where, &value, 8);
        }
    }
    return BC250HSA_OK;
}

static bc250hsa_status load_image(const bc250hsa_allocator* alloc, const void* image,
                                  size_t image_bytes, struct bc250hsa_module** out)
{
    elf_view                v;
    struct bc250hsa_module* mod;
    bc250hsa_status         status;
    uint64_t                lowest = UINT64_MAX;
    uint64_t                highest = 0;
    uint64_t                span;
    uint64_t                bias;
    uint32_t                index;
    const uint8_t*          note = NULL;
    size_t                  note_bytes = 0;
    int                      any_load = 0;

    if (alloc == NULL || alloc->alloc == NULL || alloc->free == NULL || image == NULL ||
        out == NULL) {
        return BC250HSA_EINVAL;
    }
    *out = NULL;
    if (image_bytes < sizeof(bc250hsa_elf64_ehdr)) {
        return BC250HSA_EBADELF;
    }
    v.bytes = (const uint8_t*)image;
    v.byte_count = image_bytes;
    v.eh = (const bc250hsa_elf64_ehdr*)image;
    status = check_header(&v);
    if (status != BC250HSA_OK) {
        return status;
    }

    for (index = 0; index < v.eh->e_phnum; index++) {
        const bc250hsa_elf64_phdr* ph = phdr_at(&v, index);
        if (ph == NULL) {
            return BC250HSA_EBADELF;
        }
        if (ph->p_type != BC250HSA_PT_LOAD) {
            continue;
        }
        if (!in_file(&v, ph->p_offset, ph->p_filesz) || ph->p_filesz > ph->p_memsz) {
            return BC250HSA_EBADELF;
        }
        if (ph->p_vaddr < lowest) {
            lowest = ph->p_vaddr;
        }
        if (ph->p_vaddr + ph->p_memsz > highest) {
            highest = ph->p_vaddr + ph->p_memsz;
        }
        any_load = 1;
    }
    if (!any_load || highest <= lowest) {
        return BC250HSA_EBADELF;
    }
    span = bc250hsa_align_up_u64(highest - lowest, 4096u);

    mod = (struct bc250hsa_module*)calloc(1, sizeof(*mod));
    if (mod == NULL) {
        return BC250HSA_ENOMEM;
    }
    mod->alloc = *alloc;
    mod->lowest_vaddr = lowest;

    status = alloc->alloc(alloc->ctx, span, 4096u, BC250HSA_MEM_EXEC, &mod->image);
    if (status != BC250HSA_OK) {
        free(mod);
        return status;
    }
    if (mod->image.host == NULL || mod->image.bytes < span) {
        module_free_contents(mod);
        free(mod);
        return BC250HSA_ENOMEM;
    }
    /* Step 4: copy p_filesz and zero the rest of p_memsz. The zero fill is not
     * optional: .bss and .relro_padding have p_filesz 0 and a non-zero p_memsz. */
    memset(mod->image.host, 0, (size_t)span);
    for (index = 0; index < v.eh->e_phnum; index++) {
        const bc250hsa_elf64_phdr* ph = phdr_at(&v, index);
        if (ph == NULL || ph->p_type != BC250HSA_PT_LOAD || ph->p_filesz == 0u) {
            continue;
        }
        memcpy((uint8_t*)mod->image.host + (ph->p_vaddr - lowest), v.bytes + ph->p_offset,
               (size_t)ph->p_filesz);
    }

    bias = mod->image.va - lowest;
    status = apply_relocations(&v, mod, bias);
    if (status != BC250HSA_OK) {
        goto failed;
    }
    status = find_metadata(&v, &note, &note_bytes);
    if (status != BC250HSA_OK) {
        goto failed;
    }
    status = bc250hsa_metadata_parse(note, note_bytes, mod);
    if (status != BC250HSA_OK) {
        goto failed;
    }
    status = copy_dynsym(&v, mod);
    if (status != BC250HSA_OK) {
        goto failed;
    }

    for (index = 0; index < mod->kernel_count; index++) {
        bc250hsa_kernel_internal* k = &mod->kernels[index];
        uint64_t                  symbol_value = 0;
        uint64_t                  symbol_bytes = 0;
        uint64_t                  where;

        if (!lookup_dynsym(mod, k->symbol, &symbol_value, &symbol_bytes)) {
            bc250hsa_log(BC250HSA_LOG_ERROR, "kernel descriptor symbol %s is not in .dynsym",
                         k->symbol);
            status = BC250HSA_ENOTFOUND;
            goto failed;
        }
        if (symbol_bytes != 64u || symbol_value < lowest) {
            status = BC250HSA_EBADELF;
            goto failed;
        }
        where = symbol_value - lowest;
        if (where + 64u > mod->image.bytes) {
            status = BC250HSA_EBADELF;
            goto failed;
        }
        status = bc250hsa_descriptor_read((const uint8_t*)mod->image.host + where, 64u, k);
        if (status != BC250HSA_OK) {
            goto failed;
        }
        k->pub.descriptor_va = mod->image.va + where;
        k->pub.entry_va = k->pub.descriptor_va + k->descriptor.kernel_code_entry_byte_offset;
        if (k->pub.entry_va < mod->image.va ||
            k->pub.entry_va >= mod->image.va + mod->image.bytes) {
            status = BC250HSA_EBADELF;
            goto failed;
        }
        /* COMPUTE_PGM_LO takes the entry address shifted right by 8, so the low
         * eight bits are not in the register at all. */
        if ((k->pub.entry_va & 0xFFu) != 0u) {
            status = BC250HSA_EBADELF;
            goto failed;
        }
    }

    bc250hsa_count_add(BC250HSA_C_MODULES_LOADED, 1u);
    *out = mod;
    return BC250HSA_OK;

failed:
    module_free_contents(mod);
    free(mod);
    return status;
}

bc250hsa_status bc250hsa_module_load_alloc(const bc250hsa_allocator* alloc, const void* image,
                                           size_t image_bytes, struct bc250hsa_module** out)
{
    return load_image(alloc, image, image_bytes, out);
}

void bc250hsa_module_unload(struct bc250hsa_module* mod)
{
    if (mod == NULL) {
        return;
    }
    module_free_contents(mod);
    free(mod);
}

uint32_t bc250hsa_module_kernel_count(const struct bc250hsa_module* mod)
{
    return (mod == NULL) ? 0u : mod->kernel_count;
}

const bc250hsa_kernel* bc250hsa_module_kernel_at(const struct bc250hsa_module* mod,
                                                 uint32_t index)
{
    if (mod == NULL || index >= mod->kernel_count) {
        return NULL;
    }
    return &mod->kernels[index].pub;
}

const bc250hsa_kernel* bc250hsa_module_kernel_by_name(const struct bc250hsa_module* mod,
                                                      const char* name)
{
    uint32_t i;

    if (mod == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < mod->kernel_count; i++) {
        if (mod->kernels[i].name != NULL && strcmp(mod->kernels[i].name, name) == 0) {
            return &mod->kernels[i].pub;
        }
    }
    return NULL;
}

bc250hsa_status bc250hsa_module_symbol(const struct bc250hsa_module* mod, const char* name,
                                       uint64_t* va, uint64_t* bytes)
{
    uint64_t value = 0;
    uint64_t size = 0;

    if (mod == NULL || name == NULL || va == NULL || bytes == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!lookup_dynsym(mod, name, &value, &size)) {
        return BC250HSA_ENOTFOUND;
    }
    if (value < mod->lowest_vaddr) {
        return BC250HSA_EBADELF;
    }
    *va = mod->image.va + (value - mod->lowest_vaddr);
    *bytes = size;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_module_range(const struct bc250hsa_module* mod, uint64_t* va,
                                      uint64_t* bytes)
{
    if (mod == NULL || va == NULL || bytes == NULL) {
        return BC250HSA_EINVAL;
    }
    *va = mod->image.va;
    *bytes = mod->image.bytes;
    return BC250HSA_OK;
}
