/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The core unlock as a module of the BC-250 stage (docs/design/efi-core-unlock.md, docs/design/efi-resident-stage.md).
 * It enables the two CPU cores that the BC-250 is sold without, and then resets the board. This file is the UEFI
 * binding only: it gives cu_run() its operations. The decision is cu_decide.c, the steps are cu_run.c, the message
 * is cu_smu.c over the driver's own transport and policy. The board identity comes from the core of the stage.
 *
 * Nothing of this module stays in memory after its run.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_acpi.h"
#include "efl_text.h"
#include "stage.h"
#include "stage_reg.h"
#include "stage_bar.h"
#include "cu_decide.h"
#include "cu_options.h"
#include "cu_report.h"
#include "cu_run.h"
#include "cu_smu.h"
#include "report_uefi.h"
#include "bc250_mmio_bar.h"

#define CU_NAME			"core-unlock"
#define CU_VERSION		"3"
#define CU_KEY_WINDOW_MS	1500u
#define CU_KEY_POLL_MS		10u
#define CU_RESET_PAUSE_MS	1000u	/* the console lines stay readable for this long before a reset */
#define CU_RESET_WAIT_MS	1000u	/* a reset register write that has not reset the board by then did not work */
#define CU_MARKER_MAX_BYTES	64u

static const EFI_GUID mp_services_guid = EFI_MP_SERVICES_PROTOCOL_GUID;

struct unlock {
	struct stage *stage;
	struct cu_options options;
	unsigned short marker[STAGE_PATH_CHARS];
	int marker_known;
	unsigned short report[STAGE_PATH_CHARS];
	int report_known;
};

/* ---- the operations of cu_run ------------------------------------------------------------------ */

static int op_board(void *context)
{
	return stage_board_bc250(((struct unlock *)context)->stage);
}

static unsigned op_processors(void *context)
{
	struct stage *stage = ((struct unlock *)context)->stage;
	EFI_MP_SERVICES_PROTOCOL *mp = NULL;
	struct efl_acpi acpi;
	UINTN all = 0, enabled = 0;
	unsigned madt = 0;
	if (!EFI_ERROR(stage->bs->LocateProtocol(&mp_services_guid, NULL, (VOID **)&mp)) && mp != NULL &&
	    !EFI_ERROR(mp->GetNumberOfProcessors(mp, &all, &enabled))) {
		stage_sayf(stage, CU_NAME, "MP Services: %u logical processors, %u enabled", (unsigned)all,
			   (unsigned)enabled);
		return (unsigned)enabled;
	}
	/* The fallback: the enabled processors of the ACPI MADT. */
	stage_acpi(stage, &acpi);
	if (efl_acpi_madt_processors(&acpi, &madt) == EFL_ACPI_OK) {
		stage_sayf(stage, CU_NAME, "no MP Services; the MADT lists %u enabled logical processors", madt);
		return madt;
	}
	stage_say(stage, CU_NAME, "no MP Services and no MADT: the processor count is not known");
	return 0;
}

static int marker_open(struct unlock *u, EFI_FILE_PROTOCOL **file, UINT64 mode, EFI_STATUS *status)
{
	*file = NULL;
	if (u->stage->root == NULL || !u->marker_known) {
		*status = EFI_NOT_READY;
		return -1;
	}
	*status = u->stage->root->Open(u->stage->root, file, u->marker, mode, 0);
	return EFI_ERROR(*status) ? -1 : 0;
}

static enum cu_marker op_marker_read(void *context)
{
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	unsigned char data[CU_MARKER_MAX_BYTES];
	UINTN bytes = sizeof(data);
	if (marker_open(context, &file, EFI_FILE_MODE_READ, &status))
		return status == EFI_NOT_FOUND ? CU_MARKER_NONE : CU_MARKER_ERROR;
	status = file->Read(file, &bytes, data);
	file->Close(file);
	if (EFI_ERROR(status)) return CU_MARKER_ERROR;
	return cu_marker_parse(data, (unsigned long)bytes);
}

static int op_marker_write(void *context, enum cu_marker state)
{
	struct unlock *u = context;
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	const char *text = cu_marker_text(state);
	UINTN bytes;
	if (text == NULL) return -1;
	if (marker_open(u, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, &status)) {
		stage_sayf(u->stage, CU_NAME, "marker create failed, status 0x%llx", (unsigned long long)status);
		return -1;
	}
	bytes = strlen(text);
	status = file->Write(file, &bytes, (VOID *)text);
	if (!EFI_ERROR(status)) status = file->Flush(file);
	file->Close(file);
	if (EFI_ERROR(status) || bytes != strlen(text)) {
		stage_sayf(u->stage, CU_NAME, "marker write failed, status 0x%llx", (unsigned long long)status);
		return -1;
	}
	/* The guard counts only when it reads back. */
	return op_marker_read(u) == state ? 0 : -1;
}

static int op_marker_delete(void *context)
{
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	if (marker_open(context, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, &status))
		return status == EFI_NOT_FOUND ? 0 : -1;
	/* Delete closes the handle. A warning status means that the file is still there. */
	return file->Delete(file) == EFI_SUCCESS ? 0 : -1;
}

static int op_key_window(void *context)
{
	struct unlock *u = context;
	struct stage *stage = u->stage;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = stage->st->ConIn;
	EFI_INPUT_KEY key;
	unsigned waited;
	if (!u->options.key) {
		/* A board without a person at it: a key that the firmware invents must not stop the unlock. */
		stage_say(stage, CU_NAME, "no key window, by the load option key=0");
		return 0;
	}
	if (in == NULL) return 0;
	/* Keys pressed before the window, at the firmware's own prompt for example, do not count. */
	in->Reset(in, 0);
	stage_sayf(stage, CU_NAME, "press any key within %u ms to skip and start Windows", CU_KEY_WINDOW_MS);
	for (waited = 0; waited < CU_KEY_WINDOW_MS; waited += CU_KEY_POLL_MS) {
		if (in->ReadKeyStroke(in, &key) == EFI_SUCCESS) return 1;
		stage->bs->Stall(CU_KEY_POLL_MS * 1000u);
	}
	return 0;
}

/* ---- the register BAR -------------------------------------------------------------------------- */

/* The SMU transport reaches the register BAR through the core of the stage (stage_reg.h). */
static int bar_read32(void *context, unsigned offset, unsigned *value)
{
	return stage_reg_read32(context, offset, value);
}

static int bar_write32(void *context, unsigned offset, unsigned value)
{
	return stage_reg_write32(context, offset, value);
}

static void bar_stall(void *context, unsigned usec)
{
	((struct stage *)context)->bs->Stall(usec);
}

static void bar_regs(struct stage *stage, struct cu_regs *regs)
{
	regs->context = stage;
	regs->read32 = bar_read32;
	regs->write32 = bar_write32;
	regs->stall_us = bar_stall;
}

static int op_unlock(void *context)
{
	struct stage *stage = ((struct unlock *)context)->stage;
	struct cu_regs regs;
	struct cu_unlock_report report;
	if (stage->gpu == NULL || !stage_reg_identify(stage, CU_NAME)) return -1;
	bar_regs(stage, &regs);
	(void)cu_unlock_send(&regs, &report);
	stage_sayf(stage, CU_NAME, "queue 3: CPU %u mV, temperature %d mC%s", report.millivolts, report.temperature_mc,
		   report.temperature_valid ? "" : " (not read)");
	stage_sayf(stage, CU_NAME, "%s (status %d, response 0x%x, %u messages)", cu_unlock_result_text(report.result),
		   report.status, report.response, report.messages);
	return report.result == CU_UNLOCK_SENT ? 0 : -1;
}

/* ---- the reset ---------------------------------------------------------------------------------- */

static void outb(unsigned short port, unsigned char value)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int reset_fadt(struct stage *stage)
{
	struct efl_acpi acpi;
	struct efl_reset_register reg;
	enum efl_acpi_result result;
	stage_acpi(stage, &acpi);
	result = efl_acpi_reset_register(&acpi, &reg);
	if (result != EFL_ACPI_OK) {
		stage_sayf(stage, CU_NAME, "no usable FADT reset register (reason %u)", (unsigned)result);
		return -1;
	}
	stage_sayf(stage, CU_NAME, "FADT reset register: space %u, address 0x%llx, value 0x%x", reg.space, reg.address,
		   (unsigned)reg.value);
	stage_log_flush(stage);
	if (reg.space == EFL_ACPI_SPACE_IO && reg.address <= 0xFFFFu) {
		stage->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		outb((unsigned short)reg.address, reg.value);
	} else if (reg.space == EFL_ACPI_SPACE_MEMORY) {
		stage->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		*(volatile UINT8 *)(UINTN)reg.address = reg.value;
	} else {
		stage_say(stage, CU_NAME, "a reset register in this address space is not supported");
		return -1;
	}
	stage->bs->Stall(CU_RESET_WAIT_MS * 1000u);
	return -1;
}

static int op_reset(void *context)
{
	struct unlock *u = context;
	struct stage *stage = u->stage;
	switch (u->options.reset) {
	case CU_RESET_WARM:
		stage_log_flush(stage);
		stage->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		stage->st->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
		return -1;
	case CU_RESET_FADT:
		return reset_fadt(stage);
	case CU_RESET_NONE:
	default:
		return -1;
	}
}

static void op_say(void *context, const char *line)
{
	stage_say(((struct unlock *)context)->stage, CU_NAME, line);
}

/* ---- the read-only report ----------------------------------------------------------------------- */

/* A line of the report text goes to the console only. The text has its own file, and the log of the stage keeps
 * the one line that names that file. */
static void op_say_report(void *context, const char *line)
{
	stage_say_console(((struct unlock *)context)->stage, CU_NAME, line);
}

/* The load option report=1: the report goes first, so it shows the state that the decision then reads. It does
 * not feed the decision. */
static void run_report(struct unlock *u)
{
	struct report_uefi r;
	struct efl_acpi acpi;
	stage_acpi(u->stage, &acpi);
	r.st = u->stage->st;
	r.root = u->stage->root;
	r.path = u->report_known ? u->report : NULL;
	r.acpi = &acpi;
	r.version = CU_VERSION;
	r.say = op_say_report;
	r.note = op_say;
	r.say_context = u;
	report_uefi_run(&r);
}

/* ---- the module --------------------------------------------------------------------------------- */

static void cu_module_run(struct stage *stage)
{
	static struct unlock u;
	struct cu_ops ops;

	memset(&u, 0, sizeof(u));
	u.stage = stage;
	cu_options_read(&stage->options, &u.options);
	stage_sayf(stage, CU_NAME, "reset method: %s%s", cu_reset_method_name(u.options.reset),
		   u.options.report ? ", read-only report on" : "");
	u.marker_known = stage_sibling(stage, CU_MARKER_NAME, u.marker, STAGE_PATH_CHARS);
	u.report_known = stage_sibling(stage, CU_REPORT_NAME, u.report, STAGE_PATH_CHARS);
	if (!u.marker_known) stage_say(stage, CU_NAME, "no marker path, so no reset");
	if (u.options.report) run_report(&u);

	ops.context = &u;
	ops.board = op_board;
	ops.processors = op_processors;
	ops.marker_read = op_marker_read;
	ops.marker_write = op_marker_write;
	ops.marker_delete = op_marker_delete;
	ops.key_window = op_key_window;
	ops.unlock = op_unlock;
	ops.reset = op_reset;
	ops.say = op_say;
	(void)cu_run(&ops, u.options.reset != CU_RESET_NONE);
}

/* The load options unlock=0 probe=1: the dump, the identification of the register BAR and its control read.
 * The control is a read of a named register. Nothing is written and no message is sent. */
static void cu_module_probe(struct stage *stage)
{
	if (!stage_board_bc250(stage) || stage->gpu == NULL) {
		stage_say(stage, CU_NAME, "probe: not a BC-250, no register access");
		return;
	}
	stage_say(stage, CU_NAME, stage_reg_identify(stage, CU_NAME) ? "probe: the register BAR is identified, no message sent"
								     : "probe: the register BAR is not identified, no message sent");
}

const struct stage_module cu_module = { CU_NAME, "unlock", 1, cu_module_run, cu_module_probe, NULL, NULL, NULL };
