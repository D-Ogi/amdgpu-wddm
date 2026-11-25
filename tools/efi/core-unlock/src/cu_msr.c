/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The registers of the report and the check that guards each read (cu_msr.h). Read only. */
#include <stddef.h>
#include <string.h>
#include "cu_msr.h"

/* CPUID Fn0000_0000 EBX, EDX, ECX: "Auth", "enti", "cAMD" (PPR 56176, ProcVendEbx, ProcVendEdx, ProcVendEcx). */
static const char amd_vendor[] = "AuthenticAMD";

static const struct cu_msr_def defs[CU_MSR_I_COUNT] = {
	[CU_MSR_I_HWCR] = { CU_MSR_HWCR, "HWCR", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_CUR_LIM] = { CU_MSR_PSTATE_CUR_LIM, "PStateCurLim", CU_MSR_NEED_HW_PSTATE },
	[CU_MSR_I_PSTATE_CTL] = { CU_MSR_PSTATE_CTL, "PStateCtl", CU_MSR_NEED_HW_PSTATE },
	[CU_MSR_I_PSTATE_STAT] = { CU_MSR_PSTATE_STAT, "PStateStat", CU_MSR_NEED_HW_PSTATE },
	[CU_MSR_I_PSTATE_DEF0 + 0] = { CU_MSR_PSTATE_DEF0 + 0u, "PStateDef[0]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 1] = { CU_MSR_PSTATE_DEF0 + 1u, "PStateDef[1]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 2] = { CU_MSR_PSTATE_DEF0 + 2u, "PStateDef[2]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 3] = { CU_MSR_PSTATE_DEF0 + 3u, "PStateDef[3]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 4] = { CU_MSR_PSTATE_DEF0 + 4u, "PStateDef[4]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 5] = { CU_MSR_PSTATE_DEF0 + 5u, "PStateDef[5]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 6] = { CU_MSR_PSTATE_DEF0 + 6u, "PStateDef[6]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_PSTATE_DEF0 + 7] = { CU_MSR_PSTATE_DEF0 + 7u, "PStateDef[7]", CU_MSR_NEED_FAMILY17 },
	[CU_MSR_I_CSTATE_BASE_ADDR] = { CU_MSR_CSTATE_BASE_ADDR, "CStateBaseAddr", CU_MSR_NEED_FAMILY17 },
};

const struct cu_msr_def *cu_msr_def(enum cu_msr_index index)
{
	return (unsigned)index < CU_MSR_I_COUNT ? &defs[index] : NULL;
}

void cu_cpu_identify(cu_cpuid_fn cpuid, struct cu_cpu_identity *out)
{
	struct cu_cpuid r;
	char vendor[12];
	unsigned base_family, ext_family, base_model, ext_model, max_standard, max_extended;
	memset(out, 0, sizeof(*out));
	cpuid(CU_CPUID_VENDOR, &r);
	max_standard = r.eax;
	memcpy(vendor, &r.ebx, 4);
	memcpy(vendor + 4, &r.edx, 4);
	memcpy(vendor + 8, &r.ecx, 4);
	out->amd = !memcmp(vendor, amd_vendor, sizeof(vendor));
	if (max_standard < CU_CPUID_FAM_MOD_STEP) return;
	cpuid(CU_CPUID_FAM_MOD_STEP, &r);
	/* FamModStep: Family = BaseFamily[11:8] + ExtFamily[27:20]; Model = {ExtModel[19:16], BaseModel[7:4]}. */
	base_family = (r.eax >> 8) & 0xFu;
	ext_family = (r.eax >> 20) & 0xFFu;
	base_model = (r.eax >> 4) & 0xFu;
	ext_model = (r.eax >> 16) & 0xFu;
	out->family = base_family + ext_family;
	out->model = (ext_model << 4) | base_model;
	out->stepping = r.eax & 0xFu;
	out->apic_id = r.ebx >> 24;		/* FeatureIdEbx[31:24] LocalApicId */
	if (!out->amd) return;
	cpuid(CU_CPUID_EXT_MAX, &r);
	max_extended = r.eax;
	if (max_extended < CU_CPUID_APM_INFO) return;
	cpuid(CU_CPUID_APM_INFO, &r);
	out->apm_info = 1;
	out->hw_pstate = (r.edx & CU_CPUID_APM_HW_PSTATE) != 0;
	out->cpb = (r.edx & CU_CPUID_APM_CPB) != 0;
}

int cu_msr_allowed(const struct cu_msr_def *def, const struct cu_cpu_identity *id)
{
	if (def == NULL || !id->amd || id->family != CU_AMD_FAMILY_17H) return 0;
	return def->need == CU_MSR_NEED_FAMILY17 || (def->need == CU_MSR_NEED_HW_PSTATE && id->hw_pstate);
}

void cu_cpu_sample(cu_cpuid_fn cpuid, cu_rdmsr_fn rdmsr, struct cu_cpu_sample *out)
{
	unsigned i;
	memset(out, 0, sizeof(*out));
	cu_cpu_identify(cpuid, &out->id);
	for (i = 0; i < CU_MSR_I_COUNT; i++)
		if (cu_msr_allowed(&defs[i], &out->id)) {
			out->value[i] = rdmsr(defs[i].number);
			out->read |= 1u << i;
		}
}

void cu_pstate_def_decode(unsigned long long value, struct cu_pstate_def *out)
{
	out->enabled = (unsigned)(value >> 63) & 1u;
	out->idd_div = (unsigned)(value >> 30) & 0x3u;
	out->idd_value = (unsigned)(value >> 22) & 0xFFu;
	out->cpu_vid = (unsigned)(value >> 14) & 0xFFu;
	out->cpu_dfs_id = (unsigned)(value >> 8) & 0x3Fu;
	out->cpu_fid = (unsigned)value & 0xFFu;
	out->core_cof_mhz = out->cpu_dfs_id ? out->cpu_fid * 200u / out->cpu_dfs_id : 0u;
}

unsigned cu_pstate_max_val(unsigned long long cur_lim) { return (unsigned)(cur_lim >> 4) & 0x7u; }
unsigned cu_cur_pstate_limit(unsigned long long cur_lim) { return (unsigned)cur_lim & 0x7u; }
unsigned cu_pstate_low3(unsigned long long value) { return (unsigned)value & 0x7u; }
unsigned cu_cstate_addr(unsigned long long value) { return (unsigned)value & 0xFFFFu; }
unsigned cu_hwcr_cpb_dis(unsigned long long value) { return (unsigned)(value >> 25) & 1u; }
