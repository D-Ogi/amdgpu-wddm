/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The interface between the core of the BC-250 stage and its modules (docs/design/efi-resident-stage.md).
 *
 * The core owns the console, the load options, the volume of the stage, the board identity and the start of the
 * Windows Boot Manager. A module owns one job. A module calls the core through the functions below and never
 * calls another module. The core calls a module only through its struct stage_module.
 */
#ifndef STAGE_H
#define STAGE_H
#include <stddef.h>
#include "efi.h"
#include "efl_acpi.h"
#include "stage_options.h"

/* The version of the stage. The pre-OS TPM measures it into PCR 0 (EV_S_CRTM_VERSION). */
#define STAGE_VERSION		"4"
#define STAGE_PATH_CHARS	260u
#define STAGE_PCI_CONFIG_BYTES	64u

struct stage {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *st;
	EFI_BOOT_SERVICES *bs;
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	struct stage_options options;
	EFI_FILE_PROTOCOL *root;			/* the volume of the stage image, or NULL */
	unsigned short image_file[STAGE_PATH_CHARS];	/* the path of the stage image on that volume */
	int image_file_known;
	/* The board identity, filled by stage_board_bc250 at its first call. */
	int board_checked, board_bc250;
	EFI_PCI_IO_PROTOCOL *gpu;			/* the GPU function of a BC-250, else NULL */
	unsigned char gpu_config[STAGE_PCI_CONFIG_BYTES];
	/* The handoff range of the pre-OS TPM (driver/tpm/handoff.h): reserved pages that the TPM module fills at
	 * ExitBootServices. The TPM module sets it in its run; the ACPI repair describes it in Device (TPM0). 0 when
	 * no pre-OS TPM runs. */
	unsigned long long tpm_handoff;
	unsigned tpm_handoff_bytes;
};

/* ---- what the core gives a module ---------------------------------------------------------------- */

/* One console line with the prefix "bc250-stage: module: ". */
void stage_say(struct stage *stage, const char *module, const char *line);
void stage_sayf(struct stage *stage, const char *module, const char *format, ...);

/* One console line that does not go to the log file: for long text that has a file of its own. */
void stage_say_console(struct stage *stage, const char *module, const char *line);

/* Writes the console lines that are not yet in the log file (bc250-stage.log beside the stage image). The core
 * does this before it starts the next image and before it returns. A module calls it before it restarts the
 * board. Nothing happens when the load option log=0 is set or the volume is not known. */
void stage_log_flush(struct stage *stage);

/* 1 when the board is a BC-250: exactly one PCI function and the SMBIOS table match the driver's own identity
 * check (driver/common/board/board_identity.c). The answer is found once. */
int stage_board_bc250(struct stage *stage);

/* The ACPI tables as they are now. The address of the RSDP is read again at each call, because an installed or
 * removed table can move the root tables. */
void stage_acpi(struct stage *stage, struct efl_acpi *acpi);

/* The path of a file beside the stage image. 0 on failure. */
int stage_sibling(struct stage *stage, const char *name, unsigned short *out, size_t capacity);

/* Reads a whole file of the stage's volume into pool memory. The caller frees it with stage_free. */
enum stage_read {
	STAGE_READ_OK = 0,
	STAGE_READ_ABSENT,	/* no such file, or no volume */
	STAGE_READ_FAILED	/* the file is there and did not read, or is larger than max_bytes */
};
enum stage_read stage_read_file(struct stage *stage, const unsigned short *path, size_t max_bytes,
				unsigned char **data, size_t *bytes);
void *stage_alloc(struct stage *stage, size_t bytes);
void stage_free(struct stage *stage, void *memory);

/* ---- what a module gives the core ---------------------------------------------------------------- */

/* The answer of a module to "may the core start this image". */
enum stage_gate {
	STAGE_GATE_PASS = 0,	/* no objection */
	STAGE_GATE_BIND = 1,	/* start exactly these bytes: load from the buffer, not from the path again */
	STAGE_GATE_DENY = 2	/* do not start it */
};

struct stage_module {
	const char *name;		/* the console prefix */
	const char *off_switch;		/* the load option whose value 0 turns the whole module off */
	/* The work before Windows starts. A module that resets the board does not return. */
	void (*run)(struct stage *stage);
	/* Called in place of run when the module is off and the load option probe=1 is set: says whether the
	 * firmware has what the module needs. It reads only and changes nothing. NULL when there is nothing to say. */
	void (*probe)(struct stage *stage);
	/* Asked once for the image that the core is about to start. image is NULL when the file did not read.
	 * NULL when the module has no say. */
	enum stage_gate (*gate)(struct stage *stage, const unsigned char *image, size_t bytes);
	/* Called after the load of that image, with the status of LoadImage. NULL when not needed. */
	void (*loaded)(struct stage *stage, EFI_STATUS status);
	/* Called when the image did not start or returned: undo what must not stay. NULL when nothing stays. */
	void (*unwind)(struct stage *stage);
};

extern const struct stage_module cu_module;	/* core-unlock/src/cu_module.c */
extern const struct stage_module ar_module;	/* acpi-repair/src/ar_module.c */
extern const struct stage_module sb_module;	/* secure-boot/src/sb_module.c */
extern const struct stage_module tpm_module;	/* tpm/src/tpm_module.c */

#endif
