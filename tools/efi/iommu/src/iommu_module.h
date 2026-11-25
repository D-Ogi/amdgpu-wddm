/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef IOMMU_MODULE_H
#define IOMMU_MODULE_H

#define IOMMU_NAME	"iommu"

/* SMN address of the IOMMU PCI config space on Oberon. */
#define IOMMU_SMN_BASE		0x13F00000u
#define IOMMU_SMN_CAP_LO	(IOMMU_SMN_BASE + 0x44u)
#define IOMMU_SMN_CAP_HI	(IOMMU_SMN_BASE + 0x48u)

/* MMIO base that the module assigns. 16 KB aligned, in the MMIO gap. */
#define IOMMU_MMIO_BASE		0xFEB80000u
#define IOMMU_MMIO_SIZE		0x4000u

/* BAR5 offsets for the PCIE_INDEX2/DATA2 SMN pair. */
#define IOMMU_BAR5_INDEX2	0x38u
#define IOMMU_BAR5_DATA2	0x3Cu

/* IOMMU PCI vendor/device for Oberon (1022:13E1). */
#define IOMMU_VID_DID		0x13E11022u

/* PCI device ID for the IVHD entry (bus 0, device 0, function 2 = BDF 0x0002). */
#define IOMMU_BDF		0x0002u

/* Capability offset within the IOMMU's PCI config space. */
#define IOMMU_CAP_OFFSET	0x40u

#endif
