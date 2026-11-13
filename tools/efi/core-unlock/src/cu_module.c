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
#include "cu_bar.h"
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

/* The access path that cu_bar_choose gave. Every access stays inside BC250_BAR5_LENGTH. */
struct bar_access {
	struct stage *stage;
	struct cu_bar_choice choice;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge;	/* for CU_BAR_PATH_BRIDGE */
};

static int bar_read32(void *context, unsigned offset, unsigned *value)
{
	struct bar_access *a = context;
	EFI_STATUS status;
	UINT32 v = 0;
	if ((offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	if (a->choice.path == CU_BAR_PATH_PCI_IO)
		status = a->stage->gpu->Mem.Read(a->stage->gpu, EfiPciIoWidthUint32, (UINT8)a->choice.index, offset, 1, &v);
	else if (a->choice.path == CU_BAR_PATH_BRIDGE && a->bridge != NULL)
		status = a->bridge->Mem.Read(a->bridge, EfiPciIoWidthUint32, a->choice.address + offset, 1, &v);
	else
		return -1;
	if (EFI_ERROR(status)) return -1;
	*value = v;
	return 0;
}

static int bar_write32(void *context, unsigned offset, unsigned value)
{
	struct bar_access *a = context;
	EFI_STATUS status;
	UINT32 v = value;
	if ((offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	if (a->choice.path == CU_BAR_PATH_PCI_IO)
		status = a->stage->gpu->Mem.Write(a->stage->gpu, EfiPciIoWidthUint32, (UINT8)a->choice.index, offset, 1, &v);
	else if (a->choice.path == CU_BAR_PATH_BRIDGE && a->bridge != NULL)
		status = a->bridge->Mem.Write(a->bridge, EfiPciIoWidthUint32, a->choice.address + offset, 1, &v);
	else
		return -1;
	return EFI_ERROR(status) ? -1 : 0;
}

static void bar_stall(void *context, unsigned usec)
{
	((struct bar_access *)context)->stage->bs->Stall(usec);
}

static void bar_regs(struct bar_access *a, struct cu_regs *regs)
{
	regs->context = a;
	regs->read32 = bar_read32;
	regs->write32 = bar_write32;
	regs->stall_us = bar_stall;
}

static unsigned config16(const unsigned char *config, unsigned offset)
{
	return (unsigned)config[offset] | ((unsigned)config[offset + 1u] << 8);
}

static unsigned config32(const unsigned char *config, unsigned offset)
{
	return config16(config, offset) | (config16(config, offset + 2u) << 16);
}

/* The dump of a descriptor list: every field of every descriptor up to the end tag. Log lines only. */
static void dump_list(struct stage *stage, const char *what, const struct cu_bar_list *list)
{
	unsigned i;
	for (i = 0; i < list->count; i++) {
		const struct cu_bar_resource *r = &list->resource[i];
		if (!r->address_space) {
			stage_sayf(stage, CU_NAME, "bar: %s descriptor %u: tag 0x%x, %u bytes, not an address space descriptor",
				   what, i, r->tag, r->bytes);
			continue;
		}
		stage_sayf(stage, CU_NAME, "bar: %s descriptor %u: tag 0x%x, %u bytes, type %u, general 0x%x, specific 0x%x, "
			   "granularity 0x%llx", what, i, r->tag, r->bytes, r->type, r->general, r->specific, r->granularity);
		stage_sayf(stage, CU_NAME, "bar: %s descriptor %u: minimum 0x%llx, maximum 0x%llx, translation 0x%llx, "
			   "length 0x%llx", what, i, r->minimum, r->maximum, r->translation, r->length);
	}
	if (list->complete)
		stage_sayf(stage, CU_NAME, "bar: %s: %u descriptors, end tag at byte %u", what, list->seen, list->end_offset);
	else
		stage_sayf(stage, CU_NAME, "bar: %s: %u descriptors, no end tag within %u bytes", what, list->seen,
			   CU_BAR_LIST_MAX_BYTES);
}

/* GetBarAttributes for each BarIndex of a type 0 header. UEFI 2.10 section 14.4 calls BarIndex "the BAR index of
 * the standard PCI Configuration header", but the reference implementation (EDK2 PciBusDxe, PciParseBar) counts
 * a 64-bit BAR as one index, so BarIndex and the dword of the header are not the same thing on this GPU. Only the
 * descriptor that names the address of BAR5 selects an index. */
static void scan_indexes(struct stage *stage, struct cu_bar_findings *f)
{
	unsigned i;
	for (i = 0; i < CU_BAR_INDEXES; i++) {
		char what[24];
		UINT64 supports = 0;
		VOID *resources = NULL;
		struct cu_bar_list list;
		EFI_STATUS status = stage->gpu->GetBarAttributes(stage->gpu, (UINT8)i, &supports, &resources);
		f->index[i] = CU_BAR_MATCH_NONE;
		stage_sayf(stage, CU_NAME, "bar: BarIndex %u: status 0x%llx, supports 0x%llx, resources %s", i,
			   (unsigned long long)status, (unsigned long long)supports,
			   EFI_ERROR(status) || resources == NULL ? "none" : "given");
		if (EFI_ERROR(status) || resources == NULL) continue;
		cu_bar_parse(resources, CU_BAR_LIST_MAX_BYTES, &list);
		efl_format(what, sizeof(what), "BarIndex %u", i);
		dump_list(stage, what, &list);
		if (f->config == CU_BAR_CONFIG_OK) f->index[i] = cu_bar_match(&list, f->address, BC250_BAR5_LENGTH);
		stage->bs->FreePool(resources);
	}
}

/* The root bridges of the GPU function's segment, with their windows. Returns the one that cu_bar_choose can
 * select: the only one that holds BAR5, else the only one of the segment. */
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *scan_bridges(struct stage *stage, struct cu_bar_findings *f)
{
	static const EFI_GUID bridge_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *only = NULL, *holding = NULL;
	EFI_HANDLE *handles = NULL;
	UINTN count = 0, i, segment = 0, bus = 0, device = 0, function = 0;
	unsigned only_index = 0, holding_index = 0;
	if (EFI_ERROR(stage->gpu->GetLocation(stage->gpu, &segment, &bus, &device, &function))) {
		stage_say(stage, CU_NAME, "bar: the GPU function has no location: no root bridge");
		return NULL;
	}
	if (EFI_ERROR(stage->bs->LocateHandleBuffer(ByProtocol, &bridge_guid, NULL, &count, &handles))) count = 0;
	stage_sayf(stage, CU_NAME, "bar: %u root bridges, the GPU function is on segment %u", (unsigned)count,
		   (unsigned)segment);
	for (i = 0; i < count && f->bridges < CU_BAR_MAX_BRIDGES; i++) {
		EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge = NULL;
		VOID *resources = NULL;
		struct cu_bar_list list;
		EFI_STATUS status;
		char what[24];
		if (EFI_ERROR(stage->bs->HandleProtocol(handles[i], &bridge_guid, (VOID **)&bridge)) || bridge == NULL) continue;
		if (bridge->SegmentNumber != segment) {
			stage_sayf(stage, CU_NAME, "bar: root bridge %u: segment %u, not the GPU function's", (unsigned)i,
				   (unsigned)bridge->SegmentNumber);
			continue;
		}
		f->bridges++;
		only = bridge;
		only_index = (unsigned)i;
		status = bridge->Configuration(bridge, &resources);
		stage_sayf(stage, CU_NAME, "bar: root bridge %u: segment %u, configuration status 0x%llx", (unsigned)i,
			   (unsigned)bridge->SegmentNumber, (unsigned long long)status);
		if (EFI_ERROR(status) || resources == NULL) continue;
		/* The buffer belongs to the firmware and is not freed here. */
		cu_bar_parse(resources, CU_BAR_LIST_MAX_BYTES, &list);
		efl_format(what, sizeof(what), "root bridge %u", (unsigned)i);
		dump_list(stage, what, &list);
		if (list.complete) f->bridges_described++;
		if (f->config == CU_BAR_CONFIG_OK && cu_bar_window_holds(&list, f->address, BC250_BAR5_LENGTH)) {
			f->bridges_holding++;
			holding = bridge;
			holding_index = (unsigned)i;
		}
	}
	if (handles != NULL) stage->bs->FreePool(handles);
	f->bridge = f->bridges_holding == 1u ? holding_index : only_index;
	return f->bridges_holding == 1u ? holding : only;
}

/* The register BAR is identified twice, as driver/kmd/mmio.c does: by the address that the configuration header
 * gives for BAR5, and by a known register value read through the access path to that address. The firmware's
 * descriptors only select the path. The memory decode must already be on: this module does not change the
 * function's command register, and it writes no configuration register. Returns 1 when both proofs hold. */
static int bar_identify(struct stage *stage, struct bar_access *a)
{
	const unsigned char *config = stage->gpu_config;
	struct cu_bar_findings f;
	struct cu_regs regs;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge;
	enum cu_bar_control control;
	unsigned value = 0;

	memset(&f, 0, sizeof(f));
	memset(a, 0, sizeof(*a));
	a->stage = stage;
	stage_sayf(stage, CU_NAME, "bar: function %x:%x, command 0x%x, status 0x%x", config16(config, 0),
		   config16(config, 2), config16(config, 4), config16(config, 6));
	stage_sayf(stage, CU_NAME, "bar: configuration BAR0 to BAR5: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		   config32(config, 0x10), config32(config, 0x14), config32(config, 0x18), config32(config, 0x1C),
		   config32(config, 0x20), config32(config, 0x24));
	f.config = cu_bar_config_address(config, STAGE_PCI_CONFIG_BYTES, &f.address);
	stage_sayf(stage, CU_NAME, "bar: %s, address 0x%llx", cu_bar_config_text(f.config), f.address);
	scan_indexes(stage, &f);
	bridge = scan_bridges(stage, &f);

	a->choice = cu_bar_choose(&f);
	if (a->choice.path == CU_BAR_PATH_BRIDGE) a->bridge = bridge;
	if (a->choice.path == CU_BAR_PATH_PCI_IO)
		stage_sayf(stage, CU_NAME, "bar: %s: BarIndex %u", cu_bar_reason_text(a->choice.reason), a->choice.index);
	else if (a->choice.path == CU_BAR_PATH_BRIDGE)
		stage_sayf(stage, CU_NAME, "bar: %s: root bridge %u", cu_bar_reason_text(a->choice.reason), a->choice.bridge);
	else {
		stage_sayf(stage, CU_NAME, "bar: not identified: %s", cu_bar_reason_text(a->choice.reason));
		return 0;
	}

	/* The positive control, before any write: a register whose value is known from Linux on this unit. */
	bar_regs(a, &regs);
	control = cu_bar_control_read(&regs, &value);
	stage_sayf(stage, CU_NAME, "bar: control %s at BAR5 + 0x%x reads 0x%x, known 0x%x: %s", CU_BAR_CONTROL_NAME,
		   cu_bar_control_offset(), value, CU_BAR_CONTROL_VALUE, cu_bar_control_text(control));
	if (control != CU_BAR_CONTROL_OK) {
		a->choice.path = CU_BAR_PATH_NONE;
		return 0;
	}
	stage_sayf(stage, CU_NAME, "bar: the register BAR is identified at 0x%llx", a->choice.address);
	return 1;
}

static int op_unlock(void *context)
{
	struct stage *stage = ((struct unlock *)context)->stage;
	struct bar_access access;
	struct cu_regs regs;
	struct cu_unlock_report report;
	if (stage->gpu == NULL || !bar_identify(stage, &access)) return -1;
	bar_regs(&access, &regs);
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
	struct bar_access access;
	if (!stage_board_bc250(stage) || stage->gpu == NULL) {
		stage_say(stage, CU_NAME, "probe: not a BC-250, no register access");
		return;
	}
	stage_say(stage, CU_NAME, bar_identify(stage, &access) ? "probe: the register BAR is identified, no message sent"
							      : "probe: the register BAR is not identified, no message sent");
}

const struct stage_module cu_module = { CU_NAME, "unlock", cu_module_run, cu_module_probe, NULL, NULL, NULL };
