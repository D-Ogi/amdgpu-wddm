/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/bc250_umd_private_fields.h - the field NAMES of the three imported UAPI
 * structures, as X-macro lists. Names only: no types, no offsets, no struct definition. The
 * structures themselves are never retyped anywhere in this repository; they come from
 * driver/contract/third_party/amdgpu_drm.h (Linux UAPI, MIT).
 *
 * Why this exists. The blob in bc250_umd_private.h is the kernel's `struct
 * drm_amdgpu_info_device` by value. Mesa, when built for Windows, does not include the kernel
 * header at all: src/amd/common/ac_linux_drm.h:16-321 declares its own copy of the same
 * structures in plain uint32_t/uint64_t. Two declarations of one wire format, and they must
 * agree or a caps blob written by bc250kmd is read wrong by RADV.
 *
 * The two declarations cannot appear in one translation unit (C forbids the redefinition), so
 * each side includes this list with its own struct type and emits a {name, offset, size} table.
 * driver/contract/test compares the two tables field by field at run time. That turns "they look
 * the same" into a check that fails loudly when either side moves.
 *
 * Order and spelling follow amdgpu_drm.h at kernel tag v6.18 (see third_party/PROVENANCE.md).
 * The trailing number in each comment is the line in that file.
 */
#ifndef BC250_UMD_PRIVATE_FIELDS_H
#define BC250_UMD_PRIVATE_FIELDS_H

/* struct drm_amdgpu_info_device, amdgpu_drm.h:1431-1538. 65 members. */
#define BC250_DEVICE_FIELD_LIST(X)                                                                \
    X(device_id)                          /* 1433 */                                              \
    X(chip_rev)                           /* 1435 */                                              \
    X(external_rev)                       /* 1436 */                                              \
    X(pci_rev)                            /* 1438 */                                              \
    X(family)                             /* 1439 */                                              \
    X(num_shader_engines)                 /* 1440 */                                              \
    X(num_shader_arrays_per_engine)       /* 1441 */                                              \
    X(gpu_counter_freq)                   /* 1443 */                                              \
    X(max_engine_clock)                   /* 1444 */                                              \
    X(max_memory_clock)                   /* 1445 */                                              \
    X(cu_active_number)                   /* 1447 */                                              \
    X(cu_ao_mask)                         /* 1449 */                                              \
    X(cu_bitmap)                          /* 1450 */                                              \
    X(enabled_rb_pipes_mask)              /* 1452 */                                              \
    X(num_rb_pipes)                       /* 1453 */                                              \
    X(num_hw_gfx_contexts)                /* 1454 */                                              \
    X(pcie_gen)                           /* 1456 */                                              \
    X(ids_flags)                          /* 1457 */                                              \
    X(virtual_address_offset)             /* 1459 */                                              \
    X(virtual_address_max)                /* 1461 */                                              \
    X(virtual_address_alignment)          /* 1463 */                                              \
    X(pte_fragment_size)                  /* 1465 */                                              \
    X(gart_page_size)                     /* 1466 */                                              \
    X(ce_ram_size)                        /* 1468 */                                              \
    X(vram_type)                          /* 1470 */                                              \
    X(vram_bit_width)                     /* 1472 */                                              \
    X(vce_harvest_config)                 /* 1474 */                                              \
    X(gc_double_offchip_lds_buf)          /* 1476 */                                              \
    X(prim_buf_gpu_addr)                  /* 1478 */                                              \
    X(pos_buf_gpu_addr)                   /* 1480 */                                              \
    X(cntl_sb_buf_gpu_addr)               /* 1482 */                                              \
    X(param_buf_gpu_addr)                 /* 1484 */                                              \
    X(prim_buf_size)                      /* 1485 */                                              \
    X(pos_buf_size)                       /* 1486 */                                              \
    X(cntl_sb_buf_size)                   /* 1487 */                                              \
    X(param_buf_size)                     /* 1488 */                                              \
    X(wave_front_size)                    /* 1490 */                                              \
    X(num_shader_visible_vgprs)           /* 1492 */                                              \
    X(num_cu_per_sh)                      /* 1494 */                                              \
    X(num_tcc_blocks)                     /* 1496 */                                              \
    X(gs_vgt_table_depth)                 /* 1498 */                                              \
    X(gs_prim_buffer_depth)               /* 1500 */                                              \
    X(max_gs_waves_per_vgt)               /* 1502 */                                              \
    X(pcie_num_lanes)                     /* 1504 */                                              \
    X(cu_ao_bitmap)                       /* 1506 */                                              \
    X(high_va_offset)                     /* 1508 */                                              \
    X(high_va_max)                        /* 1510 */                                              \
    X(pa_sc_tile_steering_override)       /* 1512 */                                              \
    X(tcc_disabled_mask)                  /* 1514 */                                              \
    X(min_engine_clock)                   /* 1515 */                                              \
    X(min_memory_clock)                   /* 1516 */                                              \
    X(tcp_cache_size)                     /* 1518 */                                              \
    X(num_sqc_per_wgp)                    /* 1519 */                                              \
    X(sqc_data_cache_size)                /* 1520 */                                              \
    X(sqc_inst_cache_size)                /* 1521 */                                              \
    X(gl1c_cache_size)                    /* 1522 */                                              \
    X(gl2c_cache_size)                    /* 1523 */                                              \
    X(mall_size)                          /* 1524 */                                              \
    X(enabled_rb_pipes_mask_hi)           /* 1526 */                                              \
    X(shadow_size)                        /* 1528 */                                              \
    X(shadow_alignment)                   /* 1530 */                                              \
    X(csa_size)                           /* 1532 */                                              \
    X(csa_alignment)                      /* 1534 */                                              \
    X(userq_ip_mask)                      /* 1536 */                                              \
    X(pad)                                /* 1537 */

#define BC250_DEVICE_FIELD_COUNT 65

/* struct drm_amdgpu_info_hw_ip, amdgpu_drm.h:1540-1556. 8 members.
 * Mesa's Windows copy (ac_linux_drm.h:192) declares 6 of them: capabilities_flags and
 * userq_num_slots are absent, which also shifts every offset after hw_ip_version_minor. The test
 * checks only the six Mesa knows about and reports the divergence rather than hiding it. */
#define BC250_HW_IP_FIELD_LIST(X)                                                                 \
    X(hw_ip_version_major)                /* 1542 */                                              \
    X(hw_ip_version_minor)                /* 1543 */                                              \
    X(capabilities_flags)                 /* 1545 */                                              \
    X(ib_start_alignment)                 /* 1547 */                                              \
    X(ib_size_alignment)                  /* 1549 */                                              \
    X(available_rings)                    /* 1551 */                                              \
    X(ip_discovery_version)               /* 1553 */                                              \
    X(userq_num_slots)                    /* 1555 */

#define BC250_HW_IP_FIELD_COUNT 8

/* struct drm_amdgpu_heap_info, amdgpu_drm.h:1374-1394. 4 members.
 * Mesa's Windows copy (ac_linux_drm.h) declares only total_heap_size, so the heap list is the one
 * place where the two declarations are known to differ in size. The test checks the offset of
 * total_heap_size and the offsets of the three heaps inside drm_amdgpu_memory_info, and says so
 * rather than pretending the structures match. */
#define BC250_HEAP_FIELD_LIST(X)                                                                  \
    X(total_heap_size)                    /* 1376 */                                              \
    X(usable_heap_size)                   /* 1379 */                                              \
    X(heap_usage)                         /* 1387 */                                              \
    X(max_allocation)                     /* 1393 */

#define BC250_HEAP_FIELD_COUNT 4

/* struct drm_amdgpu_memory_info, amdgpu_drm.h:1396-1400. */
#define BC250_MEMORY_FIELD_LIST(X)                                                                \
    X(vram)                               /* 1397 */                                              \
    X(cpu_accessible_vram)                /* 1398 */                                              \
    X(gtt)                                /* 1399 */

#define BC250_MEMORY_FIELD_COUNT 3

#endif /* BC250_UMD_PRIVATE_FIELDS_H */
