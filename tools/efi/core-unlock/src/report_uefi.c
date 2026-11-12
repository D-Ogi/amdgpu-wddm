/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The UEFI binding of the read-only report (report_uefi.h). The register reads use two instructions, CPUID and
 * RDMSR, and cu_msr.c decides which register a processor may read. This file has no WRMSR instruction, and no
 * other file of the image has one.
 *
 * The samples of the application processors run through MP Services StartupThisAP, one processor at a time, in
 * blocking mode. The procedure that runs there calls no UEFI service.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "cu_msr.h"
#include "cu_report.h"
#include "cu_text.h"
#include "report_uefi.h"

#define REPORT_AP_TIMEOUT_US	1000000u	/* one processor's sample is a few CPUID and RDMSR instructions */
#define REPORT_LINE_CHARS	240u

static const EFI_GUID mp_services_guid = EFI_MP_SERVICES_PROTOCOL_GUID;
static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static struct cu_report_input input;

static void x86_cpuid(unsigned function, struct cu_cpuid *out)
{
	unsigned a, b, c, d;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(function), "c"(0u));
	out->eax = a;
	out->ebx = b;
	out->ecx = c;
	out->edx = d;
}

static unsigned long long x86_rdmsr(unsigned number)
{
	unsigned low, high;
	__asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(number));
	return ((unsigned long long)high << 32) | low;
}

static VOID EFIAPI ap_sample(VOID *argument)
{
	cu_cpu_sample(x86_cpuid, x86_rdmsr, argument);
}

static void collect_processors(EFI_BOOT_SERVICES *bs, struct cu_report_input *in)
{
	EFI_MP_SERVICES_PROTOCOL *mp = NULL;
	UINTN all = 0, enabled = 0, bsp = 0, n;
	EFI_STATUS status = bs->LocateProtocol(&mp_services_guid, NULL, (VOID **)&mp);
	if (!EFI_ERROR(status) && mp != NULL) status = mp->GetNumberOfProcessors(mp, &all, &enabled);
	if (EFI_ERROR(status) || mp == NULL) {
		in->mp_status = EFI_ERROR(status) ? status : EFI_NOT_FOUND;
		in->bsp_only_sampled = 1;
		in->cpus = 1;
		in->cpu[0].number = 0;
		in->cpu[0].sampled = 1;
		cu_cpu_sample(x86_cpuid, x86_rdmsr, &in->cpu[0].sample);
		return;
	}
	in->mp_present = 1;
	in->mp_all = (unsigned)all;
	in->mp_enabled = (unsigned)enabled;
	if (EFI_ERROR(mp->WhoAmI(mp, &bsp))) bsp = 0;
	for (n = 0; n < all; n++) {
		struct cu_report_cpu *c;
		EFI_PROCESSOR_INFORMATION info;
		if (in->cpus >= CU_REPORT_MAX_CPUS) {
			in->cpus_dropped++;
			continue;
		}
		c = &in->cpu[in->cpus++];
		c->number = (unsigned)n;
		memset(&info, 0, sizeof(info));
		c->info_status = mp->GetProcessorInfo(mp, n, &info);
		c->info_ok = !EFI_ERROR(c->info_status);
		if (c->info_ok) {
			c->processor_id = info.ProcessorId;
			c->status_flag = info.StatusFlag;
			c->package = info.Location.Package;
			c->core = info.Location.Core;
			c->thread = info.Location.Thread;
		}
		if (n == bsp) {
			cu_cpu_sample(x86_cpuid, x86_rdmsr, &c->sample);
			c->sampled = 1;
		} else if (c->info_ok && (info.StatusFlag & PROCESSOR_ENABLED_BIT)) {
			c->sample_status = mp->StartupThisAP(mp, ap_sample, n, NULL, REPORT_AP_TIMEOUT_US, &c->sample, NULL);
			c->sampled = !EFI_ERROR(c->sample_status);
		}
	}
}

static void collect_graphics(EFI_BOOT_SERVICES *bs, struct cu_report_input *in)
{
	EFI_HANDLE *handles = NULL;
	UINTN count = 0, h;
	if (EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &gop_guid, NULL, &count, &handles))) return;
	for (h = 0; h < count; h++) {
		EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
		struct cu_report_gop *g;
		UINT32 m;
		if (EFI_ERROR(bs->HandleProtocol(handles[h], &gop_guid, (VOID **)&gop)) || gop == NULL || gop->Mode == NULL)
			continue;
		if (in->gops >= CU_REPORT_MAX_GOPS) {
			in->gops_dropped++;
			continue;
		}
		g = &in->gop[in->gops++];
		g->max_mode = gop->Mode->MaxMode;
		g->current = gop->Mode->Mode;
		for (m = 0; m < g->max_mode; m++) {
			EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
			UINTN size = 0;
			struct cu_report_mode *r;
			if (g->modes >= CU_REPORT_MAX_MODES) {
				g->dropped++;
				continue;
			}
			r = &g->mode[g->modes++];
			r->number = m;
			if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info)) || info == NULL ||
			    size < sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION))
				continue;
			r->ok = 1;
			r->width = info->HorizontalResolution;
			r->height = info->VerticalResolution;
			r->pixel_format = (unsigned)info->PixelFormat;
			r->pixels_per_line = info->PixelsPerScanLine;
			r->mask[0] = info->PixelInformation.RedMask;
			r->mask[1] = info->PixelInformation.GreenMask;
			r->mask[2] = info->PixelInformation.BlueMask;
			r->mask[3] = info->PixelInformation.ReservedMask;
			bs->FreePool(info);
		}
	}
	if (handles != NULL) bs->FreePool(handles);
}

static void say_lines(const struct report_uefi *r, const char *text, size_t chars)
{
	char line[REPORT_LINE_CHARS + 1u];
	size_t at = 0;
	while (at < chars) {
		size_t n = 0;
		while (at < chars && text[at] != '\n') {
			if (n < REPORT_LINE_CHARS) line[n++] = text[at];
			at++;
		}
		at++;
		line[n] = 0;
		r->say(r->say_context, line);
	}
}

/* Writes the whole text to the report file, in place of an earlier report. 0 when it is written. */
static int write_file(const struct report_uefi *r, char *text, size_t chars, EFI_STATUS *status)
{
	EFI_FILE_PROTOCOL *f = NULL;
	UINTN bytes = chars;
	*status = EFI_NOT_READY;
	if (r->root == NULL || r->path == NULL) return -1;
	/* An earlier report could be longer than this one, so it goes first. Delete closes the handle. */
	if (!EFI_ERROR(r->root->Open(r->root, &f, r->path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && f != NULL)
		(void)f->Delete(f);
	f = NULL;
	*status = r->root->Open(r->root, &f, r->path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if (EFI_ERROR(*status) || f == NULL) return -1;
	*status = f->Write(f, &bytes, text);
	if (!EFI_ERROR(*status)) *status = f->Flush(f);
	f->Close(f);
	return (EFI_ERROR(*status) || bytes != chars) ? -1 : 0;
}

void report_uefi_run(const struct report_uefi *r)
{
	EFI_BOOT_SERVICES *bs = r->st->BootServices;
	char *text = NULL, line[REPORT_LINE_CHARS];
	size_t chars;
	int truncated = 0;
	EFI_STATUS status;

	memset(&input, 0, sizeof(input));
	input.version = r->version;
	input.acpi = r->acpi;
	collect_processors(bs, &input);
	collect_graphics(bs, &input);
	if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, CU_REPORT_BYTES, (VOID **)&text)) || text == NULL) {
		r->say(r->say_context, "report: no memory for the text");
		return;
	}
	chars = cu_report_write(&input, text, CU_REPORT_BYTES, &truncated);
	say_lines(r, text, chars);
	if (truncated) r->say(r->say_context, "report: the text is longer than its buffer and ends early");
	if (write_file(r, text, chars, &status) == 0)
		r->say(r->say_context, "report: written to " CU_REPORT_NAME " beside this application");
	else {
		cu_format(line, sizeof(line), "report: the file was not written, status 0x%llx", (unsigned long long)status);
		r->say(r->say_context, line);
	}
	bs->FreePool(text);
}
