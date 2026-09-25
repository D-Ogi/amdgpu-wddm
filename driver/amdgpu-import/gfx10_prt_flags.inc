/* SPDX-License-Identifier: MIT */
/* PROVENANCE: Linux amdgpu (MIT), v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449,
 * gmc_v10_0_get_vm_pte(): the PRT flag body, unchanged. Included with u64 *flags.
 */
		*flags |= AMDGPU_PTE_PRT;
		*flags |= AMDGPU_PTE_SNOOPED;
		*flags |= AMDGPU_PTE_LOG;
		*flags |= AMDGPU_PTE_SYSTEM;
		*flags &= ~AMDGPU_PTE_VALID;
