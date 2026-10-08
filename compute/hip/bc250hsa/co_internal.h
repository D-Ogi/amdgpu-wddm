/* co_internal.h - the small MessagePack reader the metadata note needs, and the ELF
 * constants the loader checks.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, sections 3.4 and 3.5. The reader stays
 * small because the NT_AMDGPU_METADATA note uses maps, arrays, strings, integers and
 * booleans only (measured on the spike's own code objects). Everything else is
 * refused, never guessed.
 *
 * Sources: LLVM llvm/docs/AMDGPUUsage.rst ("Code Object V5 Metadata",
 * "Relocation Records", "Kernel Descriptor") and the MessagePack specification as
 * AMDGPUUsage names it.
 */
#ifndef BC250HSA_CO_INTERNAL_H
#define BC250HSA_CO_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * ELF, only the parts the loader reads
 * ------------------------------------------------------------------------ */

#define BC250HSA_EI_NIDENT     16
#define BC250HSA_EI_CLASS      4
#define BC250HSA_EI_DATA       5
#define BC250HSA_EI_OSABI      7
#define BC250HSA_EI_ABIVERSION 8

#define BC250HSA_ELFCLASS64 2
#define BC250HSA_ELFDATA2LSB 1
#define BC250HSA_ELFOSABI_AMDGPU_HSA 0x40
#define BC250HSA_EM_AMDGPU 0xE0

/* e_flags, AMDGPUUsage.rst "Code Object Target Identification". */
#define BC250HSA_EF_AMDGPU_MACH 0x0FF
#define BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX1013 0x042
#define BC250HSA_EF_AMDGPU_MACH_AMDGCN_GFX10_1_GENERIC 0x052

/* e_ident[EI_ABIVERSION]: 3 is code object version 5, 4 is version 6. */
#define BC250HSA_ELFABIVERSION_AMDGPU_HSA_V5 3
#define BC250HSA_ELFABIVERSION_AMDGPU_HSA_V6 4

#define BC250HSA_PT_LOAD    1u
#define BC250HSA_PT_DYNAMIC 2u
#define BC250HSA_PT_NOTE    4u

#define BC250HSA_SHT_RELA    4u
#define BC250HSA_SHT_NOTE    7u
#define BC250HSA_SHT_DYNSYM  11u
#define BC250HSA_SHT_STRTAB  3u

/* The one relocation type a linked gfx1013 code object needs: B + A. */
#define BC250HSA_R_AMDGPU_RELATIVE64 13u

/* NT_AMDGPU_METADATA, AMDGPUUsage.rst "Code Object Metadata". */
#define BC250HSA_NT_AMDGPU_METADATA 32u
#define BC250HSA_NOTE_OWNER_AMDGPU "AMDGPU"

typedef struct bc250hsa_elf64_ehdr {
    uint8_t  e_ident[BC250HSA_EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} bc250hsa_elf64_ehdr;

typedef struct bc250hsa_elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} bc250hsa_elf64_phdr;

typedef struct bc250hsa_elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} bc250hsa_elf64_shdr;

typedef struct bc250hsa_elf64_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} bc250hsa_elf64_sym;

typedef struct bc250hsa_elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} bc250hsa_elf64_rela;

#define BC250HSA_ELF64_R_SYM(info)  ((uint32_t)((info) >> 32))
#define BC250HSA_ELF64_R_TYPE(info) ((uint32_t)((info) & 0xFFFFFFFFu))

/* --------------------------------------------------------------------------
 * The offload bundle (clang/docs/ClangOffloadBundler.rst)
 * ------------------------------------------------------------------------ */

#define BC250HSA_BUNDLE_MAGIC "__CLANG_OFFLOAD_BUNDLE__"
#define BC250HSA_BUNDLE_MAGIC_BYTES 24u
#define BC250HSA_BUNDLE_COMPRESSED_MAGIC "CCOB"

/* --------------------------------------------------------------------------
 * MessagePack
 * ------------------------------------------------------------------------ */

typedef enum bc250hsa_mp_kind {
    BC250HSA_MP_NIL = 0,
    BC250HSA_MP_BOOL,
    BC250HSA_MP_UINT,
    BC250HSA_MP_INT,
    BC250HSA_MP_STR,
    BC250HSA_MP_ARRAY,
    BC250HSA_MP_MAP
} bc250hsa_mp_kind;

typedef struct bc250hsa_mp_value {
    bc250hsa_mp_kind kind;
    union {
        int         boolean;
        uint64_t    u;
        int64_t     i;
        struct { const char* text; uint32_t bytes; } str;
        uint32_t    count;   /* array elements, or map pairs */
    } as;
} bc250hsa_mp_value;

typedef struct bc250hsa_mp_reader {
    const uint8_t* at;
    const uint8_t* end;
    int            failed;
} bc250hsa_mp_reader;

void bc250hsa_mp_init(bc250hsa_mp_reader* r, const void* bytes, size_t byte_count);
/* Reads the next value. On an unreadable byte it sets r->failed and returns 0. An
 * array or a map value reports its element count only; the caller then reads the
 * elements, or calls bc250hsa_mp_skip() to step over the whole container. */
int  bc250hsa_mp_next(bc250hsa_mp_reader* r, bc250hsa_mp_value* out);
/* Steps over one complete value, container and all. */
int  bc250hsa_mp_skip(bc250hsa_mp_reader* r);
/* True when the value is a string equal to key. */
int  bc250hsa_mp_str_is(const bc250hsa_mp_value* v, const char* key);
/* The unsigned value of an integer value, which the metadata uses for every size.
 * A negative integer fails. */
int  bc250hsa_mp_as_u64(const bc250hsa_mp_value* v, uint64_t* out);

#endif /* BC250HSA_CO_INTERNAL_H */
