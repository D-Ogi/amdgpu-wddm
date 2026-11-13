/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Secure Boot state that the operating system reads (sb_windows.h, docs/design/efi-resident-stage.md).
 *
 * The two functions sb_rt_notify and sb_rt_filter run after the stage returns: the firmware frees the image of the
 * stage at that moment. They are therefore copied into memory of the type EfiRuntimeServicesCode, and the state they
 * read is in memory of the type EfiRuntimeServicesData. The filter finds the state through one 64-bit slot in the
 * copied code, which the install code replaces with the address of the state. The notify converts the pointers at
 * the virtual address change event (UEFI 2.10, EFI_EVENT_SIGNAL_VIRTUAL_ADDRESS_CHANGE) and writes the converted
 * state address into the slot again.
 *
 * The copied code calls nothing of the image: no string literal, no global, no libc, and the table CRC is computed
 * by a loop of its own. That is what makes the copy safe: a reference that the copy leaves behind would point into
 * freed memory.
 *
 * sb.windows=1 is honoured only together with sb.mode=enforce: the stage really verifies the boot manager against db
 * and dbx in that mode. No SetVariable call exists: the state is served from memory for this start only.
 */
#include <stddef.h>
#include "efi.h"
#include "sb_crc32.h"
#include "sb_windows.h"
#include "stage.h"

/* The state address lives in a variable of the copied range itself. A literal address in the code is not enough: the
 * compiler folds the offset of a field into it (it emitted the address of st->original as slot+16 in one build), and
 * a patched literal then leaves the other sites behind. A variable in the same section is copied with the code, and
 * the code reaches it with a displacement that stays right after the copy. */
#define SB_RT_MAGIC		0x5342525453544154ull
#define SB_RT_CODE_MAX		0x1000u		/* one page: the two functions are far smaller */
/* The attributes and the event values, from edk2 commit 8628578dacd3: MdePkg\Include\Uefi\UefiMultiPhase.h
 * (BOOTSERVICE_ACCESS 0x2, RUNTIME_ACCESS 0x4, TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x20) and
 * MdePkg\Include\Uefi\UefiSpec.h (EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE 0x60000202, TPL_NOTIFY 16). */
#define SB_RT_ACCEPTED_ATTR	0x00000027u	/* BOOTSERVICE|RUNTIME|TIME_BASED_AUTHENTICATED_WRITE_ACCESS */
#define SB_RT_GLOBAL_ATTR	0x00000006u	/* BOOTSERVICE|RUNTIME */
#define SB_RT_TPL_NOTIFY	16u

/* The copied code carries no instrumentation: a sanitizer call is a call into a library that the firmware does not
 * have, and the host test must run the same bytes as the image. The letter orders the pieces of the section: COFF
 * places the parts of one section in the order of their names, and the copy needs the two functions inside the range
 * of the two markers (a linker that splits them by usage would put a function outside it). */
#define SB_RT_TEXT(o)	__attribute__((section(".sb_rt$" #o), noinline, used, aligned(16), \
				       no_sanitize("address", "undefined")))

struct sb_rt_state {
	UINT64 magic;
	EFI_RUNTIME_SERVICES *rs;
	EFI_STATUS (EFIAPI *original)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attributes, UINTN *size, VOID *data);
	const unsigned char *pk, *kek, *db, *dbx;
	UINTN pk_bytes, kek_bytes, db_bytes, dbx_bytes;
};

static struct {
	int installed;
	EFI_EVENT event;
	EFI_RUNTIME_SERVICES *rs;
	EFI_STATUS (EFIAPI *original)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attributes, UINTN *size, VOID *data);
	void *code;
	void *state;
} sbw;

enum {
	SB_W_NAMES_GLOBAL = 0,
	SB_W_NAMES_DATABASE = 1,
	SB_W_NAMES_COUNT = 2
};

/* ---- the copied code ----------------------------------------------------------------------------- */

/* A name of the UEFI variable interface, compared without a call and without a literal. */
static __attribute__((always_inline)) inline int rt_name_is(const CHAR16 *name, const char *ascii)
{
	size_t i = 0;
	if (name == NULL) return 0;
	for (;;) {
		if ((unsigned char)name[i] != (unsigned char)ascii[i]) return 0;
		if (ascii[i] == 0) return 1;
		i++;
	}
}

/* EFI_GLOBAL_VARIABLE {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}, edk2 8628578dacd3, MdePkg\Include\Guid\GlobalVariable.h.
 * The names of that namespace are SecureBoot, SetupMode, PK (EFI_PLATFORM_KEY_NAME) and KEK
 * (EFI_KEY_EXCHANGE_KEY_NAME); the values 1 and 0 are SECURE_BOOT_MODE_ENABLE and USER_MODE of
 * MdePkg\Include\Guid\ImageAuthentication.h. */
static __attribute__((always_inline)) inline int rt_is_global(const EFI_GUID *g)
{
	return g != NULL && g->Data1 == 0x8BE4DF61u && g->Data2 == 0x93CAu && g->Data3 == 0x11D2u &&
	       g->Data4[0] == 0xAA && g->Data4[1] == 0x0D && g->Data4[2] == 0x00 && g->Data4[3] == 0xE0 &&
	       g->Data4[4] == 0x98 && g->Data4[5] == 0x03 && g->Data4[6] == 0x2B && g->Data4[7] == 0x8C;
}

/* EFI_IMAGE_SECURITY_DATABASE_GUID {D719B2CB-3D3A-4596-A3BC-DAD00E67656F}, edk2 8628578dacd3,
 * MdePkg\Include\Guid\ImageAuthentication.h; the same value as the database_guid of sb_module.c. */
static __attribute__((always_inline)) inline int rt_is_database(const EFI_GUID *g)
{
	return g != NULL && g->Data1 == 0xD719B2CBu && g->Data2 == 0x3D3Au && g->Data3 == 0x4596u &&
	       g->Data4[0] == 0xA3 && g->Data4[1] == 0xBC && g->Data4[2] == 0xDA && g->Data4[3] == 0xD0 &&
	       g->Data4[4] == 0x0E && g->Data4[5] == 0x67 && g->Data4[6] == 0x65 && g->Data4[7] == 0x6F;
}

static __attribute__((always_inline)) inline void rt_copy(void *dst, const void *src, UINTN bytes)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	UINTN i;
	for (i = 0; i < bytes; i++) d[i] = s[i];
}

/* The answer of one served variable. UEFI 2.10 GetVariable: with no buffer, or a buffer that is too small, the
 * answer is EFI_BUFFER_TOO_SMALL with the size the caller needs. */
static __attribute__((always_inline)) inline EFI_STATUS rt_serve(const unsigned char *data_bytes, UINTN bytes,
								 UINT32 attributes, UINT32 *attributes_out,
								 UINTN *size, VOID *data)
{
	if (size == NULL) return EFI_INVALID_PARAMETER;
	if (data == NULL || *size < bytes) {
		*size = bytes;
		if (attributes_out != NULL) *attributes_out = attributes;
		return EFI_BUFFER_TOO_SMALL;
	}
	rt_copy(data, data_bytes, bytes);
	*size = bytes;
	if (attributes_out != NULL) *attributes_out = attributes;
	return EFI_SUCCESS;
}

/* The CRC-32 of the runtime table, as CalculateCrc32 computes it: the field is zero while the sum is made. Inline in
 * the copied code: a call would leave the copy and land in the freed image. */
static __attribute__((always_inline, no_sanitize("address", "undefined"))) inline void rt_table_crc(EFI_RUNTIME_SERVICES *rs)
{
	unsigned int crc = 0xFFFFFFFFu;
	const unsigned char *p = (const unsigned char *)rs;
	UINTN i, bytes = (UINTN)rs->Hdr.HeaderSize;
	int bit;
	rs->Hdr.CRC32 = 0;
	for (i = 0; i < bytes; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	rs->Hdr.CRC32 = crc ^ 0xFFFFFFFFu;
}

/* The first byte of the copied range. The install code copies from here to sb_rt_end and writes the address of the
 * state in the eight bytes behind sb_rt_end; the copy carries that slot with it. The code reaches the slot through
 * the addresses of the two markers: their difference is a link-time constant, and the displacement of each marker
 * stays right after the copy. */
SB_RT_TEXT(a) static VOID EFIAPI sb_rt_begin(VOID)
{
}

static VOID EFIAPI sb_rt_end(VOID);

#ifdef SB_RT_IN_PLACE
/* The host test runs this code in place: the slot is a variable of the image, because there is no copy behind it. */
static UINT64 sb_rt_slot_in_place;
#endif

/* The slot that holds the address of the state. In the image it lies behind the range of the two markers; in the
 * copy it is the eight bytes that the install code writes behind the copied range. A call would leave the copy, so
 * this is inline. */
static __attribute__((always_inline)) inline UINT64 *rt_slot_ptr(VOID)
{
#ifdef SB_RT_IN_PLACE
	return &sb_rt_slot_in_place;
#else
	UINT64 start = (UINT64)(UINTN)&sb_rt_begin;
	return (UINT64 *)(UINTN)(start + ((UINT64)(UINTN)&sb_rt_end - start));
#endif
}

static __attribute__((always_inline)) inline struct sb_rt_state *rt_state(VOID)
{
	return (struct sb_rt_state *)(UINTN)*(volatile UINT64 *)rt_slot_ptr();
}

/* The virtual address change event: every pointer of the state is converted, then the slot behind the copy gets the
 * converted address of the state. The physical addresses are still valid while the event runs. */
SB_RT_TEXT(b) static VOID EFIAPI sb_rt_notify(EFI_EVENT event, VOID *context)
{
	struct sb_rt_state *st = rt_state();
	struct sb_rt_state *converted = st;
	EFI_RUNTIME_SERVICES *rs;
	VOID *p;
	(void)event;
	(void)context;
	if (st == NULL || st->magic != SB_RT_MAGIC) return;
	rs = st->rs;
	if (rs == NULL || rs->ConvertPointer == NULL) return;
	p = (VOID *)st;
	(void)rs->ConvertPointer(0, &p);
	converted = (struct sb_rt_state *)p;
	if (converted == NULL) return;
	p = converted->rs;
	(void)rs->ConvertPointer(0, &p);
	converted->rs = (EFI_RUNTIME_SERVICES *)p;
	p = (VOID *)converted->original;
	(void)rs->ConvertPointer(0, &p);
	converted->original = (EFI_STATUS (EFIAPI *)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, VOID *))p;
	p = (VOID *)converted->pk;
	(void)rs->ConvertPointer(0, &p);
	converted->pk = (const unsigned char *)p;
	p = (VOID *)converted->kek;
	(void)rs->ConvertPointer(0, &p);
	converted->kek = (const unsigned char *)p;
	p = (VOID *)converted->db;
	(void)rs->ConvertPointer(0, &p);
	converted->db = (const unsigned char *)p;
	p = (VOID *)converted->dbx;
	(void)rs->ConvertPointer(0, &p);
	converted->dbx = (const unsigned char *)p;
	/* The slot holds the converted address of the state from here on. */
	*(volatile UINT64 *)rt_slot_ptr() = (UINT64)(UINTN)converted;
	rt_table_crc(converted->rs);
}

/* The filter in front of the firmware's GetVariable. It serves the Secure Boot state that the stage has when the
 * firmware has none, and calls the firmware's own function for everything else. */
SB_RT_TEXT(c) static EFI_STATUS EFIAPI sb_rt_filter(CHAR16 *name, EFI_GUID *vendor, UINT32 *attributes, UINTN *size,
						 VOID *data)
{
	struct sb_rt_state *st = rt_state();
	UINTN probe = 0;
	/* The two values live on the stack: a static would be a reference into the image, and the image is freed. */
	unsigned char secure_boot_on = 1;
	unsigned char setup_mode_off = 0;
	if (st == NULL || st->magic != SB_RT_MAGIC || st->original == NULL) return EFI_DEVICE_ERROR;
	if (rt_is_global(vendor)) {
		/* PK and KEK live in this namespace, not in the database one: edk2 8628578dacd3 puts
		 * EFI_PLATFORM_KEY_NAME and EFI_KEY_EXCHANGE_KEY_NAME in Guid\GlobalVariable.h, and the database names
		 * in Guid\ImageAuthentication.h. Windows asks for them with the global GUID. */
		if (rt_name_is(name, "PK"))
			return rt_serve(st->pk, st->pk_bytes, SB_RT_ACCEPTED_ATTR, attributes, size, data);
		if (rt_name_is(name, "KEK"))
			return rt_serve(st->kek, st->kek_bytes, SB_RT_ACCEPTED_ATTR, attributes, size, data);
		if (rt_name_is(name, "SecureBoot") || rt_name_is(name, "SetupMode")) {
			/* A firmware that has the variable answers with its own value. */
			if (st->original(name, vendor, NULL, &probe, NULL) != EFI_NOT_FOUND)
				return st->original(name, vendor, attributes, size, data);
			if (rt_name_is(name, "SecureBoot"))
				return rt_serve(&secure_boot_on, 1u, SB_RT_GLOBAL_ATTR, attributes, size, data);
			return rt_serve(&setup_mode_off, 1u, SB_RT_GLOBAL_ATTR, attributes, size, data);
		}
	}
	if (rt_is_database(vendor)) {
		if (rt_name_is(name, "db"))
			return rt_serve(st->db, st->db_bytes, SB_RT_ACCEPTED_ATTR, attributes, size, data);
		if (rt_name_is(name, "dbx"))
			return rt_serve(st->dbx, st->dbx_bytes, SB_RT_ACCEPTED_ATTR, attributes, size, data);
	}
	return st->original(name, vendor, attributes, size, data);
}

/* The end of the copied range. Nothing here runs: only the address matters. */
SB_RT_TEXT(d) static VOID EFIAPI sb_rt_end(VOID)
{
}

/* ---- the install and the unwind ------------------------------------------------------------------ */

const char *sb_windows_result_text(int result)
{
	switch (result) {
	case 0: return "the filter is in place";
	case 1: return "no memory for the runtime filter";
	case 2: return "the copied range of the filter is not whole";
	case 3: return "the firmware did not give the address change event";
	case 4: return "the firmware kept its own GetVariable after the write";
	default: return "unknown result";
	}
}

int sb_windows_install(struct stage *stage, const struct sb_windows_inputs *inputs)
{
	const unsigned char *code_start = (const unsigned char *)(void *)&sb_rt_begin;
	const unsigned char *code_end = (const unsigned char *)(void *)&sb_rt_end;
	UINTN code_bytes, blob_bytes, offset;
	struct sb_rt_state *st;
	unsigned char *blobs;
	EFI_STATUS status;
	EFI_EVENT event = NULL;
	EFI_RUNTIME_SERVICES *rs;
	EFI_EVENT_NOTIFY notify_entry;
	EFI_STATUS (EFIAPI *filter_entry)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, VOID *);

	if (sbw.installed) return 0;
	/* The copy must hold the whole of the two functions, and the two markers must be in their order. */
	if (code_end <= code_start || (UINTN)(code_end - code_start) > SB_RT_CODE_MAX ||
	    (const unsigned char *)(void *)&sb_rt_filter <= code_start ||
	    (const unsigned char *)(void *)&sb_rt_filter >= code_end)
		return 2;
	code_bytes = (UINTN)(code_end - code_start);
	blob_bytes = inputs->pk_bytes + inputs->kek_bytes + inputs->db_bytes + inputs->dbx_bytes;
	/* One 8-byte slot follows the copy: the address of the state. */
	if (EFI_ERROR(stage->bs->AllocatePool(EfiRuntimeServicesCode, code_bytes + sizeof(UINT64), &sbw.code)) ||
	    sbw.code == NULL) {
		sbw.code = NULL;
		return 1;
	}
	if (EFI_ERROR(stage->bs->AllocatePool(EfiRuntimeServicesData, sizeof(*st) + blob_bytes, &sbw.state)) ||
	    sbw.state == NULL) {
		stage->bs->FreePool(sbw.code);
		sbw.code = NULL;
		sbw.state = NULL;
		return 1;
	}
	rt_copy(sbw.code, code_start, code_bytes);
	st = (struct sb_rt_state *)sbw.state;
	blobs = (unsigned char *)st + sizeof(*st);
	st->magic = SB_RT_MAGIC;
	st->rs = stage->st->RuntimeServices;
	st->original = st->rs->GetVariable;
	offset = 0;
	rt_copy(blobs + offset, inputs->pk, inputs->pk_bytes);
	st->pk = blobs + offset;
	st->pk_bytes = inputs->pk_bytes;
	offset += inputs->pk_bytes;
	rt_copy(blobs + offset, inputs->kek, inputs->kek_bytes);
	st->kek = blobs + offset;
	st->kek_bytes = inputs->kek_bytes;
	offset += inputs->kek_bytes;
	rt_copy(blobs + offset, inputs->db, inputs->db_bytes);
	st->db = blobs + offset;
	st->db_bytes = inputs->db_bytes;
	offset += inputs->db_bytes;
	rt_copy(blobs + offset, inputs->dbx, inputs->dbx_bytes);
	st->dbx = blobs + offset;
	st->dbx_bytes = inputs->dbx_bytes;
	/* The slot holds the address of the state while this start runs. */
#ifdef SB_RT_IN_PLACE
	*rt_slot_ptr() = (UINT64)(UINTN)st;
#else
	*((volatile UINT64 *)((unsigned char *)sbw.code + code_bytes)) = (UINT64)(UINTN)st;
#endif
	stage_sayf(stage, "secure-boot", "the copied range: %llx bytes at %llx, notify at %llx, filter at %llx",
		   (unsigned long long)code_bytes, (unsigned long long)(UINTN)sbw.code,
		   (unsigned long long)(UINTN)((unsigned char *)sbw.code +
					       (UINTN)((const unsigned char *)(void *)&sb_rt_notify - code_start)),
		   (unsigned long long)(UINTN)((unsigned char *)sbw.code +
					       (UINTN)((const unsigned char *)(void *)&sb_rt_filter - code_start)));
	/* The notify of the event and the GetVariable entry are the copies: the image of the stage is freed before they
	 * run. The host test builds this file with SB_RT_IN_PLACE, because the model keeps the image mapped and runs the
	 * same code in place; the copy is proved on the lab (the s6 log and the Windows readings), and the range
	 * arithmetic is the same in both builds. */
#ifdef SB_RT_IN_PLACE
	notify_entry = (EFI_EVENT_NOTIFY)(void *)sb_rt_notify;
	filter_entry = (EFI_STATUS (EFIAPI *)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, VOID *))(void *)sb_rt_filter;
#else
	notify_entry = (EFI_EVENT_NOTIFY)(void *)((unsigned char *)sbw.code +
						  (UINTN)((const unsigned char *)(void *)&sb_rt_notify - code_start));
	filter_entry = (EFI_STATUS (EFIAPI *)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, VOID *))(void *)
		       ((unsigned char *)sbw.code +
			(UINTN)((const unsigned char *)(void *)&sb_rt_filter - code_start));
#endif
	status = stage->bs->CreateEvent(EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE, SB_RT_TPL_NOTIFY, notify_entry, NULL, &event);
	if (EFI_ERROR(status) || event == NULL) {
		stage->bs->FreePool(sbw.state);
		stage->bs->FreePool(sbw.code);
		sbw.state = NULL;
		sbw.code = NULL;
		return 3;
	}
	sbw.event = event;
	rs = st->rs;
	sbw.rs = rs;
	sbw.original = st->original;
	rs->GetVariable = filter_entry;
	rt_table_crc(rs);
	/* The read-back: the table of this firmware may be in read-only memory, and a claim without the number would
	 * be a claim about nothing. */
	if (rs->GetVariable != filter_entry) {
		(void)stage->bs->CloseEvent(event);
		sbw.event = NULL;
		rs->GetVariable = sbw.original;
		rt_table_crc(rs);
		stage->bs->FreePool(sbw.state);
		stage->bs->FreePool(sbw.code);
		sbw.state = NULL;
		sbw.code = NULL;
		return 4;
	}
	sbw.installed = 1;
	return 0;
}

void sb_windows_unwind(struct stage *stage)
{
	if (sbw.code != NULL && sbw.rs != NULL && sbw.rs->GetVariable != sbw.original) {
		sbw.rs->GetVariable = sbw.original;
		rt_table_crc(sbw.rs);
	}
	if (sbw.event != NULL) (void)stage->bs->CloseEvent(sbw.event);
	if (sbw.state != NULL) stage->bs->FreePool(sbw.state);
	if (sbw.code != NULL) stage->bs->FreePool(sbw.code);
	sbw.event = NULL;
	sbw.state = NULL;
	sbw.code = NULL;
	sbw.rs = NULL;
	sbw.original = NULL;
	sbw.installed = 0;
}
