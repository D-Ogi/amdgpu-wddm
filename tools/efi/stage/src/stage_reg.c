/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The register BAR through the firmware (stage_reg.h). */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "stage_bar.h"
#include "stage_reg.h"
#include "bc250_mmio_bar.h"

static int path_read32(struct stage *stage, const struct stage_bar_choice *c, EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge,
		       unsigned offset, unsigned *value)
{
	EFI_STATUS status;
	UINT32 v = 0;
	if ((offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	if (c->path == STAGE_BAR_PATH_PCI_IO)
		status = stage->gpu->Mem.Read(stage->gpu, EfiPciIoWidthUint32, (UINT8)c->index, offset, 1, &v);
	else if (c->path == STAGE_BAR_PATH_BRIDGE && bridge != NULL)
		status = bridge->Mem.Read(bridge, EfiPciIoWidthUint32, c->address + offset, 1, &v);
	else
		return -1;
	if (EFI_ERROR(status)) return -1;
	*value = v;
	return 0;
}

int stage_reg_read32(struct stage *stage, unsigned offset, unsigned *value)
{
	if (!stage->reg_identified) return -1;
	return path_read32(stage, &stage->reg_choice, stage->reg_bridge, offset, value);
}

int stage_reg_write32(struct stage *stage, unsigned offset, unsigned value)
{
	EFI_STATUS status;
	UINT32 v = value;
	if (!stage->reg_identified || (offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	if (stage->reg_choice.path == STAGE_BAR_PATH_PCI_IO)
		status = stage->gpu->Mem.Write(stage->gpu, EfiPciIoWidthUint32, (UINT8)stage->reg_choice.index, offset, 1, &v);
	else if (stage->reg_choice.path == STAGE_BAR_PATH_BRIDGE && stage->reg_bridge != NULL)
		status = stage->reg_bridge->Mem.Write(stage->reg_bridge, EfiPciIoWidthUint32, stage->reg_choice.address + offset,
						      1, &v);
	else
		return -1;
	return EFI_ERROR(status) ? -1 : 0;
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
static void dump_list(struct stage *stage, const char *module, const char *what, const struct stage_bar_list *list)
{
	unsigned i;
	for (i = 0; i < list->count; i++) {
		const struct stage_bar_resource *r = &list->resource[i];
		if (!r->address_space) {
			stage_sayf(stage, module, "bar: %s descriptor %u: tag 0x%x, %u bytes, not an address space descriptor",
				   what, i, r->tag, r->bytes);
			continue;
		}
		stage_sayf(stage, module, "bar: %s descriptor %u: tag 0x%x, %u bytes, type %u, general 0x%x, specific 0x%x, "
			   "granularity 0x%llx", what, i, r->tag, r->bytes, r->type, r->general, r->specific, r->granularity);
		stage_sayf(stage, module, "bar: %s descriptor %u: minimum 0x%llx, maximum 0x%llx, translation 0x%llx, "
			   "length 0x%llx", what, i, r->minimum, r->maximum, r->translation, r->length);
	}
	if (list->complete)
		stage_sayf(stage, module, "bar: %s: %u descriptors, end tag at byte %u", what, list->seen, list->end_offset);
	else
		stage_sayf(stage, module, "bar: %s: %u descriptors, no end tag within %u bytes", what, list->seen,
			   STAGE_BAR_LIST_MAX_BYTES);
}

/* GetBarAttributes for each BarIndex of a type 0 header. UEFI 2.10 section 14.4 calls BarIndex "the BAR index of
 * the standard PCI Configuration header", but the reference implementation (EDK2 PciBusDxe, PciParseBar) counts
 * a 64-bit BAR as one index, so BarIndex and the dword of the header are not the same thing on this GPU. Only the
 * descriptor that names the address of BAR5 selects an index. */
static void scan_indexes(struct stage *stage, const char *module, struct stage_bar_findings *f)
{
	unsigned i;
	for (i = 0; i < STAGE_BAR_INDEXES; i++) {
		char what[24];
		UINT64 supports = 0;
		VOID *resources = NULL;
		struct stage_bar_list list;
		EFI_STATUS status = stage->gpu->GetBarAttributes(stage->gpu, (UINT8)i, &supports, &resources);
		f->index[i] = STAGE_BAR_MATCH_NONE;
		stage_sayf(stage, module, "bar: BarIndex %u: status 0x%llx, supports 0x%llx, resources %s", i,
			   (unsigned long long)status, (unsigned long long)supports,
			   EFI_ERROR(status) || resources == NULL ? "none" : "given");
		if (EFI_ERROR(status) || resources == NULL) continue;
		stage_bar_parse(resources, STAGE_BAR_LIST_MAX_BYTES, &list);
		efl_format(what, sizeof(what), "BarIndex %u", i);
		dump_list(stage, module, what, &list);
		if (f->config == STAGE_BAR_CONFIG_OK) f->index[i] = stage_bar_match(&list, f->address, BC250_BAR5_LENGTH);
		stage->bs->FreePool(resources);
	}
}

/* The root bridges of the GPU function's segment, with their windows. Returns the one that stage_bar_choose can
 * select: the only one that holds BAR5, else the only one of the segment. */
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *scan_bridges(struct stage *stage, const char *module,
						     struct stage_bar_findings *f)
{
	static const EFI_GUID bridge_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *only = NULL, *holding = NULL;
	EFI_HANDLE *handles = NULL;
	UINTN count = 0, i, segment = 0, bus = 0, device = 0, function = 0;
	unsigned only_index = 0, holding_index = 0;
	if (EFI_ERROR(stage->gpu->GetLocation(stage->gpu, &segment, &bus, &device, &function))) {
		stage_say(stage, module, "bar: the GPU function has no location: no root bridge");
		return NULL;
	}
	if (EFI_ERROR(stage->bs->LocateHandleBuffer(ByProtocol, (EFI_GUID *)&bridge_guid, NULL, &count, &handles)))
		count = 0;
	stage_sayf(stage, module, "bar: %u root bridges, the GPU function is on segment %u", (unsigned)count,
		   (unsigned)segment);
	for (i = 0; i < count && f->bridges < STAGE_BAR_MAX_BRIDGES; i++) {
		EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge = NULL;
		VOID *resources = NULL;
		struct stage_bar_list list;
		EFI_STATUS status;
		char what[24];
		if (EFI_ERROR(stage->bs->HandleProtocol(handles[i], (EFI_GUID *)&bridge_guid, (VOID **)&bridge)) ||
		    bridge == NULL)
			continue;
		if (bridge->SegmentNumber != segment) {
			stage_sayf(stage, module, "bar: root bridge %u: segment %u, not the GPU function's", (unsigned)i,
				   (unsigned)bridge->SegmentNumber);
			continue;
		}
		f->bridges++;
		only = bridge;
		only_index = (unsigned)i;
		status = bridge->Configuration(bridge, &resources);
		stage_sayf(stage, module, "bar: root bridge %u: segment %u, configuration status 0x%llx", (unsigned)i,
			   (unsigned)bridge->SegmentNumber, (unsigned long long)status);
		if (EFI_ERROR(status) || resources == NULL) continue;
		/* The buffer belongs to the firmware and is not freed here. */
		stage_bar_parse(resources, STAGE_BAR_LIST_MAX_BYTES, &list);
		efl_format(what, sizeof(what), "root bridge %u", (unsigned)i);
		dump_list(stage, module, what, &list);
		if (list.complete) f->bridges_described++;
		if (f->config == STAGE_BAR_CONFIG_OK && stage_bar_window_holds(&list, f->address, BC250_BAR5_LENGTH)) {
			f->bridges_holding++;
			holding = bridge;
			holding_index = (unsigned)i;
		}
	}
	if (handles != NULL) stage->bs->FreePool(handles);
	f->bridge = f->bridges_holding == 1u ? holding_index : only_index;
	return f->bridges_holding == 1u ? holding : only;
}

struct control_path {
	struct stage *stage;
	const struct stage_bar_choice *choice;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge;
};

static int control_read32(void *context, unsigned offset, unsigned *value)
{
	struct control_path *p = context;
	return path_read32(p->stage, p->choice, p->bridge, offset, value);
}

/* The register BAR is identified twice, as driver/kmd/mmio.c does: by the address that the configuration header
 * gives for BAR5, and by a known register value read through the access path to that address. The firmware's
 * descriptors only select the path. The memory decode must already be on: this code does not change the
 * function's command register, and it writes no configuration register. */
int stage_reg_identify(struct stage *stage, const char *module)
{
	const unsigned char *config = stage->gpu_config;
	struct stage_bar_findings f;
	struct stage_bar_choice choice;
	struct control_path path;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge;
	enum stage_bar_control control;
	unsigned value = 0;

	stage->reg_identified = 0;
	stage->reg_bridge = NULL;
	memset(&stage->reg_choice, 0, sizeof(stage->reg_choice));
	if (stage->gpu == NULL) return 0;
	memset(&f, 0, sizeof(f));
	stage_sayf(stage, module, "bar: function %x:%x, command 0x%x, status 0x%x", config16(config, 0),
		   config16(config, 2), config16(config, 4), config16(config, 6));
	stage_sayf(stage, module, "bar: configuration BAR0 to BAR5: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		   config32(config, 0x10), config32(config, 0x14), config32(config, 0x18), config32(config, 0x1C),
		   config32(config, 0x20), config32(config, 0x24));
	f.config = stage_bar_config_address(config, STAGE_PCI_CONFIG_BYTES, &f.address);
	stage_sayf(stage, module, "bar: %s, address 0x%llx", stage_bar_config_text(f.config), f.address);
	scan_indexes(stage, module, &f);
	bridge = scan_bridges(stage, module, &f);

	choice = stage_bar_choose(&f);
	if (choice.path != STAGE_BAR_PATH_BRIDGE) bridge = NULL;
	if (choice.path == STAGE_BAR_PATH_PCI_IO)
		stage_sayf(stage, module, "bar: %s: BarIndex %u", stage_bar_reason_text(choice.reason), choice.index);
	else if (choice.path == STAGE_BAR_PATH_BRIDGE)
		stage_sayf(stage, module, "bar: %s: root bridge %u", stage_bar_reason_text(choice.reason), choice.bridge);
	else {
		stage_sayf(stage, module, "bar: not identified: %s", stage_bar_reason_text(choice.reason));
		return 0;
	}

	/* The positive control, before any write: a register whose value is known from Linux on this unit. */
	path.stage = stage;
	path.choice = &choice;
	path.bridge = bridge;
	control = stage_bar_control_read(control_read32, &path, &value);
	stage_sayf(stage, module, "bar: control %s at BAR5 + 0x%x reads 0x%x, known 0x%x: %s", STAGE_BAR_CONTROL_NAME,
		   stage_bar_control_offset(), value, STAGE_BAR_CONTROL_VALUE, stage_bar_control_text(control));
	if (control != STAGE_BAR_CONTROL_OK) return 0;
	stage_sayf(stage, module, "bar: the register BAR is identified at 0x%llx", choice.address);
	stage->reg_choice = choice;
	stage->reg_bridge = bridge;
	stage->reg_identified = 1;
	return 1;
}
