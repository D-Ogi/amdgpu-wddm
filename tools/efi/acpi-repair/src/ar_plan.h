/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The ACPI repair of the BC-250 stage (docs/design/efi-resident-stage.md, "The ACPI repair"): the decision and the
 * table builder. Pure code: the caller gives the bytes of the firmware's tables and the processors of the MADT, and
 * gets a plan and the bytes of the tables to install. src/ar_module.c is the UEFI binding.
 *
 * Two repairs:
 *
 *  - Idle states. The stock firmware publishes one SSDT ("AMD CPU") whose scopes are \_PR_.C000 and up. The DSDT
 *    declares the processors as \_PR_.P000 and up, so that table does not load, and no processor has _CST. The
 *    repair writes one SSDT with the same objects in the scopes that exist. Each _CST object is a copy of the
 *    firmware's own bytes. Nothing in it is a number of this project.
 *  - Method stubs. The stock DSDT calls three methods that no table defines. The repair defines each as an empty
 *    method, so the caller goes on in place of an AE_NOT_FOUND abort.
 *
 * It writes no processor performance objects (_PSS, _PCT, _PPC). Their values are not measured on this board
 * (docs/linux-session-wishlist.md).
 *
 * The plan repairs only what it identified: the stock table by its content, the processor objects by their ProcID
 * against the MADT. On any other input the plan says what it found and builds nothing.
 */
#ifndef AR_PLAN_H
#define AR_PLAN_H
#include <stddef.h>
#include "efl_aml.h"

#define AR_MAX_CPUS	32u
#define AR_MAX_SSDTS	16u
#define AR_STUBS	3u

struct ar_cpu {
	unsigned uid;		/* the ACPI processor UID of the MADT entry, which is the ProcID of the Processor object */
	unsigned apic_id;
};

struct ar_table {
	const unsigned char *bytes;	/* the whole table, header included */
	size_t length;
};

struct ar_input {
	struct ar_table dsdt;
	unsigned ssdts;
	struct ar_table ssdt[AR_MAX_SSDTS];	/* every SSDT and PSDT of the root table, in its order */
	/* 1 when dsdt and ssdt are the whole set of definition blocks: the root table listed every table, each one
	 * mapped with a right checksum, and each one is here. With 0 the plan does nothing: a table that it cannot
	 * see can define a name that a built table would define a second time. */
	int complete;
	unsigned cpus;				/* the enabled processor entries of the MADT, in its order */
	struct ar_cpu cpu[AR_MAX_CPUS];
	int want_idle;				/* 0: the load option turned this repair off */
	int want_stubs;
	/* 1: the SSDT also carries the experiment device of the TPM work: Device(TPM0) with _HID "BC250001". */
	int want_device;
};

enum ar_idle {
	AR_IDLE_REPAIR = 0,		/* build and install the table */
	AR_IDLE_OFF,			/* turned off by the load option */
	AR_IDLE_NO_STOCK_TABLE,		/* no SSDT with the header of the stock CPU table */
	AR_IDLE_NOT_STOCK,		/* a table with that header whose content is not the stock content */
	AR_IDLE_TARGETS_EXIST,		/* an object C000 is defined: the stock table is not defective here */
	AR_IDLE_ALREADY_PRESENT,	/* a processor already has _CST or _CSD in the DSDT or in another table */
	AR_IDLE_NO_PROCESSORS,		/* the MADT lists no enabled processor, or more than AR_MAX_CPUS */
	AR_IDLE_NO_PROCESSOR_OBJECT,	/* an enabled processor has no Processor object in the DSDT */
	AR_IDLE_TABLES_INCOMPLETE,	/* the set of definition blocks is not whole, or one of them does not scan */
	AR_IDLE_COUNT
};

enum ar_stubs {
	AR_STUBS_REPAIR = 0,
	AR_STUBS_OFF,
	AR_STUBS_NOT_STOCK_DSDT,	/* the DSDT does not have the header of the stock DSDT */
	AR_STUBS_NOTHING_MISSING,	/* each method is defined, the DSDT does not name it, or a call has another form */
	AR_STUBS_TABLES_INCOMPLETE,	/* the set of definition blocks is not whole, or one of them does not scan */
	AR_STUBS_COUNT
};

struct ar_target {
	char path[EFL_AML_PATH_CHARS];	/* the Processor object of the DSDT, "\_PR_.P000" form */
	int stock_scope;		/* the scope of the stock table whose _CSD is copied, or -1: _CSD is derived */
	unsigned domain;		/* the _CSD domain */
};

struct ar_plan {
	enum ar_idle idle;
	int stock_ssdt;			/* the index in ar_input.ssdt of the stock CPU table, or -1 */
	int stock_exact;		/* the whole table has the digest of the table stored from unit A */
	unsigned stock_scopes;
	const unsigned char *cst;	/* the firmware's _CST object: NameOp, the name and the package */
	size_t cst_bytes;
	const unsigned char *csd[AR_MAX_CPUS];	/* per stock scope: the firmware's _CSD object */
	size_t csd_bytes[AR_MAX_CPUS];
	unsigned targets;		/* processors that get the objects */
	unsigned copied;		/* of them: both objects are copies of a stock scope */
	unsigned derived;		/* of them: _CST is a copy, _CSD follows the rule that the stock scopes obey */
	unsigned left_out;		/* enabled processors past the stock scopes that the rule does not cover */
	struct ar_target target[AR_MAX_CPUS];

	enum ar_stubs stubs;
	unsigned stub_mask;		/* bit i: ar_stub_name(i) gets an empty method */
	int device;			/* 1: ar_build_stubs writes the experiment device */
};

void ar_plan_make(const struct ar_input *in, struct ar_plan *plan);
const char *ar_idle_text(enum ar_idle state);
const char *ar_stubs_text(enum ar_stubs state);
const char *ar_stub_name(unsigned index);

/* The OEM table IDs of the two tables that the repair installs. */
#define AR_OEM_ID		"BC250"
#define AR_IDLE_TABLE_ID	"CPUIDLE"
#define AR_STUB_TABLE_ID	"AMLSTUB"

/* The bytes of the table, 0 when the plan does not ask for it or it does not fit in capacity. */
size_t ar_build_idle(const struct ar_plan *plan, unsigned char *out, size_t capacity);
size_t ar_build_stubs(const struct ar_plan *plan, unsigned char *out, size_t capacity);

/* The check of a built table before it is installed: the length field, the checksum, and a name scan that must
 * find exactly what the plan asked for. 1 when the table is right. */
int ar_check_idle(const struct ar_plan *plan, const unsigned char *table, size_t bytes);
int ar_check_stubs(const struct ar_plan *plan, const unsigned char *table, size_t bytes);

/* 1 when the bytes are a table whose length field equals bytes and whose checksum is right. */
int ar_table_sound(const unsigned char *table, size_t bytes);

#endif
