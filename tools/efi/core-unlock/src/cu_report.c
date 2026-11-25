/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The text of the read-only report (cu_report.h). */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "cu_report.h"
#include "efl_text.h"

/* The StatusFlag bits of EFI_PROCESSOR_INFORMATION (PI Specification 1.8 volume 2, MP Services). */
#define MP_STATUS_BSP		0x1u
#define MP_STATUS_ENABLED	0x2u
#define MP_STATUS_HEALTHY	0x4u
/* EFI_GRAPHICS_PIXEL_FORMAT (UEFI Specification 2.10 section 12.9). */
static const char *const pixel_formats[] = {
	"PixelRedGreenBlueReserved8BitPerColor", "PixelBlueGreenRedReserved8BitPerColor", "PixelBitMask",
	"PixelBltOnly"
};
#define MADT_FLAG_ENABLED	0x1u

struct sink {
	char *out;
	size_t capacity, used;
	int truncated;
};

static void add(struct sink *s, const char *format, ...)
{
	va_list args;
	size_t room, n;
	if (s->used + 1u >= s->capacity) { s->truncated = 1; return; }
	room = s->capacity - s->used;
	va_start(args, format);
	n = efl_vformat(s->out + s->used, room, format, args);
	va_end(args);
	if (n + 1u >= room) s->truncated = 1;
	s->used += n;
}

/* ---- processors ----------------------------------------------------------------------------------- */

static void processors(struct sink *s, const struct cu_report_input *in)
{
	static struct efl_acpi_madt_cpu madt[EFL_ACPI_MAX_MADT_CPUS];
	unsigned count = 0, dropped = 0, enabled = 0, i;
	add(s, "\n[processors]\n");
	if (in->mp_present)
		add(s, "MP Services: %u logical processors, %u enabled\n", in->mp_all, in->mp_enabled);
	else
		add(s, "MP Services: not present or not answering (status 0x%llx)\n", in->mp_status);
	for (i = 0; in->mp_present && i < in->cpus; i++) {
		const struct cu_report_cpu *c = &in->cpu[i];
		if (!c->info_ok) {
			add(s, "  processor %u: GetProcessorInfo status 0x%llx\n", c->number, c->info_status);
			continue;
		}
		add(s, "  processor %u: APIC ID %llu, package %u core %u thread %u, status 0x%x%s%s%s\n", c->number,
		    c->processor_id, c->package, c->core, c->thread, c->status_flag,
		    (c->status_flag & MP_STATUS_BSP) ? " BSP" : "",
		    (c->status_flag & MP_STATUS_ENABLED) ? " enabled" : " disabled",
		    (c->status_flag & MP_STATUS_HEALTHY) ? " healthy" : " not healthy");
	}
	if (in->cpus_dropped) add(s, "  %u more processors are not listed\n", in->cpus_dropped);
	if (in->acpi == NULL || efl_acpi_madt_list(in->acpi, madt, EFL_ACPI_MAX_MADT_CPUS, &count, &dropped) != EFL_ACPI_OK) {
		add(s, "MADT: not found\n");
		return;
	}
	for (i = 0; i < count; i++)
		if (madt[i].flags & MADT_FLAG_ENABLED) enabled++;
	add(s, "MADT: %u processor entries listed, %u enabled\n", count + dropped, enabled);
	for (i = 0; i < count; i++)
		add(s, "  %s UID %u, APIC ID %u, flags 0x%x%s\n", madt[i].type == 0 ? "Local APIC" : "Local x2APIC",
		    madt[i].uid, madt[i].apic_id, madt[i].flags,
		    (madt[i].flags & MADT_FLAG_ENABLED) ? " enabled" : "");
	if (dropped) add(s, "  %u more entries are not listed\n", dropped);
}

/* ---- ACPI ------------------------------------------------------------------------------------------- */

static int is_aml(const struct efl_acpi_table *t)
{
	return !memcmp(t->signature, "DSDT", 4) || !memcmp(t->signature, "SSDT", 4);
}

void cu_report_power_summary(const struct efl_acpi *acpi, struct cu_report_power_summary *out)
{
	static struct efl_acpi_table tables[EFL_ACPI_MAX_TABLES];
	static struct efl_aml_scan scan;
	unsigned count = 0, dropped = 0, i, j, k;
	memset(out, 0, sizeof(*out));
	if (acpi == NULL || efl_acpi_tables(acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped) != EFL_ACPI_OK) return;
	for (i = 0; i < count; i++) {
		if (!is_aml(&tables[i]) || tables[i].bytes == NULL || !efl_aml_scan(tables[i].bytes, tables[i].length, &scan))
			continue;
		out->tables++;
		out->objects += scan.objects;
		out->processor_objects += scan.processor_objects;
		for (j = 0; j < scan.objects; j++)
			for (k = 0; k < EFL_AML_POWER_NAMES; k++)
				if (scan.object[j].names & (1u << k)) out->with_name[k]++;
	}
}

static void aml_table(struct sink *s, const struct efl_acpi_table *t)
{
	static struct efl_aml_scan scan;
	unsigned i, k;
	add(s, "%s \"%s\" at 0x%llx:", t->signature, t->oem_table_id, t->address);
	if (t->bytes == NULL || !efl_aml_scan(t->bytes, t->length, &scan)) {
		add(s, " not scanned (checksum wrong)\n");
		return;
	}
	add(s, " %u Processor objects, %u ThermalZone objects, %u listed objects", scan.processor_objects,
	    scan.thermal_zones, scan.objects);
	if (scan.dropped) add(s, ", %u more not listed", scan.dropped);
	if (scan.malformed) add(s, ", %u scope openings did not parse", scan.malformed);
	add(s, "\n");
	for (i = 0; i < scan.objects; i++) {
		const struct efl_aml_object *o = &scan.object[i];
		add(s, "  %s (%s):", o->path, efl_aml_kind_text(o->kind));
		if (o->names == 0) add(s, " none of the names");
		for (k = 0; k < EFL_AML_POWER_NAMES; k++)
			if (o->names & (1u << k)) add(s, " %s", efl_aml_power_name_text((enum efl_aml_power_name)k));
		add(s, "\n");
	}
	add(s, "  occurrences of the bytes:");
	for (k = 0; k < EFL_AML_COUNTS; k++) add(s, " %s %u", efl_aml_count_text((enum efl_aml_count)k), scan.counts[k]);
	add(s, "\n");
}

static void acpi_part(struct sink *s, const struct efl_acpi *acpi)
{
	static struct efl_acpi_table tables[EFL_ACPI_MAX_TABLES];
	struct cu_report_power_summary summary;
	struct efl_acpi_fadt fadt;
	unsigned count = 0, dropped = 0, i, k;
	enum efl_acpi_result result;

	add(s, "\n[acpi tables]\n");
	result = acpi == NULL ? EFL_ACPI_NO_RSDP : efl_acpi_tables(acpi, tables, EFL_ACPI_MAX_TABLES, &count, &dropped);
	if (result != EFL_ACPI_OK) {
		add(s, "no RSDT or XSDT (reason %u)\n", (unsigned)result);
		return;
	}
	for (i = 0; i < count; i++)
		add(s, "  %s OEM \"%s\" table \"%s\" revision %u, %u bytes, at 0x%llx, checksum %s\n", tables[i].signature,
		    tables[i].oem_id, tables[i].oem_table_id, tables[i].revision, tables[i].length, tables[i].address,
		    tables[i].checksum_ok ? "right" : "wrong");
	if (dropped) add(s, "  %u entries did not map or are not listed\n", dropped);

	add(s, "\n[acpi names]\n");
	add(s, "This part is a scan of the names in the AML bytes. It is not an AML interpretation: it executes no "
	       "method and resolves no reference. A name counts for the scope whose opening encloses its Name or "
	       "Method definition.\n");
	for (i = 0; i < count; i++)
		if (is_aml(&tables[i])) aml_table(s, &tables[i]);
	cu_report_power_summary(acpi, &summary);
	add(s, "summary over %u tables: %u Processor objects, %u listed objects; objects that define",
	    summary.tables, summary.processor_objects, summary.objects);
	for (k = 0; k < EFL_AML_POWER_NAMES; k++)
		add(s, " %s %u", efl_aml_power_name_text((enum efl_aml_power_name)k), summary.with_name[k]);
	add(s, "\n");

	add(s, "\n[fadt]\n");
	if (efl_acpi_fadt_fields(acpi, &fadt) != EFL_ACPI_OK) {
		add(s, "no FADT with a right checksum\n");
		return;
	}
	add(s, "revision %u, %u bytes, flags 0x%x, P_LVL2_LAT %u us, P_LVL3_LAT %u us\n", fadt.revision, fadt.length,
	    fadt.flags, fadt.p_lvl2_lat, fadt.p_lvl3_lat);
	if (fadt.has_reset)
		add(s, "RESET_REG: space %u, bit width %u, bit offset %u, access size %u, address 0x%llx; RESET_VALUE 0x%x\n",
		    fadt.reset_space, fadt.reset_width, fadt.reset_offset, fadt.reset_access, fadt.reset_address,
		    fadt.reset_value);
	else
		add(s, "the FADT ends before RESET_REG\n");
}

/* ---- registers ---------------------------------------------------------------------------------------- */

static void sample(struct sink *s, const struct cu_report_cpu *c)
{
	const struct cu_cpu_sample *m = &c->sample;
	unsigned i;
	add(s, "processor %u:", c->number);
	if (!c->sampled && c->info_ok && !(c->status_flag & MP_STATUS_ENABLED)) {
		add(s, " not read, the processor is disabled\n");
		return;
	}
	if (!c->sampled && !c->info_ok) {
		add(s, " not read, GetProcessorInfo did not answer\n");
		return;
	}
	if (!c->sampled) {
		add(s, " not read (StartupThisAP status 0x%llx)\n", c->sample_status);
		return;
	}
	add(s, " %s, family 0x%x model 0x%x stepping 0x%x, LocalApicId %u", m->id.amd ? "AuthenticAMD" : "not AMD",
	    m->id.family, m->id.model, m->id.stepping, m->id.apic_id);
	if (m->id.apm_info) add(s, ", HwPstate %u, CPB %u", (unsigned)m->id.hw_pstate, (unsigned)m->id.cpb);
	add(s, "\n");
	if (m->read == 0) {
		add(s, "  no register read: the processor is not AMD family 17h\n");
		return;
	}
	for (i = 0; i < CU_MSR_I_COUNT; i++) {
		const struct cu_msr_def *d = cu_msr_def((enum cu_msr_index)i);
		unsigned long long v = m->value[i];
		if (!(m->read & (1u << i))) {
			add(s, "  %s (MSR 0x%x): not read, the CPUID check does not allow it\n", d->name, d->number);
			continue;
		}
		add(s, "  %s (MSR 0x%x) = 0x%llx", d->name, d->number, v);
		if (i == CU_MSR_I_HWCR) add(s, ": CpbDis %u", cu_hwcr_cpb_dis(v));
		else if (i == CU_MSR_I_PSTATE_CUR_LIM)
			add(s, ": PstateMaxVal %u, CurPstateLimit %u", cu_pstate_max_val(v), cu_cur_pstate_limit(v));
		else if (i == CU_MSR_I_PSTATE_CTL) add(s, ": PstateCmd %u", cu_pstate_low3(v));
		else if (i == CU_MSR_I_PSTATE_STAT) add(s, ": CurPstate %u", cu_pstate_low3(v));
		else if (i == CU_MSR_I_CSTATE_BASE_ADDR) add(s, ": CstateAddr 0x%x", cu_cstate_addr(v));
		else {
			struct cu_pstate_def p;
			cu_pstate_def_decode(v, &p);
			add(s, ": PstateEn %u, CpuFid 0x%x, CpuDfsId 0x%x, CpuVid 0x%x, IddValue 0x%x, IddDiv %u, CoreCOF %u MHz",
			    p.enabled, p.cpu_fid, p.cpu_dfs_id, p.cpu_vid, p.idd_value, p.idd_div, p.core_cof_mhz);
		}
		add(s, "\n");
	}
}

static void registers(struct sink *s, const struct cu_report_input *in)
{
	unsigned i;
	add(s, "\n[msr]\n");
	add(s, "Read only. Source of the numbers and fields: AMD PPR 56176 revision 3.06, family 17h model 71h. Each "
	       "processor reads only when CPUID says AuthenticAMD and family 17h; PStateCurLim, PStateCtl and "
	       "PStateStat also need CPUID Fn8000_0007_EDX[HwPstate].\n");
	if (in->bsp_only_sampled) add(s, "no MP Services: the BSP only\n");
	for (i = 0; i < in->cpus; i++) sample(s, &in->cpu[i]);
	add(s, "open: the C-state configuration registers are not in the public PPR 56176, so the report does not read "
	       "them\n");
}

/* ---- graphics ----------------------------------------------------------------------------------------- */

static void graphics(struct sink *s, const struct cu_report_input *in)
{
	unsigned g, i;
	add(s, "\n[gop]\n");
	if (in->gops == 0) add(s, "no Graphics Output Protocol\n");
	for (g = 0; g < in->gops; g++) {
		const struct cu_report_gop *p = &in->gop[g];
		add(s, "GOP %u: mode %u of %u modes\n", g, p->current, p->max_mode);
		for (i = 0; i < p->modes; i++) {
			const struct cu_report_mode *m = &p->mode[i];
			if (!m->ok) {
				add(s, "  mode %u: QueryMode failed\n", m->number);
				continue;
			}
			add(s, "  mode %u: %ux%u, %s, %u pixels per scan line%s", m->number, m->width, m->height,
			    m->pixel_format < 4u ? pixel_formats[m->pixel_format] : "unknown pixel format",
			    m->pixels_per_line, m->number == p->current ? " (current)" : "");
			if (m->pixel_format == 2u)
				add(s, ", masks 0x%x 0x%x 0x%x 0x%x", m->mask[0], m->mask[1], m->mask[2], m->mask[3]);
			add(s, "\n");
		}
		if (p->dropped) add(s, "  %u more modes are not listed\n", p->dropped);
	}
	if (in->gops_dropped) add(s, "%u more GOP instances are not listed\n", in->gops_dropped);
}

size_t cu_report_write(const struct cu_report_input *in, char *out, size_t capacity, int *truncated)
{
	struct sink s;
	s.out = out;
	s.capacity = capacity;
	s.used = 0;
	s.truncated = 0;
	if (out == NULL || capacity == 0) {
		if (truncated) *truncated = 1;
		return 0;
	}
	out[0] = 0;
	add(&s, "core-unlock report, version %s\n", in->version ? in->version : "?");
	add(&s, "This report only reads. It changes no setting and no register, and the decision of core-unlock does "
		"not use it.\n");
	processors(&s, in);
	acpi_part(&s, in->acpi);
	registers(&s, in);
	graphics(&s, in);
	if (truncated) *truncated = s.truncated;
	return s.used;
}
