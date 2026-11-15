/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The identification of the register BAR (BAR5) of the GPU function, for every module that reads or writes a
 * register (docs/design/efi-core-unlock.md, "The register BAR"). Pure code: no firmware call. stage_reg.c collects
 * what the firmware says, and test/stage_test.c runs the same functions on test data.
 *
 * Two proofs, as driver/kmd/mmio.c does it: the address that the configuration header gives for BAR5, and a known
 * register value read through the access path to that address. The resource descriptors of the firmware only
 * select the access path. A firmware that does not fill them does not stop the identification, and a firmware whose
 * descriptor contradicts the configuration header does.
 */
#ifndef STAGE_BAR_H
#define STAGE_BAR_H
#include <stddef.h>

/* ---- the resource descriptors (ACPI 6.5 section 6.4) ------------------------------------------- */

#define STAGE_BAR_TAG_WORD		0x88u	/* WORD address space descriptor (6.4.3.5.3) */
#define STAGE_BAR_TAG_DWORD	0x87u	/* DWORD address space descriptor (6.4.3.5.2) */
#define STAGE_BAR_TAG_QWORD	0x8Au	/* QWORD address space descriptor (6.4.3.5.1) */
#define STAGE_BAR_TAG_EXTENDED	0x8Bu	/* extended address space descriptor (6.4.3.5.4) */
#define STAGE_BAR_TAG_END		0x79u	/* end tag (6.4.2.9) */
#define STAGE_BAR_SPACE_MEMORY	0u	/* the resource type of a memory range */

/* The bytes that the parse reads at most. A list without an end tag inside this bound is not complete. */
#define STAGE_BAR_LIST_MAX_BYTES	512u
#define STAGE_BAR_MAX_RESOURCES	8u

/* One descriptor. The fields of an address space descriptor are filled for the four address space types and
 * are 0 for the other types. */
struct stage_bar_resource {
	unsigned tag;			/* the first byte */
	unsigned bytes;			/* the bytes of the descriptor, with its header */
	int address_space;		/* 1 for one of the four address space types */
	unsigned type, general, specific;
	unsigned long long granularity, minimum, maximum, translation, length;
};

struct stage_bar_list {
	unsigned count;			/* the descriptors in resource[], at most STAGE_BAR_MAX_RESOURCES */
	unsigned seen;			/* the descriptors in the list, also the ones that did not fit */
	int complete;			/* 1 when the list ends with an end tag inside the bound */
	unsigned end_offset;		/* the offset of the end tag */
	struct stage_bar_resource resource[STAGE_BAR_MAX_RESOURCES];
};

/* Reads a descriptor list up to its end tag. It reads only the bytes that each header announces and never more
 * than max_bytes. A descriptor that does not fit in max_bytes, or an address space descriptor that is shorter
 * than its type, stops the parse with complete = 0. */
void stage_bar_parse(const unsigned char *data, size_t max_bytes, struct stage_bar_list *list);

/* ---- the configuration header ------------------------------------------------------------------ */

enum stage_bar_config {
	STAGE_BAR_CONFIG_OK = 0,
	STAGE_BAR_CONFIG_DECODE_OFF = 1,	/* the command register has the memory decode off */
	STAGE_BAR_CONFIG_NOT_MEMORY = 2,	/* BAR5 is an I/O BAR, or its type is not a 32-bit memory BAR */
	STAGE_BAR_CONFIG_UPPER_HALF = 3,	/* BAR4 is a 64-bit memory BAR, so the dword of BAR5 is its upper half */
	STAGE_BAR_CONFIG_EMPTY = 4,	/* BAR5 holds no address */
	STAGE_BAR_CONFIG_COUNT
};

/* The address of the register BAR from the first 64 bytes of the configuration header. */
enum stage_bar_config stage_bar_config_address(const unsigned char *config, size_t bytes, unsigned long long *address);
const char *stage_bar_config_text(enum stage_bar_config result);

/* ---- the access path ----------------------------------------------------------------------------- */

enum stage_bar_match {
	STAGE_BAR_MATCH_NONE = 0,		/* no memory descriptor at the address with a length */
	STAGE_BAR_MATCH_EXACT = 1,		/* a memory descriptor at the address with the length of the register BAR */
	STAGE_BAR_MATCH_CONFLICT = 2	/* a memory descriptor at the address with another length that is not 0 */
};

/* What the descriptor list of one PCI I/O BarIndex says about the register BAR. The address is the host
 * address: AddrRangeMin, as UEFI 2.10 section 14.4 gives it for GetBarAttributes. */
enum stage_bar_match stage_bar_match(const struct stage_bar_list *list, unsigned long long address, unsigned long long length);

/* 1 when a memory window of the list holds address to address + length - 1. */
int stage_bar_window_holds(const struct stage_bar_list *list, unsigned long long address, unsigned long long length);

#define STAGE_BAR_INDEXES		6u	/* the BarIndex values of a type 0 header */
#define STAGE_BAR_MAX_BRIDGES	8u

/* What the firmware said, collected by the binding. */
struct stage_bar_findings {
	enum stage_bar_config config;
	unsigned long long address;		/* valid when config is STAGE_BAR_CONFIG_OK */
	enum stage_bar_match index[STAGE_BAR_INDEXES];	/* per PCI I/O BarIndex; NONE when GetBarAttributes failed */
	unsigned bridges;			/* the root bridges of the GPU function's segment */
	unsigned bridges_holding;		/* of these, the ones with a memory window that holds the BAR */
	unsigned bridges_described;		/* of these, the ones whose Configuration answered with a complete list */
	unsigned bridge;			/* the root bridge that holds the BAR, valid for STAGE_BAR_PATH_BRIDGE */
};

enum stage_bar_path {
	STAGE_BAR_PATH_NONE = 0,		/* refused: nothing is read or written */
	STAGE_BAR_PATH_PCI_IO = 1,		/* PCI I/O Mem.Read and Mem.Write with the BarIndex whose descriptor matched */
	STAGE_BAR_PATH_BRIDGE = 2		/* PCI Root Bridge I/O Mem.Read and Mem.Write at the address of BAR5 */
};

enum stage_bar_reason {
	STAGE_BAR_WHY_INDEX = 0,		/* one BarIndex has the exact descriptor */
	STAGE_BAR_WHY_BRIDGE = 1,		/* no BarIndex has it, one root bridge serves the address */
	STAGE_BAR_WHY_CONFIG = 2,		/* the configuration header gives no register BAR */
	STAGE_BAR_WHY_CONFLICT = 3,	/* a descriptor at the address has another length */
	STAGE_BAR_WHY_AMBIGUOUS = 4,	/* more than one BarIndex has the exact descriptor */
	STAGE_BAR_WHY_NO_BRIDGE = 5,	/* no descriptor and no single root bridge that serves the address */
	STAGE_BAR_WHY_COUNT
};

struct stage_bar_choice {
	enum stage_bar_path path;
	enum stage_bar_reason reason;
	unsigned index;			/* the BarIndex for STAGE_BAR_PATH_PCI_IO */
	unsigned bridge;		/* the root bridge for STAGE_BAR_PATH_BRIDGE */
	unsigned long long address;	/* the address of BAR5 */
};

/* The access path, or none. A root bridge counts when it is the only one of the segment and its Configuration
 * does not contradict, or when it is the only one that holds the address. */
struct stage_bar_choice stage_bar_choose(const struct stage_bar_findings *findings);
const char *stage_bar_reason_text(enum stage_bar_reason reason);

/* ---- the positive control ------------------------------------------------------------------------ */

/* The register that the control reads and the value that it must have. GB_ADDR_CONFIG reads this value on unit A
 * before the driver, under Linux and under Windows (facts M5 and M18). Its byte offset comes from the AMD headers. */
#define STAGE_BAR_CONTROL_NAME	"GB_ADDR_CONFIG"
#define STAGE_BAR_CONTROL_VALUE	0x00000044u

enum stage_bar_control {
	STAGE_BAR_CONTROL_OK = 0,
	STAGE_BAR_CONTROL_READ_FAILED = 1,	/* the access path refused the read */
	STAGE_BAR_CONTROL_MISMATCH = 2,	/* the read gave another value */
	STAGE_BAR_CONTROL_COUNT
};

/* The byte offset of the control register in the register BAR. */
unsigned stage_bar_control_offset(void);

/* One read of the control register through read32 (0 is success). Writes nothing. value gets what was read. */
enum stage_bar_control stage_bar_control_read(int (*read32)(void *context, unsigned offset, unsigned *value),
					      void *context, unsigned *value);
const char *stage_bar_control_text(enum stage_bar_control result);

#endif
