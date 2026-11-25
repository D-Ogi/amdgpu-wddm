/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The model-specific registers that the read-only report reads, and the CPUID check that guards each read
 * (docs/design/efi-core-unlock.md, "The report"). This module only reads. It has no write primitive, and the image
 * has no WRMSR instruction.
 *
 * The source of every MSR number, name and field is the AMD "Processor Programming Reference (PPR) for AMD Family
 * 17h Model 71h, Revision B0 Processors", publication 56176, revision 3.06, 17 July 2019. That PPR is of family
 * 17h model 71h. The repository holds no CPUID family or model of the BC-250, so the report prints the identity
 * that it reads and reads these registers only on an AMD processor of family 17h. The register names are the PPR
 * names, Core::X86::Msr::<name>.
 *
 * The primitives (CPUID, RDMSR) come from the caller, so the host tests can prove which reads a given identity
 * allows.
 */
#ifndef CU_MSR_H
#define CU_MSR_H

/* PPR 56176 section 2.1.14.3, MSRs - MSRC001_0xxx. */
#define CU_MSR_HWCR			0xC0010015u	/* MSRC001_0015 Core::X86::Msr::HWCR */
#define CU_MSR_PSTATE_CUR_LIM		0xC0010061u	/* MSRC001_0061 Core::X86::Msr::PStateCurLim */
#define CU_MSR_PSTATE_CTL		0xC0010062u	/* MSRC001_0062 Core::X86::Msr::PStateCtl */
#define CU_MSR_PSTATE_STAT		0xC0010063u	/* MSRC001_0063 Core::X86::Msr::PStateStat */
#define CU_MSR_PSTATE_DEF0		0xC0010064u	/* MSRC001_0064 Core::X86::Msr::PStateDef[0]; [7] is MSRC001_006B */
#define CU_MSR_PSTATE_DEFS		8u
#define CU_MSR_CSTATE_BASE_ADDR		0xC0010073u	/* MSRC001_0073 Core::X86::Msr::CStateBaseAddr */

/* The CPUID functions of the check (PPR 56176 section 2.1.13.1). */
#define CU_CPUID_VENDOR			0x00000000u	/* Fn0000_0000: the largest standard function, the vendor */
#define CU_CPUID_FAM_MOD_STEP		0x00000001u	/* Fn0000_0001_EAX FamModStep, _EBX FeatureIdEbx */
#define CU_CPUID_EXT_MAX		0x80000000u	/* Fn8000_0000_EAX: the largest extended function */
#define CU_CPUID_APM_INFO		0x80000007u	/* Fn8000_0007_EDX ApmInfoEdx */
#define CU_CPUID_APM_HW_PSTATE		(1u << 7)	/* ApmInfoEdx[HwPstate]: PStateCurLim, PStateCtl, PStateStat exist */
#define CU_CPUID_APM_CPB		(1u << 9)	/* ApmInfoEdx[CPB]: HWCR[CpbDis] exists */
#define CU_AMD_FAMILY_17H		0x17u

enum cu_msr_index {
	CU_MSR_I_HWCR = 0,
	CU_MSR_I_PSTATE_CUR_LIM,
	CU_MSR_I_PSTATE_CTL,
	CU_MSR_I_PSTATE_STAT,
	CU_MSR_I_PSTATE_DEF0,
	CU_MSR_I_CSTATE_BASE_ADDR = CU_MSR_I_PSTATE_DEF0 + CU_MSR_PSTATE_DEFS,
	CU_MSR_I_COUNT
};

/* What a register needs before it is read. */
enum cu_msr_need {
	CU_MSR_NEED_FAMILY17 = 0,	/* an AMD processor of family 17h */
	CU_MSR_NEED_HW_PSTATE		/* that, and ApmInfoEdx[HwPstate] */
};

struct cu_msr_def {
	unsigned number;
	const char *name;
	enum cu_msr_need need;
};
const struct cu_msr_def *cu_msr_def(enum cu_msr_index index);

struct cu_cpuid { unsigned eax, ebx, ecx, edx; };

/* The CPUID values that the check reads. */
struct cu_cpu_identity {
	int amd;			/* the vendor string is "AuthenticAMD" */
	unsigned family, model, stepping;
	unsigned apic_id;		/* FeatureIdEbx[LocalApicId], the initial local APIC ID */
	int apm_info;			/* Fn8000_0007 exists */
	int hw_pstate, cpb;
};

/* The caller's primitives. Both run on the processor that is measured. */
typedef void (*cu_cpuid_fn)(unsigned function, struct cu_cpuid *out);
typedef unsigned long long (*cu_rdmsr_fn)(unsigned number);

/* One processor's sample: the identity, and each register that the identity allows. */
struct cu_cpu_sample {
	struct cu_cpu_identity id;
	unsigned read;			/* bit (1 << cu_msr_index) for each register that was read */
	unsigned long long value[CU_MSR_I_COUNT];
};

void cu_cpu_identify(cu_cpuid_fn cpuid, struct cu_cpu_identity *out);
int cu_msr_allowed(const struct cu_msr_def *def, const struct cu_cpu_identity *id);
/* Fills a sample. It calls rdmsr only for the registers of the list that cu_msr_allowed admits. */
void cu_cpu_sample(cu_cpuid_fn cpuid, cu_rdmsr_fn rdmsr, struct cu_cpu_sample *out);

/* The fields of Core::X86::Msr::PStateDef (PPR 56176, MSRC001_006[4...B]). */
struct cu_pstate_def {
	unsigned enabled;		/* [63] PstateEn */
	unsigned idd_div;		/* [31:30] IddDiv */
	unsigned idd_value;		/* [29:22] IddValue */
	unsigned cpu_vid;		/* [21:14] CpuVid[7:0] */
	unsigned cpu_dfs_id;		/* [13:8] CpuDfsId */
	unsigned cpu_fid;		/* [7:0] CpuFid[7:0] */
	unsigned core_cof_mhz;		/* CoreCOF = (CpuFid / CpuDfsId) * 200; 0 when CpuDfsId is 0 */
};
void cu_pstate_def_decode(unsigned long long value, struct cu_pstate_def *out);

/* PStateCurLim[6:4] PstateMaxVal and [2:0] CurPstateLimit; PStateCtl[2:0] PstateCmd; PStateStat[2:0] CurPstate;
 * CStateBaseAddr[15:0] CstateAddr; HWCR[25] CpbDis. */
unsigned cu_pstate_max_val(unsigned long long cur_lim);
unsigned cu_cur_pstate_limit(unsigned long long cur_lim);
unsigned cu_pstate_low3(unsigned long long value);
unsigned cu_cstate_addr(unsigned long long value);
unsigned cu_hwcr_cpb_dis(unsigned long long value);

#endif
