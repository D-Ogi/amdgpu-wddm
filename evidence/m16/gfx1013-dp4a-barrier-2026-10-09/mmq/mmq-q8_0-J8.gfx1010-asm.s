; gfx1010-asm.o: the disassembly of one kernel, nothing else.
; llvm-objdump -d, llvm-amdgpu 22.1.8, target gfx1013.
;
000000000001ed00 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_>:
	s_clause 0x3                                               // 00000001ED00: BFA10003
	s_load_dwordx2 s[24:25], s[4:5], 0x58                      // 00000001ED04: F4040602 FA000058
	s_load_dwordx2 s[10:11], s[4:5], 0x10                      // 00000001ED0C: F4040282 FA000010
	s_load_dwordx4 s[0:3], s[4:5], 0x48                        // 00000001ED14: F4080002 FA000048
	s_load_dwordx8 s[12:19], s[4:5], 0x64                      // 00000001ED1C: F40C0302 FA000064
	v_lshlrev_b32_e32 v2, 5, v1                                // 00000001ED24: 34040285
	v_add_nc_u32_e32 v3, v2, v0                                // 00000001ED28: 4A060102
	v_cmp_gt_u32_e32 vcc_lo, 8, v3                             // 00000001ED2C: 7D880688
	s_and_saveexec_b32 s9, vcc_lo                              // 00000001ED30: BE893C6A
	v_lshl_add_u32 v4, v3, 2, 0                                // 00000001ED34: D7460004 02010503
	ds_write_b32 v4, v3                                        // 00000001ED3C: D8340000 00000304
	s_waitcnt_depctr depctr_vm_vsrc(0)                         // 00000001ED44: BFA3FF03
	s_or_b32 exec_lo, exec_lo, s9                              // 00000001ED48: 887E097E
	s_load_dwordx4 s[20:23], s[4:5], 0x94                      // 00000001ED4C: F4080502 FA000094
	s_waitcnt lgkmcnt(0)                                       // 00000001ED54: BF8CC07F
	s_mul_hi_u32 s9, s12, s8                                   // 00000001ED58: 9A89080C
	s_add_i32 s9, s8, s9                                       // 00000001ED5C: 81090908
	s_barrier                                                  // 00000001ED60: BF8A0000
	s_lshr_b32 s23, s9, s13                                    // 00000001ED64: 90170D09
	buffer_gl0_inv                                             // 00000001ED68: E1C40000 00000000
	s_mul_i32 s9, s23, s14                                     // 00000001ED70: 93090E17
	s_lshl_b32 s14, s7, 3                                      // 00000001ED74: 8F0E8307
	s_sub_i32 s26, s8, s9                                      // 00000001ED78: 819A0908
	s_cmp_eq_u64 s[10:11], 0                                   // 00000001ED7C: BF12800A
	s_cbranch_scc1 45                                          // 00000001ED80: BF85002D <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x138>
	s_load_dwordx2 s[8:9], s[4:5], 0x18                        // 00000001ED84: F4040202 FA000018
	s_ashr_i32 s27, s26, 31                                    // 00000001ED8C: 911B9F1A
	s_mov_b32 s22, 0                                           // 00000001ED90: BE960380
	s_lshl_b64 s[12:13], s[26:27], 2                           // 00000001ED94: 8F8C821A
	s_mov_b32 s7, 0                                            // 00000001ED98: BE870380
	s_waitcnt lgkmcnt(0)                                       // 00000001ED9C: BF8CC07F
	s_add_u32 s12, s8, s12                                     // 00000001EDA0: 800C0C08
	s_addc_u32 s13, s9, s13                                    // 00000001EDA4: 820D0D09
	s_load_dwordx2 s[8:9], s[12:13], null                      // 00000001EDA8: F4040206 FA000000
	s_waitcnt lgkmcnt(0)                                       // 00000001EDB0: BF8CC07F
	s_sub_i32 s0, s9, s8                                       // 00000001EDB4: 81800809
	s_mov_b32 s9, 0                                            // 00000001EDB8: BE890380
	s_cmp_lt_i32 s14, s0                                       // 00000001EDBC: BF04000E
	s_cbranch_scc0 25                                          // 00000001EDC0: BF840019 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x128>
	s_and_saveexec_b32 s7, vcc_lo                              // 00000001EDC4: BE873C6A
	s_cbranch_execz 15                                         // 00000001EDC8: BF88000F <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x108>
	v_or_b32_e32 v4, s14, v0                                   // 00000001EDCC: 3808000E
	v_add_nc_u32_e32 v4, s8, v4                                // 00000001EDD0: 4A080808
	v_ashrrev_i32_e32 v5, 31, v4                               // 00000001EDD4: 300A089F
	v_lshlrev_b64 v[4:5], 2, v[4:5]                            // 00000001EDD8: D6FF0004 00020882
	v_add_co_u32 v4, vcc_lo, s10, v4                           // 00000001EDE0: D70F6A04 0002080A
	v_add_co_ci_u32_e32 v5, vcc_lo, s11, v5, vcc_lo            // 00000001EDE8: 500A0A0B
	global_load_dword v4, v[4:5], off                          // 00000001EDEC: DC308000 047D0004
	v_lshl_add_u32 v5, v3, 2, 0                                // 00000001EDF4: D7460005 02010503
	s_waitcnt vmcnt(0)                                         // 00000001EDFC: BF8C3F70
	ds_write_b32 v5, v4                                        // 00000001EE00: D8340000 00000405
	s_waitcnt_depctr depctr_vm_vsrc(0)                         // 00000001EE08: BFA3FF03
	s_or_b32 exec_lo, exec_lo, s7                              // 00000001EE0C: 887E077E
	s_waitcnt lgkmcnt(0)                                       // 00000001EE10: BF8CC07F
	s_barrier                                                  // 00000001EE14: BF8A0000
	s_mov_b32 s9, -1                                           // 00000001EE18: BE8903C1
	s_mov_b32 s7, s8                                           // 00000001EE1C: BE870308
	buffer_gl0_inv                                             // 00000001EE20: E1C40000 00000000
	s_mov_b32 s16, 0                                           // 00000001EE28: BE900380
	s_and_b32 vcc_lo, exec_lo, s9                              // 00000001EE2C: 876A097E
	s_cbranch_vccz 4122                                        // 00000001EE30: BF86101A <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x419c>
	s_branch 10                                                // 00000001EE34: BF82000A <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x160>
	s_mul_i32 s9, s23, s22                                     // 00000001EE38: 93091617
	s_mul_i32 s10, s3, s14                                     // 00000001EE3C: 930A0E03
	s_mul_i32 s7, s23, s21                                     // 00000001EE40: 93071517
	s_mul_i32 s8, s26, s16                                     // 00000001EE44: 9308101A
	s_mul_i32 s11, s26, s17                                    // 00000001EE48: 930B111A
	s_add_i32 s9, s9, s10                                      // 00000001EE4C: 81090A09
	s_add_i32 s22, s8, s7                                      // 00000001EE50: 81160708
	s_add_i32 s16, s9, s11                                     // 00000001EE54: 81100B09
	s_mov_b32 s7, 0                                            // 00000001EE58: BE870380
	s_cbranch_execz 4111                                       // 00000001EE5C: BF88100F <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x419c>
	s_clause 0x1                                               // 00000001EE60: BFA10001
	s_load_dword s21, s[4:5], 0x40                             // 00000001EE64: F4000542 FA000040
	s_load_dwordx2 s[12:13], s[4:5], 0x20                      // 00000001EE6C: F4040302 FA000020
	s_lshl_b32 s17, s6, 7                                      // 00000001EE74: 8F118706
	s_waitcnt lgkmcnt(0)                                       // 00000001EE78: BF8CC07F
	s_cmp_lt_i32 s21, 1                                        // 00000001EE7C: BF048115
	s_cbranch_scc1 4065                                        // 00000001EE80: BF850FE1 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x4108>
	s_load_dwordx4 s[8:11], s[4:5], null                       // 00000001EE84: F4080202 FA000000
	s_add_i32 s27, s7, s14                                     // 00000001EE8C: 811B0E07
	s_mul_hi_u32 s7, s26, s24                                  // 00000001EE90: 9A87181A
	s_mul_i32 s24, s27, 36                                     // 00000001EE94: 9318A41B
	v_mul_lo_u32 v7, v1, s1                                    // 00000001EE98: D5690007 00000301
	s_add_i32 s28, s24, s22                                    // 00000001EEA0: 811C1618
	v_mul_u32_u24_e32 v16, 0x41, v1                            // 00000001EEA4: 162002FF 00000041
	s_ashr_i32 s29, s28, 31                                    // 00000001EEAC: 911D9F1C
	v_add_nc_u32_e32 v19, 32, v0                               // 00000001EEB0: 4A2600A0
	s_lshl_b64 s[28:29], s[28:29], 2                           // 00000001EEB4: 8F9C821C
	v_lshrrev_b32_e32 v4, 3, v0                                // 00000001EEB8: 2C080083
	v_lshlrev_b32_e32 v22, 2, v16                              // 00000001EEBC: 342C2082
	v_lshlrev_b32_e32 v15, 2, v0                               // 00000001EEC0: 341E0082
	v_and_b32_e32 v27, 0x1fc, v19                              // 00000001EEC4: 363626FF 000001FC
	v_add_nc_u32_e32 v20, 64, v0                               // 00000001EECC: 4A2800C0
	v_lshl_add_u32 v18, v0, 5, 0                               // 00000001EED0: D7460012 02010B00
	v_add_nc_u32_e32 v21, 0x60, v0                             // 00000001EED8: 4A2A00FF 00000060
	v_add_nc_u32_e32 v25, 0, v15                               // 00000001EEE0: 4A321E80
	v_lshl_add_u32 v26, v1, 2, v4                              // 00000001EEE4: D746001A 04110501
	v_and_b32_e32 v28, 0x1fc, v20                              // 00000001EEEC: 363828FF 000001FC
	s_waitcnt lgkmcnt(0)                                       // 00000001EEF4: BF8CC07F
	s_add_u32 s10, s10, s28                                    // 00000001EEF8: 800A1C0A
	s_addc_u32 s11, s11, s29                                   // 00000001EEFC: 820B1D0B
	s_lshl_b32 s22, s1, 3                                      // 00000001EF00: 8F168301
	v_and_b32_e32 v17, 0xfc, v0                                // 00000001EF04: 362200FF 000000FC
	v_add_nc_u32_e32 v9, s22, v7                               // 00000001EF0C: 4A120E16
	v_and_b32_e32 v29, 0x1fc, v21                              // 00000001EF10: 363A2AFF 000001FC
	v_add_nc_u32_e32 v20, v25, v22                             // 00000001EF18: 4A282D19
	v_add3_u32 v21, 0, v22, v15                                // 00000001EF1C: D76D0015 043E2C80
	v_mul_lo_u32 v22, v26, s1                                  // 00000001EF24: D5690016 0000031A
	v_add_nc_u32_e32 v10, s22, v9                              // 00000001EF2C: 4A141216
	v_lshl_add_u32 v15, v26, 5, 0                              // 00000001EF30: D746000F 02010B1A
	v_and_b32_e32 v31, 0x7fc, v26                              // 00000001EF38: 363E34FF 000007FC
	v_add_nc_u32_e32 v32, 32, v26                              // 00000001EF40: 4A4034A0
	v_add_nc_u32_e32 v33, 64, v26                              // 00000001EF44: 4A4234C0
	v_add_nc_u32_e32 v13, s22, v10                             // 00000001EF48: 4A1A1416
	v_add_nc_u32_e32 v34, 0x60, v26                            // 00000001EF4C: 4A4434FF 00000060
	v_add3_u32 v26, v18, v27, 0x400                            // 00000001EF54: D76D001A 03FE3712 00000400
	v_add3_u32 v27, v18, v28, 0x800                            // 00000001EF60: D76D001B 03FE3912 00000800
	v_add_nc_u32_e32 v17, v18, v17                             // 00000001EF6C: 4A222312
	v_add_nc_u32_e32 v16, s22, v13                             // 00000001EF70: 4A201A16
	v_add3_u32 v29, v18, v29, 0xc00                            // 00000001EF74: D76D001D 03FE3B12 00000C00
	v_and_b32_e32 v18, 0xffc, v32                              // 00000001EF80: 362440FF 00000FFC
	v_and_b32_e32 v35, 0xffc, v33                              // 00000001EF88: 364642FF 00000FFC
	v_and_b32_e32 v5, 7, v0                                    // 00000001EF90: 360A0087
	v_add_nc_u32_e32 v19, s22, v16                             // 00000001EF94: 4A262016
	v_and_b32_e32 v36, 0xffc, v34                              // 00000001EF98: 364844FF 00000FFC
	s_mul_i32 s5, s1, s17                                      // 00000001EFA0: 93051101
	v_add_nc_u32_e32 v41, v15, v35                             // 00000001EFA4: 4A52470F
	v_lshlrev_b32_e32 v42, 2, v5                               // 00000001EFA8: 34540A82
	v_add_nc_u32_e32 v23, s22, v19                             // 00000001EFAC: 4A2E2616
	s_lshl_b32 s1, s1, 5                                       // 00000001EFB0: 8F018501
	v_add_nc_u32_e32 v18, v15, v18                             // 00000001EFB4: 4A24250F
	v_lshlrev_b32_e32 v12, 2, v3                               // 00000001EFB8: 34180682
	v_add3_u32 v31, v15, v31, v42                              // 00000001EFBC: D76D001F 04AA3F0F
	v_add_nc_u32_e32 v24, s22, v23                             // 00000001EFC4: 4A302E16
	v_add_nc_u32_e32 v15, v15, v36                             // 00000001EFC8: 4A1E490F
	v_add_nc_u32_e32 v36, s1, v22                              // 00000001EFCC: 4A482C01
	v_lshlrev_b32_e32 v3, 1, v5                                // 00000001EFD0: 34060A81
	s_mul_hi_u32 s18, s23, s18                                 // 00000001EFD4: 9A921217
	v_add_nc_u32_e32 v28, s22, v24                             // 00000001EFD8: 4A383016
	v_mul_u32_u24_e32 v14, 36, v1                              // 00000001EFDC: 161C02A4
	s_add_i32 s23, s23, s18                                    // 00000001EFE0: 81171217
	v_add_nc_u32_e32 v39, s1, v36                              // 00000001EFE4: 4A4E4801
	v_lshl_add_u32 v25, v2, 2, v25                             // 00000001EFE8: D7460019 04650502
	v_add_nc_u32_e32 v32, s22, v28                             // 00000001EFF0: 4A403816
	v_lshlrev_b32_e32 v30, 1, v3                               // 00000001EFF4: 343C0681
	v_mad_u64_u32 v[2:3], s24, v4, 34, s[8:9]                  // 00000001EFF8: D5761802 00214504
	v_mad_u64_u32 v[4:5], s8, v5, 34, s[8:9]                   // 00000001F000: D5760804 00214505
	v_add_nc_u32_e32 v33, s22, v32                             // 00000001F008: 4A424016
	s_add_i32 s8, s26, s7                                      // 00000001F00C: 8108071A
	s_lshr_b32 s9, s23, s19                                    // 00000001F010: 90091317
	s_mul_i32 s6, s2, 36                                       // 00000001F014: 9306A402
	s_lshr_b32 s8, s8, s25                                     // 00000001F018: 90081908
	v_add_nc_u32_e32 v34, s22, v33                             // 00000001F01C: 4A444216
	s_mul_i32 s9, s9, s20                                      // 00000001F020: 93091409
	v_mov_b32_e32 v6, 0                                        // 00000001F024: 7E0C0280
	v_mad_u32_u24 v8, 0x104, v0, 0                             // 00000001F028: D5430008 020200FF 00000104
	v_mov_b32_e32 v11, 0                                       // 00000001F034: 7E160280
	v_add_nc_u32_e32 v35, s22, v34                             // 00000001F038: 4A464416
	v_lshl_add_u32 v14, v14, 2, 0                              // 00000001F03C: D746000E 0201050E
	v_add3_u32 v38, v18, v42, 0x400                            // 00000001F044: D76D0026 03FE5512 00000400
	v_add3_u32 v41, v41, v42, 0x800                            // 00000001F050: D76D0029 03FE5529 00000800
	v_add3_u32 v42, v15, v42, 0xc00                            // 00000001F05C: D76D002A 03FE550F 00000C00
	v_add_nc_u32_e32 v37, s22, v35                             // 00000001F068: 4A4A4616
	v_add_nc_u32_e32 v43, s1, v39                              // 00000001F06C: 4A564E01
	v_mov_b32_e32 v18, 0                                       // 00000001F070: 7E240280
	v_mov_b32_e32 v15, 0                                       // 00000001F074: 7E1E0280
	s_ashr_i32 s7, s6, 31                                      // 00000001F078: 91079F06
	v_add_nc_u32_e32 v40, s22, v37                             // 00000001F07C: 4A504A16
	s_mul_i32 s8, s8, s15                                      // 00000001F080: 93080F08
	s_add_i32 s5, s9, s5                                       // 00000001F084: 81050509
	s_mov_b32 s4, 0                                            // 00000001F088: BE840380
	s_mulk_i32 s2, 0x48                                        // 00000001F08C: B8020048
	v_add_nc_u32_e32 v44, s22, v40                             // 00000001F090: 4A585016
	s_lshl_b64 s[6:7], s[6:7], 2                               // 00000001F094: 8F868206
	s_add_i32 s1, s5, s8                                       // 00000001F098: 81010805
	s_mov_b32 s15, 0                                           // 00000001F09C: BE8F0380
	s_add_i32 s8, s1, s15                                      // 00000001F0A0: 81080F01
	s_ashr_i32 s5, s4, 31                                      // 00000001F0A4: 91059F04
	v_mad_i64_i32 v[45:46], s9, s8, 34, v[2:3]                 // 00000001F0A8: D577092D 04094408
	s_lshl_b64 s[18:19], s[4:5], 2                             // 00000001F0B0: 8F928204
	v_mad_i64_i32 v[47:48], s5, s8, 34, v[4:5]                 // 00000001F0B4: D577052F 04114408
	s_add_u32 s8, s10, s18                                     // 00000001F0BC: 8008120A
	s_addc_u32 s9, s11, s19                                    // 00000001F0C0: 8209130B
	s_clause 0x1                                               // 00000001F0C4: BFA10001
	global_load_dword v85, v12, s[8:9]                         // 00000001F0C8: DC308000 5508000C
	global_load_dword v86, v12, s[8:9] offset:1024             // 00000001F0D0: DC308400 5608000C
	v_mad_i64_i32 v[49:50], s5, v7, 34, v[45:46]               // 00000001F0D8: D5770531 04B54507
	v_mad_i64_i32 v[77:78], s5, v22, 34, v[47:48]              // 00000001F0E0: D577054D 04BD4516
	v_mad_i64_i32 v[79:80], s5, v36, 34, v[47:48]              // 00000001F0E8: D577054F 04BD4524
	v_mad_i64_i32 v[81:82], s5, v39, 34, v[47:48]              // 00000001F0F0: D5770551 04BD4527
	v_mad_i64_i32 v[51:52], s5, v9, 34, v[45:46]               // 00000001F0F8: D5770533 04B54509
	v_mad_i64_i32 v[47:48], s5, v43, 34, v[47:48]              // 00000001F100: D577052F 04BD452B
	v_mad_i64_i32 v[53:54], s5, v10, 34, v[45:46]              // 00000001F108: D5770535 04B5450A
	s_clause 0x3                                               // 00000001F110: BFA10003
	global_load_ushort v87, v[77:78], off                      // 00000001F114: DC288000 577D004D
	global_load_ushort v88, v[79:80], off                      // 00000001F11C: DC288000 587D004F
	global_load_ushort v89, v[81:82], off                      // 00000001F124: DC288000 597D0051
	global_load_ushort v90, v[47:48], off                      // 00000001F12C: DC288000 5A7D002F
	v_add_co_u32 v47, vcc_lo, v49, v30                         // 00000001F134: D70F6A2F 00023D31
	v_mad_i64_i32 v[55:56], s5, v13, 34, v[45:46]              // 00000001F13C: D5770537 04B5450D
	v_add_co_ci_u32_e32 v48, vcc_lo, 0, v50, vcc_lo            // 00000001F144: 50606480
	v_add_co_u32 v49, vcc_lo, v51, v30                         // 00000001F148: D70F6A31 00023D33
	v_mad_i64_i32 v[57:58], s5, v16, 34, v[45:46]              // 00000001F150: D5770539 04B54510
	v_add_co_ci_u32_e32 v50, vcc_lo, 0, v52, vcc_lo            // 00000001F158: 50646880
	v_add_co_u32 v51, vcc_lo, v53, v30                         // 00000001F15C: D70F6A33 00023D35
	v_mad_i64_i32 v[59:60], s5, v19, 34, v[45:46]              // 00000001F164: D577053B 04B54513
	v_add_co_ci_u32_e32 v52, vcc_lo, 0, v54, vcc_lo            // 00000001F16C: 50686C80
	v_add_co_u32 v53, vcc_lo, v55, v30                         // 00000001F170: D70F6A35 00023D37
	v_mad_i64_i32 v[61:62], s5, v23, 34, v[45:46]              // 00000001F178: D577053D 04B54517
	v_add_co_ci_u32_e32 v54, vcc_lo, 0, v56, vcc_lo            // 00000001F180: 506C7080
	v_add_co_u32 v55, vcc_lo, v57, v30                         // 00000001F184: D70F6A37 00023D39
	v_mad_i64_i32 v[63:64], s5, v24, 34, v[45:46]              // 00000001F18C: D577053F 04B54518
	v_add_co_ci_u32_e32 v56, vcc_lo, 0, v58, vcc_lo            // 00000001F194: 50707480
	v_add_co_u32 v57, vcc_lo, v59, v30                         // 00000001F198: D70F6A39 00023D3B
	v_mad_i64_i32 v[65:66], s5, v28, 34, v[45:46]              // 00000001F1A0: D5770541 04B5451C
	v_add_co_ci_u32_e32 v58, vcc_lo, 0, v60, vcc_lo            // 00000001F1A8: 50747880
	v_add_co_u32 v59, vcc_lo, v61, v30                         // 00000001F1AC: D70F6A3B 00023D3D
	v_mad_i64_i32 v[67:68], s5, v32, 34, v[45:46]              // 00000001F1B4: D5770543 04B54520
	v_add_co_ci_u32_e32 v60, vcc_lo, 0, v62, vcc_lo            // 00000001F1BC: 50787C80
	v_add_co_u32 v61, vcc_lo, v63, v30                         // 00000001F1C0: D70F6A3D 00023D3F
	v_mad_i64_i32 v[69:70], s5, v33, 34, v[45:46]              // 00000001F1C8: D5770545 04B54521
	v_add_co_ci_u32_e32 v62, vcc_lo, 0, v64, vcc_lo            // 00000001F1D0: 507C8080
	v_add_co_u32 v63, vcc_lo, v65, v30                         // 00000001F1D4: D70F6A3F 00023D41
	v_mad_i64_i32 v[71:72], s5, v34, 34, v[45:46]              // 00000001F1DC: D5770547 04B54522
	v_add_co_ci_u32_e32 v64, vcc_lo, 0, v66, vcc_lo            // 00000001F1E4: 50808480
	v_add_co_u32 v65, vcc_lo, v67, v30                         // 00000001F1E8: D70F6A41 00023D43
	v_mad_i64_i32 v[73:74], s5, v35, 34, v[45:46]              // 00000001F1F0: D5770549 04B54523
	v_add_co_ci_u32_e32 v66, vcc_lo, 0, v68, vcc_lo            // 00000001F1F8: 50848880
	v_add_co_u32 v67, vcc_lo, v69, v30                         // 00000001F1FC: D70F6A43 00023D45
	v_mad_i64_i32 v[75:76], s5, v37, 34, v[45:46]              // 00000001F204: D577054B 04B54525
	v_add_co_ci_u32_e32 v68, vcc_lo, 0, v70, vcc_lo            // 00000001F20C: 50888C80
	v_add_co_u32 v69, vcc_lo, v71, v30                         // 00000001F210: D70F6A45 00023D47
	v_mad_i64_i32 v[83:84], s5, v40, 34, v[45:46]              // 00000001F218: D5770553 04B54528
	v_add_co_ci_u32_e32 v70, vcc_lo, 0, v72, vcc_lo            // 00000001F220: 508C9080
	v_add_co_u32 v71, vcc_lo, v73, v30                         // 00000001F224: D70F6A47 00023D49
	v_mad_i64_i32 v[45:46], s5, v44, 34, v[45:46]              // 00000001F22C: D577052D 04B5452C
	v_add_co_ci_u32_e32 v72, vcc_lo, 0, v74, vcc_lo            // 00000001F234: 50909480
	v_add_co_u32 v73, vcc_lo, v75, v30                         // 00000001F238: D70F6A49 00023D4B
	v_add_co_ci_u32_e32 v74, vcc_lo, 0, v76, vcc_lo            // 00000001F240: 50949880
	v_add_co_u32 v75, vcc_lo, v83, v30                         // 00000001F244: D70F6A4B 00023D53
	v_add_co_ci_u32_e32 v76, vcc_lo, 0, v84, vcc_lo            // 00000001F24C: 5098A880
	v_add_co_u32 v45, vcc_lo, v45, v30                         // 00000001F250: D70F6A2D 00023D2D
	v_add_co_ci_u32_e32 v46, vcc_lo, 0, v46, vcc_lo            // 00000001F258: 505C5C80
	s_clause 0x1f                                              // 00000001F25C: BFA1001F
	global_load_dword v77, v[47:48], off offset:2              // 00000001F260: DC308002 4D7D002F
	global_load_dword v78, v[47:48], off offset:138            // 00000001F268: DC30808A 4E7D002F
	global_load_dword v79, v[49:50], off offset:2              // 00000001F270: DC308002 4F7D0031
	global_load_dword v80, v[49:50], off offset:138            // 00000001F278: DC30808A 507D0031
	global_load_dword v81, v[51:52], off offset:2              // 00000001F280: DC308002 517D0033
	global_load_dword v82, v[51:52], off offset:138            // 00000001F288: DC30808A 527D0033
	global_load_dword v83, v[53:54], off offset:2              // 00000001F290: DC308002 537D0035
	global_load_dword v84, v[53:54], off offset:138            // 00000001F298: DC30808A 547D0035
	global_load_dword v91, v[55:56], off offset:2              // 00000001F2A0: DC308002 5B7D0037
	global_load_dword v92, v[55:56], off offset:138            // 00000001F2A8: DC30808A 5C7D0037
	global_load_dword v93, v[57:58], off offset:2              // 00000001F2B0: DC308002 5D7D0039
	global_load_dword v94, v[57:58], off offset:138            // 00000001F2B8: DC30808A 5E7D0039
	global_load_dword v95, v[59:60], off offset:2              // 00000001F2C0: DC308002 5F7D003B
	global_load_dword v96, v[59:60], off offset:138            // 00000001F2C8: DC30808A 607D003B
	global_load_dword v97, v[61:62], off offset:2              // 00000001F2D0: DC308002 617D003D
	global_load_dword v53, v[61:62], off offset:138            // 00000001F2D8: DC30808A 357D003D
	global_load_dword v54, v[63:64], off offset:2              // 00000001F2E0: DC308002 367D003F
	global_load_dword v55, v[63:64], off offset:138            // 00000001F2E8: DC30808A 377D003F
	global_load_dword v56, v[65:66], off offset:2              // 00000001F2F0: DC308002 387D0041
	global_load_dword v57, v[65:66], off offset:138            // 00000001F2F8: DC30808A 397D0041
	global_load_dword v58, v[67:68], off offset:2              // 00000001F300: DC308002 3A7D0043
	global_load_dword v59, v[67:68], off offset:138            // 00000001F308: DC30808A 3B7D0043
	global_load_dword v60, v[69:70], off offset:2              // 00000001F310: DC308002 3C7D0045
	global_load_dword v98, v[69:70], off offset:138            // 00000001F318: DC30808A 627D0045
	global_load_dword v99, v[71:72], off offset:2              // 00000001F320: DC308002 637D0047
	global_load_dword v100, v[71:72], off offset:138           // 00000001F328: DC30808A 647D0047
	global_load_dword v101, v[73:74], off offset:2             // 00000001F330: DC308002 657D0049
	global_load_dword v102, v[73:74], off offset:138           // 00000001F338: DC30808A 667D0049
	global_load_dword v103, v[75:76], off offset:2             // 00000001F340: DC308002 677D004B
	global_load_dword v104, v[75:76], off offset:138           // 00000001F348: DC30808A 687D004B
	global_load_dword v61, v[45:46], off offset:2              // 00000001F350: DC308002 3D7D002D
	global_load_dword v62, v[45:46], off offset:138            // 00000001F358: DC30808A 3E7D002D
	v_add_nc_u32_e32 v52, 0x800, v8                            // 00000001F360: 4A6810FF 00000800
	v_add_nc_u32_e32 v181, 32, v25                             // 00000001F368: 4B6A32A0
	v_add_nc_u32_e32 v51, 0x2800, v8                           // 00000001F36C: 4A6610FF 00002800
	v_add_nc_u32_e32 v46, 0x4800, v8                           // 00000001F374: 4A5C10FF 00004800
	v_add_nc_u32_e32 v45, 0x6800, v8                           // 00000001F37C: 4A5A10FF 00006800
	v_mov_b32_e32 v182, 0                                      // 00000001F384: 7F6C0280
	v_mov_b32_e32 v183, 0                                      // 00000001F388: 7F6E0280
	v_mov_b32_e32 v184, 0                                      // 00000001F38C: 7F700280
	v_mov_b32_e32 v185, 0                                      // 00000001F390: 7F720280
	v_mov_b32_e32 v186, 0                                      // 00000001F394: 7F740280
	v_mov_b32_e32 v191, 0                                      // 00000001F398: 7F7E0280
	v_mov_b32_e32 v192, 0                                      // 00000001F39C: 7F800280
	v_mov_b32_e32 v194, 0                                      // 00000001F3A0: 7F840280
	v_add_nc_u32_e32 v50, 0x8800, v17                          // 00000001F3A4: 4A6422FF 00008800
	v_add_nc_u32_e32 v49, 0x8800, v26                          // 00000001F3AC: 4A6234FF 00008800
	v_add_nc_u32_e32 v48, 0x8800, v27                          // 00000001F3B4: 4A6036FF 00008800
	v_add_nc_u32_e32 v47, 0x8800, v29                          // 00000001F3BC: 4A5E3AFF 00008800
	v_mov_b32_e32 v187, 0                                      // 00000001F3C4: 7F760280
	v_mov_b32_e32 v188, 0                                      // 00000001F3C8: 7F780280
	v_mov_b32_e32 v189, 0                                      // 00000001F3CC: 7F7A0280
	v_mov_b32_e32 v190, 0                                      // 00000001F3D0: 7F7C0280
	v_mov_b32_e32 v193, 0                                      // 00000001F3D4: 7F820280
	v_mov_b32_e32 v195, 0                                      // 00000001F3D8: 7F860280
	v_mov_b32_e32 v196, 0                                      // 00000001F3DC: 7F880280
	v_mov_b32_e32 v197, 0                                      // 00000001F3E0: 7F8A0280
	s_add_u32 s8, s8, s6                                       // 00000001F3E4: 80080608
	s_addc_u32 s9, s9, s7                                      // 00000001F3E8: 82090709
	v_mov_b32_e32 v203, 0                                      // 00000001F3EC: 7F960280
	v_mov_b32_e32 v204, 0                                      // 00000001F3F0: 7F980280
	v_mov_b32_e32 v200, 0                                      // 00000001F3F4: 7F900280
	v_mov_b32_e32 v201, 0                                      // 00000001F3F8: 7F920280
	v_mov_b32_e32 v202, 0                                      // 00000001F3FC: 7F940280
	v_mov_b32_e32 v205, 0                                      // 00000001F400: 7F9A0280
	v_mov_b32_e32 v206, 0                                      // 00000001F404: 7F9C0280
	v_mov_b32_e32 v207, 0                                      // 00000001F408: 7F9E0280
	v_mov_b32_e32 v208, 0                                      // 00000001F40C: 7FA00280
	v_mov_b32_e32 v210, 0                                      // 00000001F410: 7FA40280
	v_mov_b32_e32 v211, 0                                      // 00000001F414: 7FA60280
	v_mov_b32_e32 v212, 0                                      // 00000001F418: 7FA80280
	v_mov_b32_e32 v209, 0                                      // 00000001F41C: 7FA20280
	v_mov_b32_e32 v213, 0                                      // 00000001F420: 7FAA0280
	s_add_i32 s15, s15, 8                                      // 00000001F424: 810F880F
	s_add_i32 s4, s4, s2                                       // 00000001F428: 81040204
	s_cmp_lt_i32 s15, s21                                      // 00000001F42C: BF04150F
	s_waitcnt vmcnt(35)                                        // 00000001F430: BF8CBF73
	v_cvt_f32_f16_e32 v63, v87                                 // 00000001F434: 7E7E1757
	s_waitcnt vmcnt(34)                                        // 00000001F438: BF8CBF72
	v_cvt_f32_f16_e32 v64, v88                                 // 00000001F43C: 7E801758
	s_waitcnt vmcnt(33)                                        // 00000001F440: BF8CBF71
	v_cvt_f32_f16_e32 v65, v89                                 // 00000001F444: 7E821759
	s_waitcnt vmcnt(32)                                        // 00000001F448: BF8CBF70
	v_cvt_f32_f16_e32 v66, v90                                 // 00000001F44C: 7E84175A
	s_waitcnt vmcnt(31)                                        // 00000001F450: BF8C7F7F
	ds_write_b32 v20, v77 offset:2080                          // 00000001F454: D8340820 00004D14
	s_waitcnt vmcnt(30)                                        // 00000001F45C: BF8C7F7E
	ds_write_b32 v21, v78 offset:2208                          // 00000001F460: D83408A0 00004E15
	s_waitcnt vmcnt(29)                                        // 00000001F468: BF8C7F7D
	ds_write_b32 v20, v79 offset:4160                          // 00000001F46C: D8341040 00004F14
	s_waitcnt vmcnt(28)                                        // 00000001F474: BF8C7F7C
	ds_write_b32 v21, v80 offset:4288                          // 00000001F478: D83410C0 00005015
	s_waitcnt vmcnt(27)                                        // 00000001F480: BF8C7F7B
	ds_write_b32 v20, v81 offset:6240                          // 00000001F484: D8341860 00005114
	s_waitcnt vmcnt(26)                                        // 00000001F48C: BF8C7F7A
	ds_write_b32 v21, v82 offset:6368                          // 00000001F490: D83418E0 00005215
	s_waitcnt vmcnt(25)                                        // 00000001F498: BF8C7F79
	ds_write_b32 v20, v83 offset:8320                          // 00000001F49C: D8342080 00005314
	s_waitcnt vmcnt(24)                                        // 00000001F4A4: BF8C7F78
	ds_write_b32 v21, v84 offset:8448                          // 00000001F4A8: D8342100 00005415
	s_waitcnt vmcnt(23)                                        // 00000001F4B0: BF8C7F77
	ds_write_b32 v20, v91 offset:10400                         // 00000001F4B4: D83428A0 00005B14
	s_waitcnt vmcnt(22)                                        // 00000001F4BC: BF8C7F76
	ds_write_b32 v21, v92 offset:10528                         // 00000001F4C0: D8342920 00005C15
	s_waitcnt vmcnt(21)                                        // 00000001F4C8: BF8C7F75
	ds_write_b32 v20, v93 offset:12480                         // 00000001F4CC: D83430C0 00005D14
	s_waitcnt vmcnt(20)                                        // 00000001F4D4: BF8C7F74
	ds_write_b32 v21, v94 offset:12608                         // 00000001F4D8: D8343140 00005E15
	s_waitcnt vmcnt(19)                                        // 00000001F4E0: BF8C7F73
	ds_write_b32 v20, v95 offset:14560                         // 00000001F4E4: D83438E0 00005F14
	s_waitcnt vmcnt(18)                                        // 00000001F4EC: BF8C7F72
	ds_write_b32 v21, v96 offset:14688                         // 00000001F4F0: D8343960 00006015
	s_waitcnt vmcnt(17)                                        // 00000001F4F8: BF8C7F71
	ds_write_b32 v20, v97 offset:16640                         // 00000001F4FC: D8344100 00006114
	s_waitcnt vmcnt(16)                                        // 00000001F504: BF8C7F70
	ds_write_b32 v21, v53 offset:16768                         // 00000001F508: D8344180 00003515
	s_waitcnt vmcnt(15)                                        // 00000001F510: BF8C3F7F
	ds_write_b32 v20, v54 offset:18720                         // 00000001F514: D8344920 00003614
	s_waitcnt vmcnt(14)                                        // 00000001F51C: BF8C3F7E
	ds_write_b32 v21, v55 offset:18848                         // 00000001F520: D83449A0 00003715
	s_waitcnt vmcnt(13)                                        // 00000001F528: BF8C3F7D
	ds_write_b32 v20, v56 offset:20800                         // 00000001F52C: D8345140 00003814
	s_waitcnt vmcnt(12)                                        // 00000001F534: BF8C3F7C
	ds_write_b32 v21, v57 offset:20928                         // 00000001F538: D83451C0 00003915
	s_waitcnt vmcnt(11)                                        // 00000001F540: BF8C3F7B
	ds_write_b32 v20, v58 offset:22880                         // 00000001F544: D8345960 00003A14
	s_waitcnt vmcnt(10)                                        // 00000001F54C: BF8C3F7A
	ds_write_b32 v21, v59 offset:23008                         // 00000001F550: D83459E0 00003B15
	s_waitcnt vmcnt(9)                                         // 00000001F558: BF8C3F79
	ds_write_b32 v20, v60 offset:24960                         // 00000001F55C: D8346180 00003C14
	s_waitcnt vmcnt(8)                                         // 00000001F564: BF8C3F78
	ds_write_b32 v21, v98 offset:25088                         // 00000001F568: D8346200 00006215
	s_waitcnt vmcnt(7)                                         // 00000001F570: BF8C3F77
	ds_write_b32 v20, v99 offset:27040                         // 00000001F574: D83469A0 00006314
	s_waitcnt vmcnt(6)                                         // 00000001F57C: BF8C3F76
	ds_write_b32 v21, v100 offset:27168                        // 00000001F580: D8346A20 00006415
	s_waitcnt vmcnt(5)                                         // 00000001F588: BF8C3F75
	ds_write_b32 v20, v101 offset:29120                        // 00000001F58C: D83471C0 00006514
	s_waitcnt vmcnt(4)                                         // 00000001F594: BF8C3F74
	ds_write_b32 v21, v102 offset:29248                        // 00000001F598: D8347240 00006615
	s_waitcnt vmcnt(3)                                         // 00000001F5A0: BF8C3F73
	ds_write_b32 v20, v103 offset:31200                        // 00000001F5A4: D83479E0 00006714
	s_waitcnt vmcnt(2)                                         // 00000001F5AC: BF8C3F72
	ds_write_b32 v21, v104 offset:31328                        // 00000001F5B0: D8347A60 00006815
	s_waitcnt vmcnt(1)                                         // 00000001F5B8: BF8C3F71
	ds_write_b32 v20, v61 offset:33280                         // 00000001F5BC: D8348200 00003D14
	s_waitcnt vmcnt(0)                                         // 00000001F5C4: BF8C3F70
	ds_write_b32 v21, v62 offset:33408                         // 00000001F5C8: D8348280 00003E15
	ds_write_b32 v31, v63 offset:35360                         // 00000001F5D0: D8348A20 00003F1F
	ds_write_b32 v38, v64 offset:35360                         // 00000001F5D8: D8348A20 00004026
	ds_write_b32 v41, v65 offset:35360                         // 00000001F5E0: D8348A20 00004129
	ds_write_b32 v42, v66 offset:35360                         // 00000001F5E8: D8348A20 0000422A
	ds_write2st64_b32 v181, v85, v86 offset1:4                 // 00000001F5F0: D83C0400 005655B5
	s_waitcnt lgkmcnt(0)                                       // 00000001F5F8: BF8CC07F
	s_barrier                                                  // 00000001F5FC: BF8A0000
	buffer_gl0_inv                                             // 00000001F600: E1C40000 00000000
	ds_read2_b32 v[53:54], v14 offset0:12 offset1:13           // 00000001F608: D8DC0D0C 3500000E
	ds_read2_b32 v[55:56], v52 offset0:8 offset1:9             // 00000001F610: D8DC0908 37000034
	ds_read2_b32 v[57:58], v52 offset0:10 offset1:11           // 00000001F618: D8DC0B0A 39000034
	ds_read2_b32 v[59:60], v52 offset0:12 offset1:13           // 00000001F620: D8DC0D0C 3B000034
	ds_read2_b32 v[61:62], v52 offset0:14 offset1:15           // 00000001F628: D8DC0F0E 3D000034
	ds_read2_b32 v[63:64], v51 offset0:40 offset1:41           // 00000001F630: D8DC2928 3F000033
	ds_read2_b32 v[65:66], v51 offset0:42 offset1:43           // 00000001F638: D8DC2B2A 41000033
	ds_read2_b32 v[67:68], v51 offset0:44 offset1:45           // 00000001F640: D8DC2D2C 43000033
	ds_read2_b32 v[69:70], v51 offset0:46 offset1:47           // 00000001F648: D8DC2F2E 45000033
	ds_read2_b32 v[71:72], v46 offset0:72 offset1:73           // 00000001F650: D8DC4948 4700002E
	ds_read2_b32 v[73:74], v46 offset0:74 offset1:75           // 00000001F658: D8DC4B4A 4900002E
	ds_read2_b32 v[75:76], v46 offset0:76 offset1:77           // 00000001F660: D8DC4D4C 4B00002E
	ds_read2_b32 v[77:78], v46 offset0:78 offset1:79           // 00000001F668: D8DC4F4E 4D00002E
	ds_read2_b32 v[79:80], v45 offset0:104 offset1:105         // 00000001F670: D8DC6968 4F00002D
	ds_read2_b32 v[81:82], v45 offset0:106 offset1:107         // 00000001F678: D8DC6B6A 5100002D
	ds_read2_b32 v[83:84], v45 offset0:108 offset1:109         // 00000001F680: D8DC6D6C 5300002D
	ds_read2_b32 v[85:86], v45 offset0:110 offset1:111         // 00000001F688: D8DC6F6E 5500002D
	ds_read2_b32 v[87:88], v14 offset0:20 offset1:21           // 00000001F690: D8DC1514 5700000E
	ds_read2_b32 v[89:90], v14 offset0:22 offset1:23           // 00000001F698: D8DC1716 5900000E
	ds_read2_b32 v[91:92], v52 offset0:16 offset1:17           // 00000001F6A0: D8DC1110 5B000034
	ds_read2_b32 v[93:94], v52 offset0:18 offset1:19           // 00000001F6A8: D8DC1312 5D000034
	ds_read2_b32 v[95:96], v52 offset0:20 offset1:21           // 00000001F6B0: D8DC1514 5F000034
	ds_read2_b32 v[97:98], v52 offset0:22 offset1:23           // 00000001F6B8: D8DC1716 61000034
	ds_read2_b32 v[99:100], v51 offset0:48 offset1:49          // 00000001F6C0: D8DC3130 63000033
	ds_read2_b32 v[101:102], v51 offset0:50 offset1:51         // 00000001F6C8: D8DC3332 65000033
	ds_read2_b32 v[103:104], v51 offset0:52 offset1:53         // 00000001F6D0: D8DC3534 67000033
	ds_read2_b32 v[105:106], v51 offset0:54 offset1:55         // 00000001F6D8: D8DC3736 69000033
	ds_read2_b32 v[107:108], v46 offset0:80 offset1:81         // 00000001F6E0: D8DC5150 6B00002E
	ds_read2_b32 v[109:110], v46 offset0:82 offset1:83         // 00000001F6E8: D8DC5352 6D00002E
	ds_read2_b32 v[111:112], v46 offset0:84 offset1:85         // 00000001F6F0: D8DC5554 6F00002E
	ds_read2_b32 v[113:114], v46 offset0:86 offset1:87         // 00000001F6F8: D8DC5756 7100002E
	ds_read2_b32 v[115:116], v45 offset0:112 offset1:113       // 00000001F700: D8DC7170 7300002D
	ds_read2_b32 v[117:118], v45 offset0:114 offset1:115       // 00000001F708: D8DC7372 7500002D
	ds_read2_b32 v[119:120], v45 offset0:116 offset1:117       // 00000001F710: D8DC7574 7700002D
	ds_read2_b32 v[121:122], v45 offset0:118 offset1:119       // 00000001F718: D8DC7776 7900002D
	ds_read2_b32 v[123:124], v14 offset0:28 offset1:29         // 00000001F720: D8DC1D1C 7B00000E
	ds_read2_b32 v[125:126], v14 offset0:30 offset1:31         // 00000001F728: D8DC1F1E 7D00000E
	ds_read2_b32 v[127:128], v52 offset0:24 offset1:25         // 00000001F730: D8DC1918 7F000034
	ds_read2_b32 v[129:130], v52 offset0:26 offset1:27         // 00000001F738: D8DC1B1A 81000034
	ds_read2_b32 v[131:132], v52 offset0:28 offset1:29         // 00000001F740: D8DC1D1C 83000034
	ds_read2_b32 v[133:134], v52 offset0:30 offset1:31         // 00000001F748: D8DC1F1E 85000034
	ds_read2_b32 v[135:136], v51 offset0:56 offset1:57         // 00000001F750: D8DC3938 87000033
	ds_read2_b32 v[137:138], v51 offset0:58 offset1:59         // 00000001F758: D8DC3B3A 89000033
	ds_read2_b32 v[139:140], v51 offset0:60 offset1:61         // 00000001F760: D8DC3D3C 8B000033
	ds_read2_b32 v[141:142], v51 offset0:62 offset1:63         // 00000001F768: D8DC3F3E 8D000033
	ds_read2_b32 v[143:144], v46 offset0:88 offset1:89         // 00000001F770: D8DC5958 8F00002E
	ds_read2_b32 v[145:146], v46 offset0:90 offset1:91         // 00000001F778: D8DC5B5A 9100002E
	ds_read2_b32 v[147:148], v46 offset0:92 offset1:93         // 00000001F780: D8DC5D5C 9300002E
	ds_read2_b32 v[149:150], v46 offset0:94 offset1:95         // 00000001F788: D8DC5F5E 9500002E
	ds_read2_b32 v[151:152], v45 offset0:120 offset1:121       // 00000001F790: D8DC7978 9700002D
	ds_read2_b32 v[153:154], v45 offset0:122 offset1:123       // 00000001F798: D8DC7B7A 9900002D
	ds_read2_b32 v[155:156], v45 offset0:124 offset1:125       // 00000001F7A0: D8DC7D7C 9B00002D
	ds_read2_b32 v[157:158], v45 offset0:126 offset1:127       // 00000001F7A8: D8DC7F7E 9D00002D
	ds_read2_b32 v[159:160], v14 offset0:36 offset1:37         // 00000001F7B0: D8DC2524 9F00000E
	ds_read2_b32 v[161:162], v14 offset0:38 offset1:39         // 00000001F7B8: D8DC2726 A100000E
	ds_read2_b32 v[163:164], v51 offset0:64 offset1:65         // 00000001F7C0: D8DC4140 A3000033
	ds_read2_b32 v[165:166], v52 offset0:32 offset1:33         // 00000001F7C8: D8DC2120 A5000034
	ds_read2_b32 v[167:168], v52 offset0:34 offset1:35         // 00000001F7D0: D8DC2322 A7000034
	ds_read2_b32 v[169:170], v52 offset0:36 offset1:37         // 00000001F7D8: D8DC2524 A9000034
	ds_read2_b32 v[171:172], v52 offset0:38 offset1:39         // 00000001F7E0: D8DC2726 AB000034
	ds_read2_b32 v[173:174], v46 offset0:96 offset1:97         // 00000001F7E8: D8DC6160 AD00002E
	ds_read2_b32 v[175:176], v14 offset0:14 offset1:15         // 00000001F7F0: D8DC0F0E AF00000E
	ds_read2_b32 v[177:178], v14 offset0:8 offset1:9           // 00000001F7F8: D8DC0908 B100000E
	ds_read2_b32 v[179:180], v14 offset0:10 offset1:11         // 00000001F800: D8DC0B0A B300000E
	s_waitcnt lgkmcnt(62)                                      // 00000001F808: BF8CFE7F
	v_mul_i32_i24_sdwa v198, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F80C: 138C6AF9 08080637
	v_mul_i32_i24_sdwa v199, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F814: 138E6AF9 09090637
	v_add3_u32 v182, v198, v199, v182                          // 00000001F81C: D76D00B6 06DB8FC6
	v_mul_i32_i24_sdwa v198, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F824: 138C6AF9 0A0A0637
	v_mul_i32_i24_sdwa v199, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F82C: 138E6AF9 0B0B0637
	v_add3_u32 v182, v198, v199, v182                          // 00000001F834: D76D00B6 06DB8FC6
	s_waitcnt lgkmcnt(58)                                      // 00000001F83C: BF8CFA7F
	v_mul_i32_i24_sdwa v198, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F840: 138C6AF9 0808063F
	v_mul_i32_i24_sdwa v199, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F848: 138E6AF9 0909063F
	v_add3_u32 v183, v198, v199, v183                          // 00000001F850: D76D00B7 06DF8FC6
	v_mul_i32_i24_sdwa v198, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F858: 138C6AF9 0A0A063F
	v_mul_i32_i24_sdwa v199, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F860: 138E6AF9 0B0B063F
	v_add3_u32 v183, v198, v199, v183                          // 00000001F868: D76D00B7 06DF8FC6
	s_waitcnt lgkmcnt(54)                                      // 00000001F870: BF8CF67F
	v_mul_i32_i24_sdwa v198, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F874: 138C6AF9 08080647
	v_mul_i32_i24_sdwa v199, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F87C: 138E6AF9 09090647
	v_add3_u32 v184, v198, v199, v184                          // 00000001F884: D76D00B8 06E38FC6
	v_mul_i32_i24_sdwa v198, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F88C: 138C6AF9 0A0A0647
	v_mul_i32_i24_sdwa v199, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F894: 138E6AF9 0B0B0647
	v_add3_u32 v184, v198, v199, v184                          // 00000001F89C: D76D00B8 06E38FC6
	s_waitcnt lgkmcnt(50)                                      // 00000001F8A4: BF8CF27F
	v_mul_i32_i24_sdwa v198, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F8A8: 138C6AF9 0808064F
	v_mul_i32_i24_sdwa v199, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F8B0: 138E6AF9 0909064F
	v_add3_u32 v185, v198, v199, v185                          // 00000001F8B8: D76D00B9 06E78FC6
	v_mul_i32_i24_sdwa v198, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F8C0: 138C6AF9 0A0A064F
	v_mul_i32_i24_sdwa v199, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F8C8: 138E6AF9 0B0B064F
	v_add3_u32 v185, v198, v199, v185                          // 00000001F8D0: D76D00B9 06E78FC6
	s_waitcnt lgkmcnt(44)                                      // 00000001F8D8: BF8CEC7F
	v_mul_i32_i24_sdwa v198, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F8DC: 138CAEF9 0808065B
	v_mul_i32_i24_sdwa v199, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F8E4: 138EAEF9 0909065B
	v_add3_u32 v186, v198, v199, v186                          // 00000001F8EC: D76D00BA 06EB8FC6
	v_mul_i32_i24_sdwa v198, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F8F4: 138CAEF9 0A0A065B
	v_mul_i32_i24_sdwa v199, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F8FC: 138EAEF9 0B0B065B
	v_add3_u32 v186, v198, v199, v186                          // 00000001F904: D76D00BA 06EB8FC6
	s_waitcnt lgkmcnt(22)                                      // 00000001F90C: BF8CD67F
	v_mul_i32_i24_sdwa v198, sext(v135), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F910: 138CF6F9 08080687
	v_mul_i32_i24_sdwa v199, sext(v135), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F918: 138EF6F9 09090687
	v_add3_u32 v191, v198, v199, v191                          // 00000001F920: D76D00BF 06FF8FC6
	v_mul_i32_i24_sdwa v198, sext(v135), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F928: 138CF6F9 0A0A0687
	v_mul_i32_i24_sdwa v199, sext(v135), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F930: 138EF6F9 0B0B0687
	v_add3_u32 v191, v198, v199, v191                          // 00000001F938: D76D00BF 06FF8FC6
	s_waitcnt lgkmcnt(18)                                      // 00000001F940: BF8CD27F
	v_mul_i32_i24_sdwa v198, sext(v143), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F944: 138CF6F9 0808068F
	v_mul_i32_i24_sdwa v199, sext(v143), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F94C: 138EF6F9 0909068F
	v_add3_u32 v192, v198, v199, v192                          // 00000001F954: D76D00C0 07038FC6
	v_mul_i32_i24_sdwa v198, sext(v143), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F95C: 138CF6F9 0A0A068F
	v_mul_i32_i24_sdwa v199, sext(v143), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F964: 138EF6F9 0B0B068F
	v_add3_u32 v192, v198, v199, v192                          // 00000001F96C: D76D00C0 07038FC6
	s_waitcnt lgkmcnt(7)                                       // 00000001F974: BF8CC77F
	v_mul_i32_i24_sdwa v198, sext(v165), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F978: 138D3EF9 080806A5
	v_mul_i32_i24_sdwa v199, sext(v165), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F980: 138F3EF9 090906A5
	v_add3_u32 v194, v198, v199, v194                          // 00000001F988: D76D00C2 070B8FC6
	v_mul_i32_i24_sdwa v198, sext(v165), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F990: 138D3EF9 0A0A06A5
	v_mul_i32_i24_sdwa v199, sext(v165), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F998: 138F3EF9 0B0B06A5
	v_add3_u32 v194, v198, v199, v194                          // 00000001F9A0: D76D00C2 070B8FC6
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F9A8: 126E6CF9 08080638
	v_mul_i32_i24_sdwa v198, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F9B0: 138C6CF9 09090638
	v_add3_u32 v182, v55, v198, v182                           // 00000001F9B8: D76D00B6 06DB8D37
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F9C0: 126E6CF9 0A0A0638
	v_mul_i32_i24_sdwa v198, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F9C8: 138C6CF9 0B0B0638
	v_add3_u32 v182, v55, v198, v182                           // 00000001F9D0: D76D00B6 06DB8D37
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F9D8: 127E6CF9 08080640
	v_mul_i32_i24_sdwa v198, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001F9E0: 138C6CF9 09090640
	v_add3_u32 v183, v63, v198, v183                           // 00000001F9E8: D76D00B7 06DF8D3F
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F9F0: 127E6CF9 0A0A0640
	v_mul_i32_i24_sdwa v198, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F9F8: 138C6CF9 0B0B0640
	v_add3_u32 v183, v63, v198, v183                           // 00000001FA00: D76D00B7 06DF8D3F
	v_mul_i32_i24_sdwa v71, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA08: 128E6CF9 08080648
	v_mul_i32_i24_sdwa v198, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FA10: 138C6CF9 09090648
	v_add3_u32 v184, v71, v198, v184                           // 00000001FA18: D76D00B8 06E38D47
	v_mul_i32_i24_sdwa v71, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA20: 128E6CF9 0A0A0648
	v_mul_i32_i24_sdwa v198, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA28: 138C6CF9 0B0B0648
	v_add3_u32 v184, v71, v198, v184                           // 00000001FA30: D76D00B8 06E38D47
	v_mul_i32_i24_sdwa v53, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA38: 126A6CF9 08080650
	v_mul_i32_i24_sdwa v79, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FA40: 129E6CF9 09090650
	v_add3_u32 v185, v53, v79, v185                            // 00000001FA48: D76D00B9 06E69F35
	v_mul_i32_i24_sdwa v53, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA50: 126A6CF9 0A0A0650
	v_mul_i32_i24_sdwa v79, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA58: 129E6CF9 0B0B0650
	v_add3_u32 v185, v53, v79, v185                            // 00000001FA60: D76D00B9 06E69F35
	v_mul_i32_i24_sdwa v91, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA68: 12B6B0F9 0808065C
	v_mul_i32_i24_sdwa v198, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FA70: 138CB0F9 0909065C
	v_add3_u32 v186, v91, v198, v186                           // 00000001FA78: D76D00BA 06EB8D5B
	v_mul_i32_i24_sdwa v91, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA80: 12B6B0F9 0A0A065C
	v_mul_i32_i24_sdwa v198, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA88: 138CB0F9 0B0B065C
	v_add3_u32 v186, v91, v198, v186                           // 00000001FA90: D76D00BA 06EB8D5B
	v_mul_i32_i24_sdwa v135, sext(v136), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA98: 130EF8F9 08080688
	v_mul_i32_i24_sdwa v198, sext(v136), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FAA0: 138CF8F9 09090688
	v_add3_u32 v191, v135, v198, v191                          // 00000001FAA8: D76D00BF 06FF8D87
	v_mul_i32_i24_sdwa v135, sext(v136), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAB0: 130EF8F9 0A0A0688
	v_mul_i32_i24_sdwa v198, sext(v136), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FAB8: 138CF8F9 0B0B0688
	v_add3_u32 v191, v135, v198, v191                          // 00000001FAC0: D76D00BF 06FF8D87
	v_mul_i32_i24_sdwa v143, sext(v144), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FAC8: 131EF8F9 08080690
	v_mul_i32_i24_sdwa v198, sext(v144), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FAD0: 138CF8F9 09090690
	v_add3_u32 v192, v143, v198, v192                          // 00000001FAD8: D76D00C0 07038D8F
	v_mul_i32_i24_sdwa v143, sext(v144), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAE0: 131EF8F9 0A0A0690
	v_mul_i32_i24_sdwa v198, sext(v144), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FAE8: 138CF8F9 0B0B0690
	v_add3_u32 v192, v143, v198, v192                          // 00000001FAF0: D76D00C0 07038D8F
	v_mul_i32_i24_sdwa v165, sext(v166), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FAF8: 134B40F9 080806A6
	v_mul_i32_i24_sdwa v198, sext(v166), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FB00: 138D40F9 090906A6
	v_add3_u32 v194, v165, v198, v194                          // 00000001FB08: D76D00C2 070B8DA5
	v_mul_i32_i24_sdwa v165, sext(v166), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FB10: 134B40F9 0A0A06A6
	v_mul_i32_i24_sdwa v198, sext(v166), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FB18: 138D40F9 0B0B06A6
	v_add3_u32 v194, v165, v198, v194                          // 00000001FB20: D76D00C2 070B8DA5
	ds_read2_b32 v[55:56], v45 offset0:128 offset1:129         // 00000001FB28: D8DC8180 3700002D
	ds_read2_b32 v[135:136], v45 offset0:130 offset1:131       // 00000001FB30: D8DC8382 8700002D
	ds_read2_b32 v[143:144], v45 offset0:132 offset1:133       // 00000001FB38: D8DC8584 8F00002D
	ds_read2_b32 v[165:166], v45 offset0:134 offset1:135       // 00000001FB40: D8DC8786 A500002D
	v_mul_i32_i24_sdwa v198, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB48: 138CAEF9 08080663
	v_mul_i32_i24_sdwa v199, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FB50: 138EAEF9 09090663
	v_add3_u32 v187, v198, v199, v187                          // 00000001FB58: D76D00BB 06EF8FC6
	v_mul_i32_i24_sdwa v198, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FB60: 138CAEF9 0A0A0663
	v_mul_i32_i24_sdwa v199, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FB68: 138EAEF9 0B0B0663
	v_add3_u32 v187, v198, v199, v187                          // 00000001FB70: D76D00BB 06EF8FC6
	v_mul_i32_i24_sdwa v198, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB78: 138CAEF9 0808066B
	v_mul_i32_i24_sdwa v199, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FB80: 138EAEF9 0909066B
	v_add3_u32 v188, v198, v199, v188                          // 00000001FB88: D76D00BC 06F38FC6
	v_mul_i32_i24_sdwa v198, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FB90: 138CAEF9 0A0A066B
	v_mul_i32_i24_sdwa v199, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FB98: 138EAEF9 0B0B066B
	v_add3_u32 v188, v198, v199, v188                          // 00000001FBA0: D76D00BC 06F38FC6
	v_mul_i32_i24_sdwa v198, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FBA8: 138CAEF9 08080673
	v_mul_i32_i24_sdwa v199, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FBB0: 138EAEF9 09090673
	v_add3_u32 v189, v198, v199, v189                          // 00000001FBB8: D76D00BD 06F78FC6
	v_mul_i32_i24_sdwa v198, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FBC0: 138CAEF9 0A0A0673
	v_mul_i32_i24_sdwa v199, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FBC8: 138EAEF9 0B0B0673
	v_add3_u32 v189, v198, v199, v189                          // 00000001FBD0: D76D00BD 06F78FC6
	v_mul_i32_i24_sdwa v198, sext(v127), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FBD8: 138CF6F9 0808067F
	v_mul_i32_i24_sdwa v199, sext(v127), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FBE0: 138EF6F9 0909067F
	v_add3_u32 v190, v198, v199, v190                          // 00000001FBE8: D76D00BE 06FB8FC6
	v_mul_i32_i24_sdwa v198, sext(v127), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FBF0: 138CF6F9 0A0A067F
	v_mul_i32_i24_sdwa v199, sext(v127), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FBF8: 138EF6F9 0B0B067F
	v_add3_u32 v190, v198, v199, v190                          // 00000001FC00: D76D00BE 06FB8FC6
	v_mul_i32_i24_sdwa v198, sext(v151), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC08: 138CF6F9 08080697
	v_mul_i32_i24_sdwa v199, sext(v151), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FC10: 138EF6F9 09090697
	v_add3_u32 v193, v198, v199, v193                          // 00000001FC18: D76D00C1 07078FC6
	v_mul_i32_i24_sdwa v198, sext(v151), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FC20: 138CF6F9 0A0A0697
	v_mul_i32_i24_sdwa v199, sext(v151), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FC28: 138EF6F9 0B0B0697
	v_add3_u32 v193, v198, v199, v193                          // 00000001FC30: D76D00C1 07078FC6
	v_mul_i32_i24_sdwa v198, sext(v163), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC38: 138D3EF9 080806A3
	v_mul_i32_i24_sdwa v199, sext(v163), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FC40: 138F3EF9 090906A3
	v_add3_u32 v195, v198, v199, v195                          // 00000001FC48: D76D00C3 070F8FC6
	v_mul_i32_i24_sdwa v198, sext(v163), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FC50: 138D3EF9 0A0A06A3
	v_mul_i32_i24_sdwa v199, sext(v163), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FC58: 138F3EF9 0B0B06A3
	v_add3_u32 v195, v198, v199, v195                          // 00000001FC60: D76D00C3 070F8FC6
	s_waitcnt lgkmcnt(7)                                       // 00000001FC68: BF8CC77F
	v_mul_i32_i24_sdwa v198, sext(v173), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC6C: 138D3EF9 080806AD
	v_mul_i32_i24_sdwa v199, sext(v173), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FC74: 138F3EF9 090906AD
	v_add3_u32 v196, v198, v199, v196                          // 00000001FC7C: D76D00C4 07138FC6
	v_mul_i32_i24_sdwa v198, sext(v173), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FC84: 138D3EF9 0A0A06AD
	v_mul_i32_i24_sdwa v199, sext(v173), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FC8C: 138F3EF9 0B0B06AD
	v_add3_u32 v196, v198, v199, v196                          // 00000001FC94: D76D00C4 07138FC6
	s_waitcnt lgkmcnt(3)                                       // 00000001FC9C: BF8CC37F
	v_mul_i32_i24_sdwa v198, sext(v55), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FCA0: 138D3EF9 08080637
	v_mul_i32_i24_sdwa v199, sext(v55), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FCA8: 138F3EF9 09090637
	v_add3_u32 v197, v198, v199, v197                          // 00000001FCB0: D76D00C5 07178FC6
	v_mul_i32_i24_sdwa v198, sext(v55), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FCB8: 138D3EF9 0A0A0637
	v_mul_i32_i24_sdwa v199, sext(v55), sext(v159) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FCC0: 138F3EF9 0B0B0637
	v_add3_u32 v197, v198, v199, v197                          // 00000001FCC8: D76D00C5 07178FC6
	v_mul_i32_i24_sdwa v198, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FCD0: 138D5EF9 08080639
	v_mul_i32_i24_sdwa v199, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FCD8: 138F5EF9 09090639
	v_add3_u32 v182, v198, v199, v182                          // 00000001FCE0: D76D00B6 06DB8FC6
	v_mul_i32_i24_sdwa v198, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FCE8: 138D5EF9 0A0A0639
	v_mul_i32_i24_sdwa v199, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FCF0: 138F5EF9 0B0B0639
	v_add3_u32 v182, v198, v199, v182                          // 00000001FCF8: D76D00B6 06DB8FC6
	v_mul_i32_i24_sdwa v198, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FD00: 138D5EF9 08080641
	v_mul_i32_i24_sdwa v199, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FD08: 138F5EF9 09090641
	v_add3_u32 v183, v198, v199, v183                          // 00000001FD10: D76D00B7 06DF8FC6
	v_mul_i32_i24_sdwa v198, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FD18: 138D5EF9 0A0A0641
	v_mul_i32_i24_sdwa v199, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FD20: 138F5EF9 0B0B0641
	v_add3_u32 v183, v198, v199, v183                          // 00000001FD28: D76D00B7 06DF8FC6
	v_mul_i32_i24_sdwa v198, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FD30: 138D5EF9 08080649
	v_mul_i32_i24_sdwa v199, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FD38: 138F5EF9 09090649
	v_add3_u32 v184, v198, v199, v184                          // 00000001FD40: D76D00B8 06E38FC6
	v_mul_i32_i24_sdwa v198, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FD48: 138D5EF9 0A0A0649
	v_mul_i32_i24_sdwa v199, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FD50: 138F5EF9 0B0B0649
	v_add3_u32 v184, v198, v199, v184                          // 00000001FD58: D76D00B8 06E38FC6
	v_mul_i32_i24_sdwa v198, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FD60: 138D5EF9 08080651
	v_mul_i32_i24_sdwa v199, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FD68: 138F5EF9 09090651
	v_add3_u32 v185, v198, v199, v185                          // 00000001FD70: D76D00B9 06E78FC6
	v_mul_i32_i24_sdwa v198, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FD78: 138D5EF9 0A0A0651
	v_mul_i32_i24_sdwa v199, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FD80: 138F5EF9 0B0B0651
	v_add3_u32 v185, v198, v199, v185                          // 00000001FD88: D76D00B9 06E78FC6
	v_mul_i32_i24_sdwa v198, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FD90: 138CB2F9 0808065D
	v_mul_i32_i24_sdwa v199, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FD98: 138EB2F9 0909065D
	v_add3_u32 v186, v198, v199, v186                          // 00000001FDA0: D76D00BA 06EB8FC6
	v_mul_i32_i24_sdwa v198, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FDA8: 138CB2F9 0A0A065D
	v_mul_i32_i24_sdwa v199, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FDB0: 138EB2F9 0B0B065D
	v_add3_u32 v186, v198, v199, v186                          // 00000001FDB8: D76D00BA 06EB8FC6
	v_mul_i32_i24_sdwa v99, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FDC0: 12C6B0F9 08080664
	v_mul_i32_i24_sdwa v198, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FDC8: 138CB0F9 09090664
	v_add3_u32 v187, v99, v198, v187                           // 00000001FDD0: D76D00BB 06EF8D63
	v_mul_i32_i24_sdwa v99, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FDD8: 12C6B0F9 0A0A0664
	v_mul_i32_i24_sdwa v198, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FDE0: 138CB0F9 0B0B0664
	v_add3_u32 v187, v99, v198, v187                           // 00000001FDE8: D76D00BB 06EF8D63
	v_mul_i32_i24_sdwa v107, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FDF0: 12D6B0F9 0808066C
	v_mul_i32_i24_sdwa v198, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FDF8: 138CB0F9 0909066C
	v_add3_u32 v188, v107, v198, v188                          // 00000001FE00: D76D00BC 06F38D6B
	v_mul_i32_i24_sdwa v107, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FE08: 12D6B0F9 0A0A066C
	v_mul_i32_i24_sdwa v198, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FE10: 138CB0F9 0B0B066C
	v_add3_u32 v188, v107, v198, v188                          // 00000001FE18: D76D00BC 06F38D6B
	v_mul_i32_i24_sdwa v87, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FE20: 12AEB0F9 08080674
	v_mul_i32_i24_sdwa v115, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FE28: 12E6B0F9 09090674
	v_add3_u32 v189, v87, v115, v189                           // 00000001FE30: D76D00BD 06F6E757
	v_mul_i32_i24_sdwa v87, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FE38: 12AEB0F9 0A0A0674
	v_mul_i32_i24_sdwa v115, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FE40: 12E6B0F9 0B0B0674
	v_add3_u32 v189, v87, v115, v189                           // 00000001FE48: D76D00BD 06F6E757
	v_mul_i32_i24_sdwa v127, sext(v128), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FE50: 12FEF8F9 08080680
	v_mul_i32_i24_sdwa v198, sext(v128), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FE58: 138CF8F9 09090680
	v_add3_u32 v190, v127, v198, v190                          // 00000001FE60: D76D00BE 06FB8D7F
	v_mul_i32_i24_sdwa v127, sext(v128), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FE68: 12FEF8F9 0A0A0680
	v_mul_i32_i24_sdwa v198, sext(v128), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FE70: 138CF8F9 0B0B0680
	v_add3_u32 v190, v127, v198, v190                          // 00000001FE78: D76D00BE 06FB8D7F
	v_mul_i32_i24_sdwa v123, sext(v152), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FE80: 12F6F8F9 08080698
	v_mul_i32_i24_sdwa v151, sext(v152), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FE88: 132EF8F9 09090698
	v_add3_u32 v193, v123, v151, v193                          // 00000001FE90: D76D00C1 07072F7B
	v_mul_i32_i24_sdwa v123, sext(v152), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FE98: 12F6F8F9 0A0A0698
	v_mul_i32_i24_sdwa v151, sext(v152), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FEA0: 132EF8F9 0B0B0698
	v_add3_u32 v193, v123, v151, v193                          // 00000001FEA8: D76D00C1 07072F7B
	v_mul_i32_i24_sdwa v163, sext(v164), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FEB0: 134740F9 080806A4
	v_mul_i32_i24_sdwa v198, sext(v164), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FEB8: 138D40F9 090906A4
	v_add3_u32 v195, v163, v198, v195                          // 00000001FEC0: D76D00C3 070F8DA3
	v_mul_i32_i24_sdwa v163, sext(v164), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FEC8: 134740F9 0A0A06A4
	v_mul_i32_i24_sdwa v198, sext(v164), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FED0: 138D40F9 0B0B06A4
	v_add3_u32 v195, v163, v198, v195                          // 00000001FED8: D76D00C3 070F8DA3
	v_mul_i32_i24_sdwa v173, sext(v174), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FEE0: 135B40F9 080806AE
	v_mul_i32_i24_sdwa v198, sext(v174), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FEE8: 138D40F9 090906AE
	v_add3_u32 v196, v173, v198, v196                          // 00000001FEF0: D76D00C4 07138DAD
	v_mul_i32_i24_sdwa v173, sext(v174), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FEF8: 135B40F9 0A0A06AE
	v_mul_i32_i24_sdwa v198, sext(v174), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FF00: 138D40F9 0B0B06AE
	v_add3_u32 v196, v173, v198, v196                          // 00000001FF08: D76D00C4 07138DAD
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FF10: 126F40F9 08080638
	v_mul_i32_i24_sdwa v159, sext(v56), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FF18: 133F40F9 09090638
	v_add3_u32 v197, v55, v159, v197                           // 00000001FF20: D76D00C5 07173F37
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FF28: 126F40F9 0A0A0638
	v_mul_i32_i24_sdwa v159, sext(v56), sext(v160) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FF30: 133F40F9 0B0B0638
	v_add3_u32 v197, v55, v159, v197                           // 00000001FF38: D76D00C5 07173F37
	v_mul_i32_i24_sdwa v57, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FF40: 127360F9 0808063A
	v_mul_i32_i24_sdwa v198, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FF48: 138D60F9 0909063A
	v_add3_u32 v182, v57, v198, v182                           // 00000001FF50: D76D00B6 06DB8D39
	v_mul_i32_i24_sdwa v57, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FF58: 127360F9 0A0A063A
	v_mul_i32_i24_sdwa v198, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FF60: 138D60F9 0B0B063A
	v_add3_u32 v182, v57, v198, v182                           // 00000001FF68: D76D00B6 06DB8D39
	v_mul_i32_i24_sdwa v65, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FF70: 128360F9 08080642
	v_mul_i32_i24_sdwa v198, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FF78: 138D60F9 09090642
	v_add3_u32 v183, v65, v198, v183                           // 00000001FF80: D76D00B7 06DF8D41
	v_mul_i32_i24_sdwa v65, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FF88: 128360F9 0A0A0642
	v_mul_i32_i24_sdwa v198, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FF90: 138D60F9 0B0B0642
	v_add3_u32 v183, v65, v198, v183                           // 00000001FF98: D76D00B7 06DF8D41
	v_mul_i32_i24_sdwa v73, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FFA0: 129360F9 0808064A
	v_mul_i32_i24_sdwa v198, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FFA8: 138D60F9 0909064A
	v_add3_u32 v184, v73, v198, v184                           // 00000001FFB0: D76D00B8 06E38D49
	v_mul_i32_i24_sdwa v73, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FFB8: 129360F9 0A0A064A
	v_mul_i32_i24_sdwa v198, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FFC0: 138D60F9 0B0B064A
	v_add3_u32 v184, v73, v198, v184                           // 00000001FFC8: D76D00B8 06E38D49
	v_mul_i32_i24_sdwa v81, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FFD0: 12A360F9 08080652
	v_mul_i32_i24_sdwa v175, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000001FFD8: 135F60F9 09090652
	v_add3_u32 v185, v81, v175, v185                           // 00000001FFE0: D76D00B9 06E75F51
	v_mul_i32_i24_sdwa v81, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FFE8: 12A360F9 0A0A0652
	v_mul_i32_i24_sdwa v175, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FFF0: 135F60F9 0B0B0652
	v_add3_u32 v185, v81, v175, v185                           // 00000001FFF8: D76D00B9 06E75F51
	v_mul_i32_i24_sdwa v93, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020000: 12BAB4F9 0808065E
	v_mul_i32_i24_sdwa v198, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020008: 138CB4F9 0909065E
	v_add3_u32 v186, v93, v198, v186                           // 000000020010: D76D00BA 06EB8D5D
	v_mul_i32_i24_sdwa v93, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020018: 12BAB4F9 0A0A065E
	v_mul_i32_i24_sdwa v198, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020020: 138CB4F9 0B0B065E
	v_add3_u32 v186, v93, v198, v186                           // 000000020028: D76D00BA 06EB8D5D
	ds_read2_b32 v[63:64], v51 offset0:66 offset1:67           // 000000020030: D8DC4342 3F000033
	ds_read2_b32 v[71:72], v51 offset0:68 offset1:69           // 000000020038: D8DC4544 47000033
	ds_read2_b32 v[91:92], v51 offset0:70 offset1:71           // 000000020040: D8DC4746 5B000033
	ds_read2_b32 v[53:54], v14 offset0:16 offset1:17           // 000000020048: D8DC1110 3500000E
	ds_read2_b32 v[79:80], v14 offset0:18 offset1:19           // 000000020050: D8DC1312 4F00000E
	ds_read2_b32 v[99:100], v46 offset0:98 offset1:99          // 000000020058: D8DC6362 6300002E
	ds_read2_b32 v[107:108], v46 offset0:100 offset1:101       // 000000020060: D8DC6564 6B00002E
	ds_read2_b32 v[127:128], v46 offset0:102 offset1:103       // 000000020068: D8DC6766 7F00002E
	ds_read2_b32 v[87:88], v14 offset0:24 offset1:25           // 000000020070: D8DC1918 5700000E
	ds_read2_b32 v[115:116], v14 offset0:26 offset1:27         // 000000020078: D8DC1B1A 7300000E
	ds_read2_b32 v[123:124], v14 offset0:32 offset1:33         // 000000020080: D8DC2120 7B00000E
	ds_read2_b32 v[151:152], v14 offset0:34 offset1:35         // 000000020088: D8DC2322 9700000E
	ds_read2_b32 v[163:164], v49 offset0:136 offset1:137       // 000000020090: D8DC8988 A3000031
	ds_read2_b32 v[65:66], v49 offset0:138 offset1:139         // 000000020098: D8DC8B8A 41000031
	ds_read2_b32 v[173:174], v48 offset0:136 offset1:137       // 0000000200A0: D8DC8988 AD000030
	ds_read2_b32 v[73:74], v48 offset0:138 offset1:139         // 0000000200A8: D8DC8B8A 49000030
	ds_read2_b32 v[55:56], v50 offset0:136 offset1:137         // 0000000200B0: D8DC8988 37000032
	ds_read2_b32 v[159:160], v50 offset0:138 offset1:139       // 0000000200B8: D8DC8B8A 9F000032
	ds_read2_b32 v[57:58], v47 offset0:136 offset1:137         // 0000000200C0: D8DC8988 3900002F
	ds_read2_b32 v[81:82], v47 offset0:138 offset1:139         // 0000000200C8: D8DC8B8A 5100002F
	ds_read2_b32 v[175:176], v14 offset0:40 offset1:41         // 0000000200D0: D8DC2928 AF00000E
	ds_read2_b32 v[93:94], v14 offset0:42 offset1:43           // 0000000200D8: D8DC2B2A 5D00000E
	v_mul_i32_i24_sdwa v198, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000200E0: 138CB2F9 08080665
	v_mul_i32_i24_sdwa v199, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000200E8: 138EB2F9 09090665
	v_add3_u32 v187, v198, v199, v187                          // 0000000200F0: D76D00BB 06EF8FC6
	v_mul_i32_i24_sdwa v198, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000200F8: 138CB2F9 0A0A0665
	v_mul_i32_i24_sdwa v199, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020100: 138EB2F9 0B0B0665
	v_add3_u32 v187, v198, v199, v187                          // 000000020108: D76D00BB 06EF8FC6
	v_mul_i32_i24_sdwa v101, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020110: 12CAB4F9 08080666
	v_mul_i32_i24_sdwa v198, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020118: 138CB4F9 09090666
	v_add3_u32 v187, v101, v198, v187                          // 000000020120: D76D00BB 06EF8D65
	v_mul_i32_i24_sdwa v101, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020128: 12CAB4F9 0A0A0666
	v_mul_i32_i24_sdwa v198, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020130: 138CB4F9 0B0B0666
	v_add3_u32 v187, v101, v198, v187                          // 000000020138: D76D00BB 06EF8D65
	v_mul_i32_i24_sdwa v198, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020140: 138CB2F9 0808066D
	v_mul_i32_i24_sdwa v199, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020148: 138EB2F9 0909066D
	v_add3_u32 v188, v198, v199, v188                          // 000000020150: D76D00BC 06F38FC6
	v_mul_i32_i24_sdwa v198, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020158: 138CB2F9 0A0A066D
	v_mul_i32_i24_sdwa v199, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020160: 138EB2F9 0B0B066D
	v_add3_u32 v188, v198, v199, v188                          // 000000020168: D76D00BC 06F38FC6
	v_mul_i32_i24_sdwa v109, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020170: 12DAB4F9 0808066E
	v_mul_i32_i24_sdwa v198, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020178: 138CB4F9 0909066E
	v_add3_u32 v188, v109, v198, v188                          // 000000020180: D76D00BC 06F38D6D
	v_mul_i32_i24_sdwa v109, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020188: 12DAB4F9 0A0A066E
	v_mul_i32_i24_sdwa v198, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020190: 138CB4F9 0B0B066E
	v_add3_u32 v188, v109, v198, v188                          // 000000020198: D76D00BC 06F38D6D
	v_mul_i32_i24_sdwa v109, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000201A0: 12DAB2F9 08080675
	v_mul_i32_i24_sdwa v110, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000201A8: 12DCB2F9 09090675
	v_add3_u32 v189, v109, v110, v189                          // 0000000201B0: D76D00BD 06F6DD6D
	v_mul_i32_i24_sdwa v109, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000201B8: 12DAB2F9 0A0A0675
	v_mul_i32_i24_sdwa v110, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000201C0: 12DCB2F9 0B0B0675
	v_add3_u32 v189, v109, v110, v189                          // 0000000201C8: D76D00BD 06F6DD6D
	v_mul_i32_i24_sdwa v89, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000201D0: 12B2B4F9 08080676
	v_mul_i32_i24_sdwa v109, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000201D8: 12DAB4F9 09090676
	v_add3_u32 v189, v89, v109, v189                           // 0000000201E0: D76D00BD 06F6DB59
	v_mul_i32_i24_sdwa v89, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000201E8: 12B2B4F9 0A0A0676
	v_mul_i32_i24_sdwa v109, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000201F0: 12DAB4F9 0B0B0676
	v_add3_u32 v189, v89, v109, v189                           // 0000000201F8: D76D00BD 06F6DB59
	v_mov_b32_e32 v198, 0                                      // 000000020200: 7F8C0280
	v_mov_b32_e32 v199, 0                                      // 000000020204: 7F8E0280
	s_waitcnt lgkmcnt(9)                                       // 000000020208: BF8CC97F
	v_mul_f32_e32 v89, v163, v177                              // 00000002020C: 10B363A3
	v_mul_f32_e32 v90, v164, v178                              // 000000020210: 10B565A4
	s_waitcnt lgkmcnt(7)                                       // 000000020214: BF8CC77F
	v_mul_f32_e32 v109, v173, v177                             // 000000020218: 10DB63AD
	s_waitcnt lgkmcnt(5)                                       // 00000002021C: BF8CC57F
	v_mul_f32_e32 v55, v55, v177                               // 000000020220: 106F6337
	s_waitcnt lgkmcnt(4)                                       // 000000020224: BF8CC47F
	v_mul_f32_e32 v117, v159, v179                             // 000000020228: 10EB679F
	v_mul_f32_e32 v118, v160, v180                             // 00000002022C: 10ED69A0
	v_mul_i32_i24_sdwa v159, sext(v129), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020230: 133EFAF9 08080681
	v_mul_i32_i24_sdwa v160, sext(v129), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020238: 1340FAF9 09090681
	v_add3_u32 v190, v159, v160, v190                          // 000000020240: D76D00BE 06FB419F
	v_mul_i32_i24_sdwa v159, sext(v129), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020248: 133EFAF9 0A0A0681
	v_mul_i32_i24_sdwa v160, sext(v129), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020250: 1340FAF9 0B0B0681
	v_add3_u32 v190, v159, v160, v190                          // 000000020258: D76D00BE 06FB419F
	v_mul_i32_i24_sdwa v129, sext(v130), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020260: 1302FCF9 08080682
	v_mul_i32_i24_sdwa v159, sext(v130), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020268: 133EFCF9 09090682
	v_add3_u32 v190, v129, v159, v190                          // 000000020270: D76D00BE 06FB3F81
	v_mul_i32_i24_sdwa v129, sext(v130), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020278: 1302FCF9 0A0A0682
	v_mul_i32_i24_sdwa v159, sext(v130), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020280: 133EFCF9 0B0B0682
	v_add3_u32 v190, v129, v159, v190                          // 000000020288: D76D00BE 06FB3F81
	v_mul_i32_i24_sdwa v129, sext(v137), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020290: 1302FAF9 08080689
	v_mul_i32_i24_sdwa v130, sext(v137), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020298: 1304FAF9 09090689
	v_add3_u32 v191, v129, v130, v191                          // 0000000202A0: D76D00BF 06FF0581
	v_mul_i32_i24_sdwa v129, sext(v137), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000202A8: 1302FAF9 0A0A0689
	v_mul_i32_i24_sdwa v130, sext(v137), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000202B0: 1304FAF9 0B0B0689
	v_add3_u32 v191, v129, v130, v191                          // 0000000202B8: D76D00BF 06FF0581
	v_mul_i32_i24_sdwa v129, sext(v145), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000202C0: 1302FAF9 08080691
	v_mul_i32_i24_sdwa v130, sext(v145), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000202C8: 1304FAF9 09090691
	v_add3_u32 v192, v129, v130, v192                          // 0000000202D0: D76D00C0 07030581
	v_mul_i32_i24_sdwa v129, sext(v145), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000202D8: 1302FAF9 0A0A0691
	v_mul_i32_i24_sdwa v130, sext(v145), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000202E0: 1304FAF9 0B0B0691
	v_add3_u32 v192, v129, v130, v192                          // 0000000202E8: D76D00C0 07030581
	v_mul_i32_i24_sdwa v129, sext(v153), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000202F0: 1302FAF9 08080699
	v_mul_i32_i24_sdwa v130, sext(v153), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000202F8: 1304FAF9 09090699
	v_add3_u32 v193, v129, v130, v193                          // 000000020300: D76D00C1 07070581
	v_mul_i32_i24_sdwa v129, sext(v153), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020308: 1302FAF9 0A0A0699
	v_mul_i32_i24_sdwa v130, sext(v153), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020310: 1304FAF9 0B0B0699
	v_add3_u32 v193, v129, v130, v193                          // 000000020318: D76D00C1 07070581
	v_mul_i32_i24_sdwa v125, sext(v167), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020320: 12FB42F9 080806A7
	v_mul_i32_i24_sdwa v129, sext(v167), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020328: 130342F9 090906A7
	v_add3_u32 v194, v125, v129, v194                          // 000000020330: D76D00C2 070B037D
	v_mul_i32_i24_sdwa v125, sext(v167), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020338: 12FB42F9 0A0A06A7
	v_mul_i32_i24_sdwa v129, sext(v167), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020340: 130342F9 0B0B06A7
	v_add3_u32 v194, v125, v129, v194                          // 000000020348: D76D00C2 070B037D
	v_mul_i32_i24_sdwa v125, sext(v63), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020350: 12FB42F9 0808063F
	v_mul_i32_i24_sdwa v129, sext(v63), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020358: 130342F9 0909063F
	v_add3_u32 v195, v125, v129, v195                          // 000000020360: D76D00C3 070F037D
	v_mul_i32_i24_sdwa v125, sext(v63), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020368: 12FB42F9 0A0A063F
	v_mul_i32_i24_sdwa v129, sext(v63), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020370: 130342F9 0B0B063F
	v_add3_u32 v195, v125, v129, v195                          // 000000020378: D76D00C3 070F037D
	v_mul_i32_i24_sdwa v63, sext(v99), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020380: 127F42F9 08080663
	v_mul_i32_i24_sdwa v125, sext(v99), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020388: 12FB42F9 09090663
	v_add3_u32 v196, v63, v125, v196                           // 000000020390: D76D00C4 0712FB3F
	v_mul_i32_i24_sdwa v63, sext(v99), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020398: 127F42F9 0A0A0663
	v_mul_i32_i24_sdwa v125, sext(v99), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000203A0: 12FB42F9 0B0B0663
	v_add3_u32 v196, v63, v125, v196                           // 0000000203A8: D76D00C4 0712FB3F
	v_mul_i32_i24_sdwa v63, sext(v135), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203B0: 127F42F9 08080687
	v_mul_i32_i24_sdwa v99, sext(v135), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000203B8: 12C742F9 09090687
	v_add3_u32 v197, v63, v99, v197                            // 0000000203C0: D76D00C5 0716C73F
	v_mul_i32_i24_sdwa v63, sext(v135), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000203C8: 127F42F9 0A0A0687
	v_mul_i32_i24_sdwa v99, sext(v135), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000203D0: 12C742F9 0B0B0687
	v_add3_u32 v197, v63, v99, v197                            // 0000000203D8: D76D00C5 0716C73F
	v_mul_i32_i24_sdwa v129, sext(v138), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203E0: 1302FCF9 0808068A
	v_mul_i32_i24_sdwa v130, sext(v138), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000203E8: 1304FCF9 0909068A
	v_add3_u32 v191, v129, v130, v191                          // 0000000203F0: D76D00BF 06FF0581
	v_mul_i32_i24_sdwa v129, sext(v138), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000203F8: 1302FCF9 0A0A068A
	v_mul_i32_i24_sdwa v130, sext(v138), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020400: 1304FCF9 0B0B068A
	v_add3_u32 v191, v129, v130, v191                          // 000000020408: D76D00BF 06FF0581
	v_mul_i32_i24_sdwa v129, sext(v146), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020410: 1302FCF9 08080692
	v_mul_i32_i24_sdwa v130, sext(v146), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020418: 1304FCF9 09090692
	v_add3_u32 v192, v129, v130, v192                          // 000000020420: D76D00C0 07030581
	v_mul_i32_i24_sdwa v129, sext(v146), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020428: 1302FCF9 0A0A0692
	v_mul_i32_i24_sdwa v130, sext(v146), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020430: 1304FCF9 0B0B0692
	v_add3_u32 v192, v129, v130, v192                          // 000000020438: D76D00C0 07030581
	v_mul_i32_i24_sdwa v63, sext(v154), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020440: 127EFCF9 0808069A
	v_mul_i32_i24_sdwa v99, sext(v154), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020448: 12C6FCF9 0909069A
	v_add3_u32 v193, v63, v99, v193                            // 000000020450: D76D00C1 0706C73F
	v_mul_i32_i24_sdwa v63, sext(v154), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020458: 127EFCF9 0A0A069A
	v_mul_i32_i24_sdwa v99, sext(v154), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020460: 12C6FCF9 0B0B069A
	v_add3_u32 v193, v63, v99, v193                            // 000000020468: D76D00C1 0706C73F
	v_mul_i32_i24_sdwa v63, sext(v168), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020470: 127F44F9 080806A8
	v_mul_i32_i24_sdwa v99, sext(v168), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020478: 12C744F9 090906A8
	v_add3_u32 v194, v63, v99, v194                            // 000000020480: D76D00C2 070AC73F
	v_mul_i32_i24_sdwa v63, sext(v168), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020488: 127F44F9 0A0A06A8
	v_mul_i32_i24_sdwa v99, sext(v168), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020490: 12C744F9 0B0B06A8
	v_add3_u32 v194, v63, v99, v194                            // 000000020498: D76D00C2 070AC73F
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204A0: 127F44F9 08080640
	v_mul_i32_i24_sdwa v99, sext(v64), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000204A8: 12C744F9 09090640
	v_add3_u32 v195, v63, v99, v195                            // 0000000204B0: D76D00C3 070EC73F
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000204B8: 127F44F9 0A0A0640
	v_mul_i32_i24_sdwa v99, sext(v64), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000204C0: 12C744F9 0B0B0640
	v_add3_u32 v195, v63, v99, v195                            // 0000000204C8: D76D00C3 070EC73F
	v_mul_i32_i24_sdwa v63, sext(v100), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204D0: 127F44F9 08080664
	v_mul_i32_i24_sdwa v64, sext(v100), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000204D8: 128144F9 09090664
	v_add3_u32 v196, v63, v64, v196                            // 0000000204E0: D76D00C4 0712813F
	v_mul_i32_i24_sdwa v63, sext(v100), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000204E8: 127F44F9 0A0A0664
	v_mul_i32_i24_sdwa v64, sext(v100), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000204F0: 128144F9 0B0B0664
	v_add3_u32 v196, v63, v64, v196                            // 0000000204F8: D76D00C4 0712813F
	v_mul_i32_i24_sdwa v63, sext(v136), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020500: 127F44F9 08080688
	v_mul_i32_i24_sdwa v64, sext(v136), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020508: 128144F9 09090688
	v_add3_u32 v197, v63, v64, v197                            // 000000020510: D76D00C5 0716813F
	v_mul_i32_i24_sdwa v63, sext(v136), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020518: 127F44F9 0A0A0688
	v_mul_i32_i24_sdwa v64, sext(v136), sext(v162) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020520: 128144F9 0B0B0688
	v_add3_u32 v197, v63, v64, v197                            // 000000020528: D76D00C5 0716813F
	v_mul_i32_i24_sdwa v63, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020530: 127E6AF9 0808063B
	v_mul_i32_i24_sdwa v64, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020538: 12806AF9 0909063B
	v_add3_u32 v182, v63, v64, v182                            // 000000020540: D76D00B6 06DA813F
	v_mul_i32_i24_sdwa v63, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020548: 127E6AF9 0A0A063B
	v_mul_i32_i24_sdwa v64, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020550: 12806AF9 0B0B063B
	v_add3_u32 v182, v63, v64, v182                            // 000000020558: D76D00B6 06DA813F
	v_mul_i32_i24_sdwa v59, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020560: 12766AF9 08080643
	v_mul_i32_i24_sdwa v63, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020568: 127E6AF9 09090643
	v_add3_u32 v183, v59, v63, v183                            // 000000020570: D76D00B7 06DE7F3B
	v_mul_i32_i24_sdwa v59, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020578: 12766AF9 0A0A0643
	v_mul_i32_i24_sdwa v63, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020580: 127E6AF9 0B0B0643
	v_add3_u32 v183, v59, v63, v183                            // 000000020588: D76D00B7 06DE7F3B
	v_mul_i32_i24_sdwa v59, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020590: 12766AF9 0808064B
	v_mul_i32_i24_sdwa v63, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020598: 127E6AF9 0909064B
	v_add3_u32 v184, v59, v63, v184                            // 0000000205A0: D76D00B8 06E27F3B
	v_mul_i32_i24_sdwa v59, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000205A8: 12766AF9 0A0A064B
	v_mul_i32_i24_sdwa v63, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000205B0: 127E6AF9 0B0B064B
	v_add3_u32 v184, v59, v63, v184                            // 0000000205B8: D76D00B8 06E27F3B
	v_mul_i32_i24_sdwa v59, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000205C0: 12766AF9 08080653
	v_mul_i32_i24_sdwa v63, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000205C8: 127E6AF9 09090653
	v_add3_u32 v185, v59, v63, v185                            // 0000000205D0: D76D00B9 06E67F3B
	v_mul_i32_i24_sdwa v59, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000205D8: 12766AF9 0A0A0653
	v_mul_i32_i24_sdwa v63, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000205E0: 127E6AF9 0B0B0653
	v_add3_u32 v185, v59, v63, v185                            // 0000000205E8: D76D00B9 06E67F3B
	v_mul_i32_i24_sdwa v53, sext(v95), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000205F0: 126AAEF9 0808065F
	v_mul_i32_i24_sdwa v59, sext(v95), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000205F8: 1276AEF9 0909065F
	v_add3_u32 v186, v53, v59, v186                            // 000000020600: D76D00BA 06EA7735
	v_mul_i32_i24_sdwa v53, sext(v95), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020608: 126AAEF9 0A0A065F
	v_mul_i32_i24_sdwa v59, sext(v95), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020610: 1276AEF9 0B0B065F
	v_add3_u32 v186, v53, v59, v186                            // 000000020618: D76D00BA 06EA7735
	v_mul_i32_i24_sdwa v53, sext(v103), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020620: 126AAEF9 08080667
	v_mul_i32_i24_sdwa v59, sext(v103), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020628: 1276AEF9 09090667
	v_add3_u32 v187, v53, v59, v187                            // 000000020630: D76D00BB 06EE7735
	v_mul_i32_i24_sdwa v53, sext(v103), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020638: 126AAEF9 0A0A0667
	v_mul_i32_i24_sdwa v59, sext(v103), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020640: 1276AEF9 0B0B0667
	v_add3_u32 v187, v53, v59, v187                            // 000000020648: D76D00BB 06EE7735
	v_mul_i32_i24_sdwa v53, sext(v111), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020650: 126AAEF9 0808066F
	v_mul_i32_i24_sdwa v59, sext(v111), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020658: 1276AEF9 0909066F
	v_add3_u32 v188, v53, v59, v188                            // 000000020660: D76D00BC 06F27735
	v_mul_i32_i24_sdwa v53, sext(v111), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020668: 126AAEF9 0A0A066F
	v_mul_i32_i24_sdwa v59, sext(v111), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020670: 1276AEF9 0B0B066F
	v_add3_u32 v188, v53, v59, v188                            // 000000020678: D76D00BC 06F27735
	v_mul_i32_i24_sdwa v53, sext(v119), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020680: 126AAEF9 08080677
	v_mul_i32_i24_sdwa v59, sext(v119), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020688: 1276AEF9 09090677
	v_add3_u32 v189, v53, v59, v189                            // 000000020690: D76D00BD 06F67735
	v_mul_i32_i24_sdwa v53, sext(v119), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020698: 126AAEF9 0A0A0677
	v_mul_i32_i24_sdwa v59, sext(v119), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000206A0: 1276AEF9 0B0B0677
	v_add3_u32 v189, v53, v59, v189                            // 0000000206A8: D76D00BD 06F67735
	v_mul_i32_i24_sdwa v53, sext(v131), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000206B0: 126AF6F9 08080683
	v_mul_i32_i24_sdwa v59, sext(v131), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000206B8: 1276F6F9 09090683
	v_add3_u32 v190, v53, v59, v190                            // 0000000206C0: D76D00BE 06FA7735
	v_mul_i32_i24_sdwa v53, sext(v131), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000206C8: 126AF6F9 0A0A0683
	v_mul_i32_i24_sdwa v59, sext(v131), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000206D0: 1276F6F9 0B0B0683
	v_add3_u32 v190, v53, v59, v190                            // 0000000206D8: D76D00BE 06FA7735
	v_mul_i32_i24_sdwa v53, sext(v139), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000206E0: 126AF6F9 0808068B
	v_mul_i32_i24_sdwa v59, sext(v139), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000206E8: 1276F6F9 0909068B
	v_add3_u32 v191, v53, v59, v191                            // 0000000206F0: D76D00BF 06FE7735
	v_mul_i32_i24_sdwa v53, sext(v139), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000206F8: 126AF6F9 0A0A068B
	v_mul_i32_i24_sdwa v59, sext(v139), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020700: 1276F6F9 0B0B068B
	v_add3_u32 v191, v53, v59, v191                            // 000000020708: D76D00BF 06FE7735
	v_mul_i32_i24_sdwa v53, sext(v147), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020710: 126AF6F9 08080693
	v_mul_i32_i24_sdwa v59, sext(v147), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020718: 1276F6F9 09090693
	v_add3_u32 v192, v53, v59, v192                            // 000000020720: D76D00C0 07027735
	v_mul_i32_i24_sdwa v53, sext(v147), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020728: 126AF6F9 0A0A0693
	v_mul_i32_i24_sdwa v59, sext(v147), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020730: 1276F6F9 0B0B0693
	v_add3_u32 v192, v53, v59, v192                            // 000000020738: D76D00C0 07027735
	v_mul_i32_i24_sdwa v53, sext(v155), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020740: 126AF6F9 0808069B
	v_mul_i32_i24_sdwa v59, sext(v155), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020748: 1276F6F9 0909069B
	v_add3_u32 v193, v53, v59, v193                            // 000000020750: D76D00C1 07067735
	v_mul_i32_i24_sdwa v53, sext(v155), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020758: 126AF6F9 0A0A069B
	v_mul_i32_i24_sdwa v59, sext(v155), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020760: 1276F6F9 0B0B069B
	v_add3_u32 v193, v53, v59, v193                            // 000000020768: D76D00C1 07067735
	s_waitcnt lgkmcnt(1)                                       // 000000020770: BF8CC17F
	v_mul_i32_i24_sdwa v53, sext(v169), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020774: 126B5EF9 080806A9
	v_mul_i32_i24_sdwa v59, sext(v169), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002077C: 12775EF9 090906A9
	v_add3_u32 v194, v53, v59, v194                            // 000000020784: D76D00C2 070A7735
	v_mul_i32_i24_sdwa v53, sext(v169), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002078C: 126B5EF9 0A0A06A9
	v_mul_i32_i24_sdwa v59, sext(v169), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020794: 12775EF9 0B0B06A9
	v_add3_u32 v194, v53, v59, v194                            // 00000002079C: D76D00C2 070A7735
	v_mul_i32_i24_sdwa v53, sext(v71), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000207A4: 126B5EF9 08080647
	v_mul_i32_i24_sdwa v59, sext(v71), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000207AC: 12775EF9 09090647
	v_add3_u32 v195, v53, v59, v195                            // 0000000207B4: D76D00C3 070E7735
	v_mul_i32_i24_sdwa v53, sext(v71), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000207BC: 126B5EF9 0A0A0647
	v_mul_i32_i24_sdwa v59, sext(v71), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000207C4: 12775EF9 0B0B0647
	v_add3_u32 v195, v53, v59, v195                            // 0000000207CC: D76D00C3 070E7735
	v_mul_i32_i24_sdwa v53, sext(v107), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000207D4: 126B5EF9 0808066B
	v_mul_i32_i24_sdwa v59, sext(v107), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000207DC: 12775EF9 0909066B
	v_add3_u32 v196, v53, v59, v196                            // 0000000207E4: D76D00C4 07127735
	v_mul_i32_i24_sdwa v53, sext(v107), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000207EC: 126B5EF9 0A0A066B
	v_mul_i32_i24_sdwa v59, sext(v107), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000207F4: 12775EF9 0B0B066B
	v_add3_u32 v196, v53, v59, v196                            // 0000000207FC: D76D00C4 07127735
	v_mul_i32_i24_sdwa v53, sext(v143), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020804: 126B5EF9 0808068F
	v_mul_i32_i24_sdwa v59, sext(v143), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002080C: 12775EF9 0909068F
	v_add3_u32 v197, v53, v59, v197                            // 000000020814: D76D00C5 07167735
	v_mul_i32_i24_sdwa v53, sext(v143), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002081C: 126B5EF9 0A0A068F
	v_mul_i32_i24_sdwa v59, sext(v143), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020824: 12775EF9 0B0B068F
	v_add3_u32 v197, v53, v59, v197                            // 00000002082C: D76D00C5 07167735
	v_mul_i32_i24_sdwa v53, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020834: 126A6CF9 0808063C
	v_mul_i32_i24_sdwa v59, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002083C: 12766CF9 0909063C
	v_add3_u32 v182, v53, v59, v182                            // 000000020844: D76D00B6 06DA7735
	v_mul_i32_i24_sdwa v53, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002084C: 126A6CF9 0A0A063C
	v_mul_i32_i24_sdwa v59, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020854: 12766CF9 0B0B063C
	v_add3_u32 v182, v53, v59, v182                            // 00000002085C: D76D00B6 06DA7735
	v_mul_i32_i24_sdwa v53, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020864: 126A6CF9 08080644
	v_mul_i32_i24_sdwa v59, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002086C: 12766CF9 09090644
	v_add3_u32 v183, v53, v59, v183                            // 000000020874: D76D00B7 06DE7735
	v_mul_i32_i24_sdwa v53, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002087C: 126A6CF9 0A0A0644
	v_mul_i32_i24_sdwa v59, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020884: 12766CF9 0B0B0644
	v_add3_u32 v183, v53, v59, v183                            // 00000002088C: D76D00B7 06DE7735
	v_mul_i32_i24_sdwa v53, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020894: 126A6CF9 0808064C
	v_mul_i32_i24_sdwa v59, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002089C: 12766CF9 0909064C
	v_add3_u32 v184, v53, v59, v184                            // 0000000208A4: D76D00B8 06E27735
	v_mul_i32_i24_sdwa v53, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000208AC: 126A6CF9 0A0A064C
	v_mul_i32_i24_sdwa v59, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000208B4: 12766CF9 0B0B064C
	v_add3_u32 v184, v53, v59, v184                            // 0000000208BC: D76D00B8 06E27735
	v_mul_i32_i24_sdwa v53, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000208C4: 126A6CF9 08080654
	v_mul_i32_i24_sdwa v59, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000208CC: 12766CF9 09090654
	v_add3_u32 v185, v53, v59, v185                            // 0000000208D4: D76D00B9 06E67735
	v_mul_i32_i24_sdwa v53, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000208DC: 126A6CF9 0A0A0654
	v_mul_i32_i24_sdwa v59, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000208E4: 12766CF9 0B0B0654
	v_add3_u32 v185, v53, v59, v185                            // 0000000208EC: D76D00B9 06E67735
	v_mul_i32_i24_sdwa v53, sext(v96), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000208F4: 126AB0F9 08080660
	v_mul_i32_i24_sdwa v54, sext(v96), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000208FC: 126CB0F9 09090660
	v_add3_u32 v186, v53, v54, v186                            // 000000020904: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v96), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002090C: 126AB0F9 0A0A0660
	v_mul_i32_i24_sdwa v54, sext(v96), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020914: 126CB0F9 0B0B0660
	v_add3_u32 v186, v53, v54, v186                            // 00000002091C: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v104), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020924: 126AB0F9 08080668
	v_mul_i32_i24_sdwa v54, sext(v104), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002092C: 126CB0F9 09090668
	v_add3_u32 v187, v53, v54, v187                            // 000000020934: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v104), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002093C: 126AB0F9 0A0A0668
	v_mul_i32_i24_sdwa v54, sext(v104), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020944: 126CB0F9 0B0B0668
	v_add3_u32 v187, v53, v54, v187                            // 00000002094C: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v112), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020954: 126AB0F9 08080670
	v_mul_i32_i24_sdwa v54, sext(v112), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002095C: 126CB0F9 09090670
	v_add3_u32 v188, v53, v54, v188                            // 000000020964: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v112), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002096C: 126AB0F9 0A0A0670
	v_mul_i32_i24_sdwa v54, sext(v112), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020974: 126CB0F9 0B0B0670
	v_add3_u32 v188, v53, v54, v188                            // 00000002097C: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v120), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020984: 126AB0F9 08080678
	v_mul_i32_i24_sdwa v54, sext(v120), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002098C: 126CB0F9 09090678
	v_add3_u32 v189, v53, v54, v189                            // 000000020994: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v120), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002099C: 126AB0F9 0A0A0678
	v_mul_i32_i24_sdwa v54, sext(v120), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000209A4: 126CB0F9 0B0B0678
	v_add3_u32 v189, v53, v54, v189                            // 0000000209AC: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v132), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000209B4: 126AF8F9 08080684
	v_mul_i32_i24_sdwa v54, sext(v132), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000209BC: 126CF8F9 09090684
	v_add3_u32 v190, v53, v54, v190                            // 0000000209C4: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v132), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000209CC: 126AF8F9 0A0A0684
	v_mul_i32_i24_sdwa v54, sext(v132), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000209D4: 126CF8F9 0B0B0684
	v_add3_u32 v190, v53, v54, v190                            // 0000000209DC: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v140), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000209E4: 126AF8F9 0808068C
	v_mul_i32_i24_sdwa v54, sext(v140), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000209EC: 126CF8F9 0909068C
	v_add3_u32 v191, v53, v54, v191                            // 0000000209F4: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v140), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000209FC: 126AF8F9 0A0A068C
	v_mul_i32_i24_sdwa v54, sext(v140), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020A04: 126CF8F9 0B0B068C
	v_add3_u32 v191, v53, v54, v191                            // 000000020A0C: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v148), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020A14: 126AF8F9 08080694
	v_mul_i32_i24_sdwa v54, sext(v148), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020A1C: 126CF8F9 09090694
	v_add3_u32 v192, v53, v54, v192                            // 000000020A24: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v148), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020A2C: 126AF8F9 0A0A0694
	v_mul_i32_i24_sdwa v54, sext(v148), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020A34: 126CF8F9 0B0B0694
	v_add3_u32 v192, v53, v54, v192                            // 000000020A3C: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v156), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020A44: 126AF8F9 0808069C
	v_mul_i32_i24_sdwa v54, sext(v156), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020A4C: 126CF8F9 0909069C
	v_add3_u32 v193, v53, v54, v193                            // 000000020A54: D76D00C1 07066D35
	v_mul_i32_i24_sdwa v53, sext(v156), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020A5C: 126AF8F9 0A0A069C
	v_mul_i32_i24_sdwa v54, sext(v156), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020A64: 126CF8F9 0B0B069C
	v_add3_u32 v193, v53, v54, v193                            // 000000020A6C: D76D00C1 07066D35
	v_mul_i32_i24_sdwa v53, sext(v170), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020A74: 126B60F9 080806AA
	v_mul_i32_i24_sdwa v54, sext(v170), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020A7C: 126D60F9 090906AA
	v_add3_u32 v194, v53, v54, v194                            // 000000020A84: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v170), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020A8C: 126B60F9 0A0A06AA
	v_mul_i32_i24_sdwa v54, sext(v170), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020A94: 126D60F9 0B0B06AA
	v_add3_u32 v194, v53, v54, v194                            // 000000020A9C: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v72), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020AA4: 126B60F9 08080648
	v_mul_i32_i24_sdwa v54, sext(v72), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020AAC: 126D60F9 09090648
	v_add3_u32 v195, v53, v54, v195                            // 000000020AB4: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v72), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020ABC: 126B60F9 0A0A0648
	v_mul_i32_i24_sdwa v54, sext(v72), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020AC4: 126D60F9 0B0B0648
	v_add3_u32 v195, v53, v54, v195                            // 000000020ACC: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v108), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020AD4: 126B60F9 0808066C
	v_mul_i32_i24_sdwa v54, sext(v108), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020ADC: 126D60F9 0909066C
	v_add3_u32 v196, v53, v54, v196                            // 000000020AE4: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v108), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020AEC: 126B60F9 0A0A066C
	v_mul_i32_i24_sdwa v54, sext(v108), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020AF4: 126D60F9 0B0B066C
	v_add3_u32 v196, v53, v54, v196                            // 000000020AFC: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v144), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020B04: 126B60F9 08080690
	v_mul_i32_i24_sdwa v54, sext(v144), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020B0C: 126D60F9 09090690
	v_add3_u32 v197, v53, v54, v197                            // 000000020B14: D76D00C5 07166D35
	v_mul_i32_i24_sdwa v53, sext(v144), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020B1C: 126B60F9 0A0A0690
	v_mul_i32_i24_sdwa v54, sext(v144), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020B24: 126D60F9 0B0B0690
	v_add3_u32 v197, v53, v54, v197                            // 000000020B2C: D76D00C5 07166D35
	v_mul_i32_i24_sdwa v53, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020B34: 126A9EF9 0808063D
	v_mul_i32_i24_sdwa v54, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020B3C: 126C9EF9 0909063D
	v_add3_u32 v182, v53, v54, v182                            // 000000020B44: D76D00B6 06DA6D35
	v_mul_i32_i24_sdwa v53, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020B4C: 126A9EF9 0A0A063D
	v_mul_i32_i24_sdwa v54, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020B54: 126C9EF9 0B0B063D
	v_add3_u32 v182, v53, v54, v182                            // 000000020B5C: D76D00B6 06DA6D35
	v_mul_i32_i24_sdwa v53, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020B64: 126A9EF9 08080645
	v_mul_i32_i24_sdwa v54, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020B6C: 126C9EF9 09090645
	v_add3_u32 v183, v53, v54, v183                            // 000000020B74: D76D00B7 06DE6D35
	v_mul_i32_i24_sdwa v53, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020B7C: 126A9EF9 0A0A0645
	v_mul_i32_i24_sdwa v54, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020B84: 126C9EF9 0B0B0645
	v_add3_u32 v183, v53, v54, v183                            // 000000020B8C: D76D00B7 06DE6D35
	v_mul_i32_i24_sdwa v53, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020B94: 126A9EF9 0808064D
	v_mul_i32_i24_sdwa v54, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020B9C: 126C9EF9 0909064D
	v_add3_u32 v184, v53, v54, v184                            // 000000020BA4: D76D00B8 06E26D35
	v_mul_i32_i24_sdwa v53, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020BAC: 126A9EF9 0A0A064D
	v_mul_i32_i24_sdwa v54, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020BB4: 126C9EF9 0B0B064D
	v_add3_u32 v184, v53, v54, v184                            // 000000020BBC: D76D00B8 06E26D35
	v_mul_i32_i24_sdwa v53, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020BC4: 126A9EF9 08080655
	v_mul_i32_i24_sdwa v54, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020BCC: 126C9EF9 09090655
	v_add3_u32 v185, v53, v54, v185                            // 000000020BD4: D76D00B9 06E66D35
	v_mul_i32_i24_sdwa v53, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020BDC: 126A9EF9 0A0A0655
	v_mul_i32_i24_sdwa v54, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020BE4: 126C9EF9 0B0B0655
	v_add3_u32 v185, v53, v54, v185                            // 000000020BEC: D76D00B9 06E66D35
	v_mul_i32_i24_sdwa v53, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020BF4: 126AE6F9 08080661
	v_mul_i32_i24_sdwa v54, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020BFC: 126CE6F9 09090661
	v_add3_u32 v186, v53, v54, v186                            // 000000020C04: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020C0C: 126AE6F9 0A0A0661
	v_mul_i32_i24_sdwa v54, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020C14: 126CE6F9 0B0B0661
	v_add3_u32 v186, v53, v54, v186                            // 000000020C1C: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020C24: 126AE6F9 08080669
	v_mul_i32_i24_sdwa v54, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020C2C: 126CE6F9 09090669
	v_add3_u32 v187, v53, v54, v187                            // 000000020C34: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020C3C: 126AE6F9 0A0A0669
	v_mul_i32_i24_sdwa v54, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020C44: 126CE6F9 0B0B0669
	v_add3_u32 v187, v53, v54, v187                            // 000000020C4C: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020C54: 126AE6F9 08080671
	v_mul_i32_i24_sdwa v54, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020C5C: 126CE6F9 09090671
	v_add3_u32 v188, v53, v54, v188                            // 000000020C64: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020C6C: 126AE6F9 0A0A0671
	v_mul_i32_i24_sdwa v54, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020C74: 126CE6F9 0B0B0671
	v_add3_u32 v188, v53, v54, v188                            // 000000020C7C: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020C84: 126AE6F9 08080679
	v_mul_i32_i24_sdwa v54, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020C8C: 126CE6F9 09090679
	v_add3_u32 v189, v53, v54, v189                            // 000000020C94: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020C9C: 126AE6F9 0A0A0679
	v_mul_i32_i24_sdwa v54, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020CA4: 126CE6F9 0B0B0679
	v_add3_u32 v189, v53, v54, v189                            // 000000020CAC: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v133), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020CB4: 126B2EF9 08080685
	v_mul_i32_i24_sdwa v54, sext(v133), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020CBC: 126D2EF9 09090685
	v_add3_u32 v190, v53, v54, v190                            // 000000020CC4: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v133), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020CCC: 126B2EF9 0A0A0685
	v_mul_i32_i24_sdwa v54, sext(v133), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020CD4: 126D2EF9 0B0B0685
	v_add3_u32 v190, v53, v54, v190                            // 000000020CDC: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v141), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020CE4: 126B2EF9 0808068D
	v_mul_i32_i24_sdwa v54, sext(v141), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020CEC: 126D2EF9 0909068D
	v_add3_u32 v191, v53, v54, v191                            // 000000020CF4: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v141), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020CFC: 126B2EF9 0A0A068D
	v_mul_i32_i24_sdwa v54, sext(v141), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020D04: 126D2EF9 0B0B068D
	v_add3_u32 v191, v53, v54, v191                            // 000000020D0C: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v149), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020D14: 126B2EF9 08080695
	v_mul_i32_i24_sdwa v54, sext(v149), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020D1C: 126D2EF9 09090695
	v_add3_u32 v192, v53, v54, v192                            // 000000020D24: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v149), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020D2C: 126B2EF9 0A0A0695
	v_mul_i32_i24_sdwa v54, sext(v149), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020D34: 126D2EF9 0B0B0695
	v_add3_u32 v192, v53, v54, v192                            // 000000020D3C: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v157), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020D44: 126B2EF9 0808069D
	v_mul_i32_i24_sdwa v54, sext(v157), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020D4C: 126D2EF9 0909069D
	v_add3_u32 v193, v53, v54, v193                            // 000000020D54: D76D00C1 07066D35
	v_mul_i32_i24_sdwa v53, sext(v157), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020D5C: 126B2EF9 0A0A069D
	v_mul_i32_i24_sdwa v54, sext(v157), sext(v151) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020D64: 126D2EF9 0B0B069D
	v_add3_u32 v193, v53, v54, v193                            // 000000020D6C: D76D00C1 07066D35
	s_waitcnt lgkmcnt(0)                                       // 000000020D74: BF8CC07F
	v_mul_i32_i24_sdwa v53, sext(v171), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020D78: 126ABAF9 080806AB
	v_mul_i32_i24_sdwa v54, sext(v171), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020D80: 126CBAF9 090906AB
	v_add3_u32 v194, v53, v54, v194                            // 000000020D88: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v171), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020D90: 126ABAF9 0A0A06AB
	v_mul_i32_i24_sdwa v54, sext(v171), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020D98: 126CBAF9 0B0B06AB
	v_add3_u32 v194, v53, v54, v194                            // 000000020DA0: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v91), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020DA8: 126ABAF9 0808065B
	v_mul_i32_i24_sdwa v54, sext(v91), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020DB0: 126CBAF9 0909065B
	v_add3_u32 v195, v53, v54, v195                            // 000000020DB8: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v91), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020DC0: 126ABAF9 0A0A065B
	v_mul_i32_i24_sdwa v54, sext(v91), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020DC8: 126CBAF9 0B0B065B
	v_add3_u32 v195, v53, v54, v195                            // 000000020DD0: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v127), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020DD8: 126ABAF9 0808067F
	v_mul_i32_i24_sdwa v54, sext(v127), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020DE0: 126CBAF9 0909067F
	v_add3_u32 v196, v53, v54, v196                            // 000000020DE8: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v127), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020DF0: 126ABAF9 0A0A067F
	v_mul_i32_i24_sdwa v54, sext(v127), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020DF8: 126CBAF9 0B0B067F
	v_add3_u32 v196, v53, v54, v196                            // 000000020E00: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v165), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020E08: 126ABAF9 080806A5
	v_mul_i32_i24_sdwa v54, sext(v165), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020E10: 126CBAF9 090906A5
	v_add3_u32 v197, v53, v54, v197                            // 000000020E18: D76D00C5 07166D35
	v_mul_i32_i24_sdwa v53, sext(v165), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020E20: 126ABAF9 0A0A06A5
	v_mul_i32_i24_sdwa v54, sext(v165), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020E28: 126CBAF9 0B0B06A5
	v_add3_u32 v197, v53, v54, v197                            // 000000020E30: D76D00C5 07166D35
	v_mul_i32_i24_sdwa v53, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020E38: 126AA0F9 0808063E
	v_mul_i32_i24_sdwa v54, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020E40: 126CA0F9 0909063E
	v_add3_u32 v182, v53, v54, v182                            // 000000020E48: D76D00B6 06DA6D35
	v_mul_i32_i24_sdwa v53, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020E50: 126AA0F9 0A0A063E
	v_mul_i32_i24_sdwa v54, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020E58: 126CA0F9 0B0B063E
	v_add3_u32 v182, v53, v54, v182                            // 000000020E60: D76D00B6 06DA6D35
	v_mul_i32_i24_sdwa v53, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020E68: 126AA0F9 08080646
	v_mul_i32_i24_sdwa v54, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020E70: 126CA0F9 09090646
	v_add3_u32 v183, v53, v54, v183                            // 000000020E78: D76D00B7 06DE6D35
	v_mul_i32_i24_sdwa v53, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020E80: 126AA0F9 0A0A0646
	v_mul_i32_i24_sdwa v54, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020E88: 126CA0F9 0B0B0646
	v_add3_u32 v183, v53, v54, v183                            // 000000020E90: D76D00B7 06DE6D35
	v_mul_i32_i24_sdwa v53, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020E98: 126AA0F9 0808064E
	v_mul_i32_i24_sdwa v54, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020EA0: 126CA0F9 0909064E
	v_add3_u32 v184, v53, v54, v184                            // 000000020EA8: D76D00B8 06E26D35
	v_mul_i32_i24_sdwa v53, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020EB0: 126AA0F9 0A0A064E
	v_mul_i32_i24_sdwa v54, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020EB8: 126CA0F9 0B0B064E
	v_add3_u32 v184, v53, v54, v184                            // 000000020EC0: D76D00B8 06E26D35
	v_mul_i32_i24_sdwa v53, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020EC8: 126AA0F9 08080656
	v_mul_i32_i24_sdwa v54, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020ED0: 126CA0F9 09090656
	v_add3_u32 v185, v53, v54, v185                            // 000000020ED8: D76D00B9 06E66D35
	v_mul_i32_i24_sdwa v53, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020EE0: 126AA0F9 0A0A0656
	v_mul_i32_i24_sdwa v54, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020EE8: 126CA0F9 0B0B0656
	v_add3_u32 v185, v53, v54, v185                            // 000000020EF0: D76D00B9 06E66D35
	v_mul_i32_i24_sdwa v53, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020EF8: 126AE8F9 08080662
	v_mul_i32_i24_sdwa v54, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020F00: 126CE8F9 09090662
	v_add3_u32 v186, v53, v54, v186                            // 000000020F08: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020F10: 126AE8F9 0A0A0662
	v_mul_i32_i24_sdwa v54, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020F18: 126CE8F9 0B0B0662
	v_add3_u32 v186, v53, v54, v186                            // 000000020F20: D76D00BA 06EA6D35
	v_mul_i32_i24_sdwa v53, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020F28: 126AE8F9 0808066A
	v_mul_i32_i24_sdwa v54, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020F30: 126CE8F9 0909066A
	v_add3_u32 v187, v53, v54, v187                            // 000000020F38: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020F40: 126AE8F9 0A0A066A
	v_mul_i32_i24_sdwa v54, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020F48: 126CE8F9 0B0B066A
	v_add3_u32 v187, v53, v54, v187                            // 000000020F50: D76D00BB 06EE6D35
	v_mul_i32_i24_sdwa v53, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020F58: 126AE8F9 08080672
	v_mul_i32_i24_sdwa v54, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020F60: 126CE8F9 09090672
	v_add3_u32 v188, v53, v54, v188                            // 000000020F68: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020F70: 126AE8F9 0A0A0672
	v_mul_i32_i24_sdwa v54, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020F78: 126CE8F9 0B0B0672
	v_add3_u32 v188, v53, v54, v188                            // 000000020F80: D76D00BC 06F26D35
	v_mul_i32_i24_sdwa v53, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020F88: 126AE8F9 0808067A
	v_mul_i32_i24_sdwa v54, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020F90: 126CE8F9 0909067A
	v_add3_u32 v189, v53, v54, v189                            // 000000020F98: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020FA0: 126AE8F9 0A0A067A
	v_mul_i32_i24_sdwa v54, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020FA8: 126CE8F9 0B0B067A
	v_add3_u32 v189, v53, v54, v189                            // 000000020FB0: D76D00BD 06F66D35
	v_mul_i32_i24_sdwa v53, sext(v134), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020FB8: 126B30F9 08080686
	v_mul_i32_i24_sdwa v54, sext(v134), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020FC0: 126D30F9 09090686
	v_add3_u32 v190, v53, v54, v190                            // 000000020FC8: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v134), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020FD0: 126B30F9 0A0A0686
	v_mul_i32_i24_sdwa v54, sext(v134), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020FD8: 126D30F9 0B0B0686
	v_add3_u32 v190, v53, v54, v190                            // 000000020FE0: D76D00BE 06FA6D35
	v_mul_i32_i24_sdwa v53, sext(v142), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020FE8: 126B30F9 0808068E
	v_mul_i32_i24_sdwa v54, sext(v142), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000020FF0: 126D30F9 0909068E
	v_add3_u32 v191, v53, v54, v191                            // 000000020FF8: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v142), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021000: 126B30F9 0A0A068E
	v_mul_i32_i24_sdwa v54, sext(v142), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021008: 126D30F9 0B0B068E
	v_add3_u32 v191, v53, v54, v191                            // 000000021010: D76D00BF 06FE6D35
	v_mul_i32_i24_sdwa v53, sext(v150), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021018: 126B30F9 08080696
	v_mul_i32_i24_sdwa v54, sext(v150), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021020: 126D30F9 09090696
	v_add3_u32 v192, v53, v54, v192                            // 000000021028: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v150), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021030: 126B30F9 0A0A0696
	v_mul_i32_i24_sdwa v54, sext(v150), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021038: 126D30F9 0B0B0696
	v_add3_u32 v192, v53, v54, v192                            // 000000021040: D76D00C0 07026D35
	v_mul_i32_i24_sdwa v53, sext(v158), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021048: 126B30F9 0808069E
	v_mul_i32_i24_sdwa v54, sext(v158), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021050: 126D30F9 0909069E
	v_add3_u32 v193, v53, v54, v193                            // 000000021058: D76D00C1 07066D35
	v_mul_i32_i24_sdwa v53, sext(v158), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021060: 126B30F9 0A0A069E
	v_mul_i32_i24_sdwa v54, sext(v158), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021068: 126D30F9 0B0B069E
	v_add3_u32 v193, v53, v54, v193                            // 000000021070: D76D00C1 07066D35
	v_mul_i32_i24_sdwa v53, sext(v172), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021078: 126ABCF9 080806AC
	v_mul_i32_i24_sdwa v54, sext(v172), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021080: 126CBCF9 090906AC
	v_add3_u32 v194, v53, v54, v194                            // 000000021088: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v172), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021090: 126ABCF9 0A0A06AC
	v_mul_i32_i24_sdwa v54, sext(v172), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021098: 126CBCF9 0B0B06AC
	v_add3_u32 v194, v53, v54, v194                            // 0000000210A0: D76D00C2 070A6D35
	v_mul_i32_i24_sdwa v53, sext(v92), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000210A8: 126ABCF9 0808065C
	v_mul_i32_i24_sdwa v54, sext(v92), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000210B0: 126CBCF9 0909065C
	v_add3_u32 v195, v53, v54, v195                            // 0000000210B8: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v92), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000210C0: 126ABCF9 0A0A065C
	v_mul_i32_i24_sdwa v54, sext(v92), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000210C8: 126CBCF9 0B0B065C
	v_add3_u32 v195, v53, v54, v195                            // 0000000210D0: D76D00C3 070E6D35
	v_mul_i32_i24_sdwa v53, sext(v128), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000210D8: 126ABCF9 08080680
	v_mul_i32_i24_sdwa v54, sext(v128), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000210E0: 126CBCF9 09090680
	v_add3_u32 v196, v53, v54, v196                            // 0000000210E8: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v128), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000210F0: 126ABCF9 0A0A0680
	v_mul_i32_i24_sdwa v54, sext(v128), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000210F8: 126CBCF9 0B0B0680
	v_add3_u32 v196, v53, v54, v196                            // 000000021100: D76D00C4 07126D35
	v_mul_i32_i24_sdwa v53, sext(v166), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021108: 126ABCF9 080806A6
	v_mul_i32_i24_sdwa v54, sext(v166), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021110: 126CBCF9 090906A6
	v_add3_u32 v197, v53, v54, v197                            // 000000021118: D76D00C5 07166D35
	v_mul_i32_i24_sdwa v53, sext(v166), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021120: 126ABCF9 0A0A06A6
	v_mul_i32_i24_sdwa v54, sext(v166), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021128: 126CBCF9 0B0B06A6
	v_add3_u32 v197, v53, v54, v197                            // 000000021130: D76D00C5 07166D35
	v_mul_f32_e32 v57, v57, v177                               // 000000021138: 10736339
	v_cvt_f32_i32_e32 v53, v182                                // 00000002113C: 7E6A0BB6
	v_cvt_f32_i32_e32 v54, v183                                // 000000021140: 7E6C0BB7
	v_cvt_f32_i32_e32 v59, v184                                // 000000021144: 7E760BB8
	v_cvt_f32_i32_e32 v60, v185                                // 000000021148: 7E780BB9
	v_mul_f32_e32 v56, v56, v178                               // 00000002114C: 10716538
	v_mul_f32_e32 v110, v174, v178                             // 000000021150: 10DD65AE
	v_mul_f32_e32 v58, v58, v178                               // 000000021154: 1075653A
	v_cvt_f32_i32_e32 v61, v186                                // 000000021158: 7E7A0BBA
	v_cvt_f32_i32_e32 v62, v187                                // 00000002115C: 7E7C0BBB
	v_cvt_f32_i32_e32 v63, v188                                // 000000021160: 7E7E0BBC
	v_cvt_f32_i32_e32 v64, v189                                // 000000021164: 7E800BBD
	v_fmac_f32_e32 v6, v55, v53                                // 000000021168: 560C6B37
	v_fmac_f32_e32 v11, v89, v54                               // 00000002116C: 56166D59
	v_fmac_f32_e32 v18, v109, v59                              // 000000021170: 5624776D
	v_fmac_f32_e32 v15, v57, v60                               // 000000021174: 561E7939
	v_mul_f32_e32 v65, v65, v179                               // 000000021178: 10836741
	v_mul_f32_e32 v73, v73, v179                               // 00000002117C: 10936749
	v_mul_f32_e32 v81, v81, v179                               // 000000021180: 10A36751
	v_cvt_f32_i32_e32 v67, v190                                // 000000021184: 7E860BBE
	v_cvt_f32_i32_e32 v68, v191                                // 000000021188: 7E880BBF
	v_cvt_f32_i32_e32 v69, v192                                // 00000002118C: 7E8A0BC0
	v_cvt_f32_i32_e32 v70, v193                                // 000000021190: 7E8C0BC1
	v_fmac_f32_e32 v6, v56, v61                                // 000000021194: 560C7B38
	v_fmac_f32_e32 v11, v90, v62                               // 000000021198: 56167D5A
	v_fmac_f32_e32 v18, v110, v63                              // 00000002119C: 56247F6E
	v_fmac_f32_e32 v15, v58, v64                               // 0000000211A0: 561E813A
	v_mul_f32_e32 v66, v66, v180                               // 0000000211A4: 10856942
	v_mul_f32_e32 v74, v74, v180                               // 0000000211A8: 1095694A
	v_mul_f32_e32 v82, v82, v180                               // 0000000211AC: 10A56952
	v_cvt_f32_i32_e32 v71, v194                                // 0000000211B0: 7E8E0BC2
	v_cvt_f32_i32_e32 v72, v195                                // 0000000211B4: 7E900BC3
	v_cvt_f32_i32_e32 v75, v196                                // 0000000211B8: 7E960BC4
	v_cvt_f32_i32_e32 v76, v197                                // 0000000211BC: 7E980BC5
	v_fmac_f32_e32 v6, v117, v67                               // 0000000211C0: 560C8775
	v_fmac_f32_e32 v11, v65, v68                               // 0000000211C4: 56168941
	v_fmac_f32_e32 v18, v73, v69                               // 0000000211C8: 56248B49
	v_fmac_f32_e32 v15, v81, v70                               // 0000000211CC: 561E8D51
	v_fmac_f32_e32 v6, v118, v71                               // 0000000211D0: 560C8F76
	v_fmac_f32_e32 v11, v66, v72                               // 0000000211D4: 56169142
	v_fmac_f32_e32 v18, v74, v75                               // 0000000211D8: 5624974A
	v_fmac_f32_e32 v15, v82, v76                               // 0000000211DC: 561E9952
	s_barrier                                                  // 0000000211E0: BF8A0000
	buffer_gl0_inv                                             // 0000000211E4: E1C40000 00000000
	s_clause 0x1                                               // 0000000211EC: BFA10001
	global_load_dword v101, v12, s[8:9]                        // 0000000211F0: DC308000 6508000C
	global_load_dword v102, v12, s[8:9] offset:1024            // 0000000211F8: DC308400 6608000C
	s_waitcnt vmcnt(0)                                         // 000000021200: BF8C3F70
	ds_write2st64_b32 v181, v101, v102 offset1:4               // 000000021204: D83C0400 006665B5
	s_waitcnt lgkmcnt(0)                                       // 00000002120C: BF8CC07F
	s_barrier                                                  // 000000021210: BF8A0000
	buffer_gl0_inv                                             // 000000021214: E1C40000 00000000
	ds_read2_b32 v[53:54], v14 offset0:12 offset1:13           // 00000002121C: D8DC0D0C 3500000E
	ds_read2_b32 v[55:56], v52 offset0:40 offset1:41           // 000000021224: D8DC2928 37000034
	ds_read2_b32 v[57:58], v52 offset0:42 offset1:43           // 00000002122C: D8DC2B2A 39000034
	ds_read2_b32 v[59:60], v52 offset0:44 offset1:45           // 000000021234: D8DC2D2C 3B000034
	ds_read2_b32 v[61:62], v52 offset0:46 offset1:47           // 00000002123C: D8DC2F2E 3D000034
	ds_read2_b32 v[63:64], v51 offset0:72 offset1:73           // 000000021244: D8DC4948 3F000033
	ds_read2_b32 v[65:66], v51 offset0:74 offset1:75           // 00000002124C: D8DC4B4A 41000033
	ds_read2_b32 v[67:68], v51 offset0:76 offset1:77           // 000000021254: D8DC4D4C 43000033
	ds_read2_b32 v[69:70], v51 offset0:78 offset1:79           // 00000002125C: D8DC4F4E 45000033
	ds_read2_b32 v[71:72], v46 offset0:104 offset1:105         // 000000021264: D8DC6968 4700002E
	ds_read2_b32 v[73:74], v46 offset0:106 offset1:107         // 00000002126C: D8DC6B6A 4900002E
	ds_read2_b32 v[75:76], v46 offset0:108 offset1:109         // 000000021274: D8DC6D6C 4B00002E
	ds_read2_b32 v[77:78], v46 offset0:110 offset1:111         // 00000002127C: D8DC6F6E 4D00002E
	ds_read2_b32 v[79:80], v45 offset0:136 offset1:137         // 000000021284: D8DC8988 4F00002D
	ds_read2_b32 v[81:82], v45 offset0:138 offset1:139         // 00000002128C: D8DC8B8A 5100002D
	ds_read2_b32 v[83:84], v45 offset0:140 offset1:141         // 000000021294: D8DC8D8C 5300002D
	ds_read2_b32 v[85:86], v45 offset0:142 offset1:143         // 00000002129C: D8DC8F8E 5500002D
	ds_read2_b32 v[87:88], v14 offset0:20 offset1:21           // 0000000212A4: D8DC1514 5700000E
	ds_read2_b32 v[89:90], v14 offset0:22 offset1:23           // 0000000212AC: D8DC1716 5900000E
	ds_read2_b32 v[91:92], v52 offset0:48 offset1:49           // 0000000212B4: D8DC3130 5B000034
	ds_read2_b32 v[93:94], v52 offset0:50 offset1:51           // 0000000212BC: D8DC3332 5D000034
	ds_read2_b32 v[95:96], v52 offset0:52 offset1:53           // 0000000212C4: D8DC3534 5F000034
	ds_read2_b32 v[97:98], v52 offset0:54 offset1:55           // 0000000212CC: D8DC3736 61000034
	ds_read2_b32 v[99:100], v51 offset0:80 offset1:81          // 0000000212D4: D8DC5150 63000033
	ds_read2_b32 v[101:102], v51 offset0:82 offset1:83         // 0000000212DC: D8DC5352 65000033
	ds_read2_b32 v[103:104], v51 offset0:84 offset1:85         // 0000000212E4: D8DC5554 67000033
	ds_read2_b32 v[105:106], v51 offset0:86 offset1:87         // 0000000212EC: D8DC5756 69000033
	ds_read2_b32 v[107:108], v46 offset0:112 offset1:113       // 0000000212F4: D8DC7170 6B00002E
	ds_read2_b32 v[109:110], v46 offset0:114 offset1:115       // 0000000212FC: D8DC7372 6D00002E
	ds_read2_b32 v[111:112], v46 offset0:116 offset1:117       // 000000021304: D8DC7574 6F00002E
	ds_read2_b32 v[113:114], v46 offset0:118 offset1:119       // 00000002130C: D8DC7776 7100002E
	ds_read2_b32 v[115:116], v45 offset0:144 offset1:145       // 000000021314: D8DC9190 7300002D
	ds_read2_b32 v[117:118], v45 offset0:146 offset1:147       // 00000002131C: D8DC9392 7500002D
	ds_read2_b32 v[119:120], v45 offset0:148 offset1:149       // 000000021324: D8DC9594 7700002D
	ds_read2_b32 v[121:122], v45 offset0:150 offset1:151       // 00000002132C: D8DC9796 7900002D
	ds_read2_b32 v[123:124], v14 offset0:28 offset1:29         // 000000021334: D8DC1D1C 7B00000E
	ds_read2_b32 v[125:126], v52 offset0:56 offset1:57         // 00000002133C: D8DC3938 7D000034
	ds_read2_b32 v[127:128], v52 offset0:58 offset1:59         // 000000021344: D8DC3B3A 7F000034
	ds_read2_b32 v[129:130], v52 offset0:60 offset1:61         // 00000002134C: D8DC3D3C 81000034
	ds_read2_b32 v[131:132], v52 offset0:62 offset1:63         // 000000021354: D8DC3F3E 83000034
	ds_read2_b32 v[133:134], v51 offset0:88 offset1:89         // 00000002135C: D8DC5958 85000033
	ds_read2_b32 v[135:136], v51 offset0:90 offset1:91         // 000000021364: D8DC5B5A 87000033
	ds_read2_b32 v[137:138], v51 offset0:92 offset1:93         // 00000002136C: D8DC5D5C 89000033
	ds_read2_b32 v[139:140], v51 offset0:94 offset1:95         // 000000021374: D8DC5F5E 8B000033
	ds_read2_b32 v[141:142], v46 offset0:120 offset1:121       // 00000002137C: D8DC7978 8D00002E
	ds_read2_b32 v[143:144], v46 offset0:122 offset1:123       // 000000021384: D8DC7B7A 8F00002E
	ds_read2_b32 v[145:146], v46 offset0:124 offset1:125       // 00000002138C: D8DC7D7C 9100002E
	ds_read2_b32 v[147:148], v46 offset0:126 offset1:127       // 000000021394: D8DC7F7E 9300002E
	ds_read2_b32 v[149:150], v45 offset0:152 offset1:153       // 00000002139C: D8DC9998 9500002D
	ds_read2_b32 v[151:152], v45 offset0:154 offset1:155       // 0000000213A4: D8DC9B9A 9700002D
	ds_read2_b32 v[153:154], v45 offset0:156 offset1:157       // 0000000213AC: D8DC9D9C 9900002D
	ds_read2_b32 v[155:156], v45 offset0:158 offset1:159       // 0000000213B4: D8DC9F9E 9B00002D
	ds_read2_b32 v[157:158], v52 offset0:64 offset1:65         // 0000000213BC: D8DC4140 9D000034
	ds_read2_b32 v[159:160], v52 offset0:66 offset1:67         // 0000000213C4: D8DC4342 9F000034
	ds_read2_b32 v[161:162], v52 offset0:68 offset1:69         // 0000000213CC: D8DC4544 A1000034
	ds_read2_b32 v[163:164], v52 offset0:70 offset1:71         // 0000000213D4: D8DC4746 A3000034
	ds_read2_b32 v[165:166], v14 offset0:36 offset1:37         // 0000000213DC: D8DC2524 A500000E
	ds_read2_b32 v[167:168], v51 offset0:96 offset1:97         // 0000000213E4: D8DC6160 A7000033
	ds_read2_b32 v[169:170], v51 offset0:98 offset1:99         // 0000000213EC: D8DC6362 A9000033
	ds_read2_b32 v[171:172], v51 offset0:100 offset1:101       // 0000000213F4: D8DC6564 AB000033
	ds_read2_b32 v[51:52], v51 offset0:102 offset1:103         // 0000000213FC: D8DC6766 33000033
	ds_read2_b32 v[173:174], v46 offset0:128 offset1:129       // 000000021404: D8DC8180 AD00002E
	ds_read2_b32 v[175:176], v14 offset0:14 offset1:15         // 00000002140C: D8DC0F0E AF00000E
	ds_read2_b32 v[177:178], v14 offset0:8 offset1:9           // 000000021414: D8DC0908 B100000E
	ds_read2_b32 v[179:180], v14 offset0:10 offset1:11         // 00000002141C: D8DC0B0A B300000E
	s_waitcnt lgkmcnt(62)                                      // 000000021424: BF8CFE7F
	v_mul_i32_i24_sdwa v181, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021428: 136A6AF9 08080637
	v_mul_i32_i24_sdwa v182, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021430: 136C6AF9 09090637
	v_add3_u32 v198, v181, v182, v198                          // 000000021438: D76D00C6 071B6DB5
	v_mul_i32_i24_sdwa v181, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021440: 136A6AF9 0A0A0637
	v_mul_i32_i24_sdwa v182, sext(v55), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021448: 136C6AF9 0B0B0637
	v_add3_u32 v198, v181, v182, v198                          // 000000021450: D76D00C6 071B6DB5
	s_waitcnt lgkmcnt(59)                                      // 000000021458: BF8CFB7F
	v_mul_i32_i24_sdwa v181, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002145C: 136A6AF9 0808063F
	v_mul_i32_i24_sdwa v182, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021464: 136C6AF9 0909063F
	v_add3_u32 v199, v181, v182, v199                          // 00000002146C: D76D00C7 071F6DB5
	v_mul_i32_i24_sdwa v181, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021474: 136A6AF9 0A0A063F
	v_mul_i32_i24_sdwa v182, sext(v63), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002147C: 136C6AF9 0B0B063F
	v_add3_u32 v199, v181, v182, v199                          // 000000021484: D76D00C7 071F6DB5
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002148C: 126E6CF9 08080638
	v_mul_i32_i24_sdwa v181, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021494: 136A6CF9 09090638
	v_add3_u32 v198, v55, v181, v198                           // 00000002149C: D76D00C6 071B6B37
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000214A4: 126E6CF9 0A0A0638
	v_mul_i32_i24_sdwa v181, sext(v56), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000214AC: 136A6CF9 0B0B0638
	v_add3_u32 v198, v55, v181, v198                           // 0000000214B4: D76D00C6 071B6B37
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000214BC: 127E6CF9 08080640
	v_mul_i32_i24_sdwa v181, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000214C4: 136A6CF9 09090640
	v_add3_u32 v199, v63, v181, v199                           // 0000000214CC: D76D00C7 071F6B3F
	v_mul_i32_i24_sdwa v63, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000214D4: 127E6CF9 0A0A0640
	v_mul_i32_i24_sdwa v181, sext(v64), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000214DC: 136A6CF9 0B0B0640
	v_add3_u32 v199, v63, v181, v199                           // 0000000214E4: D76D00C7 071F6B3F
	ds_read2_b32 v[63:64], v50 offset0:140 offset1:141         // 0000000214EC: D8DC8D8C 3F000032
	ds_read2_b32 v[181:182], v50 offset0:142 offset1:143       // 0000000214F4: D8DC8F8E B5000032
	s_waitcnt lgkmcnt(43)                                      // 0000000214FC: BF8CEB7F
	v_mul_i32_i24_sdwa v50, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021500: 1264AEF9 08080663
	v_mul_i32_i24_sdwa v183, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021508: 136EAEF9 09090663
	v_add3_u32 v203, v50, v183, v203                           // 000000021510: D76D00CB 072F6F32
	v_mul_i32_i24_sdwa v50, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021518: 1264AEF9 0A0A0663
	v_mul_i32_i24_sdwa v183, sext(v99), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021520: 136EAEF9 0B0B0663
	v_add3_u32 v203, v50, v183, v203                           // 000000021528: D76D00CB 072F6F32
	v_mul_i32_i24_sdwa v50, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021530: 1264B0F9 08080664
	v_mul_i32_i24_sdwa v99, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021538: 12C6B0F9 09090664
	v_add3_u32 v203, v50, v99, v203                            // 000000021540: D76D00CB 072EC732
	v_mul_i32_i24_sdwa v50, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021548: 1264B0F9 0A0A0664
	v_mul_i32_i24_sdwa v99, sext(v100), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021550: 12C6B0F9 0B0B0664
	v_add3_u32 v203, v50, v99, v203                            // 000000021558: D76D00CB 072EC732
	s_waitcnt lgkmcnt(39)                                      // 000000021560: BF8CE77F
	v_mul_i32_i24_sdwa v99, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021564: 12C6AEF9 0808066B
	v_mul_i32_i24_sdwa v100, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002156C: 12C8AEF9 0909066B
	v_add3_u32 v204, v99, v100, v204                           // 000000021574: D76D00CC 0732C963
	v_mul_i32_i24_sdwa v99, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002157C: 12C6AEF9 0A0A066B
	v_mul_i32_i24_sdwa v100, sext(v107), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021584: 12C8AEF9 0B0B066B
	v_add3_u32 v204, v99, v100, v204                           // 00000002158C: D76D00CC 0732C963
	v_mul_i32_i24_sdwa v99, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021594: 12C6B0F9 0808066C
	v_mul_i32_i24_sdwa v100, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002159C: 12C8B0F9 0909066C
	v_add3_u32 v204, v99, v100, v204                           // 0000000215A4: D76D00CC 0732C963
	v_mul_i32_i24_sdwa v99, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000215AC: 12C6B0F9 0A0A066C
	v_mul_i32_i24_sdwa v100, sext(v108), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000215B4: 12C8B0F9 0B0B066C
	v_add3_u32 v204, v99, v100, v204                           // 0000000215BC: D76D00CC 0732C963
	v_mul_i32_i24_sdwa v50, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000215C4: 12646AF9 08080647
	v_mul_i32_i24_sdwa v183, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000215CC: 136E6AF9 09090647
	v_add3_u32 v200, v50, v183, v200                           // 0000000215D4: D76D00C8 07236F32
	v_mul_i32_i24_sdwa v50, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000215DC: 12646AF9 0A0A0647
	v_mul_i32_i24_sdwa v183, sext(v71), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000215E4: 136E6AF9 0B0B0647
	v_add3_u32 v200, v50, v183, v200                           // 0000000215EC: D76D00C8 07236F32
	v_mul_i32_i24_sdwa v50, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000215F4: 12646AF9 0808064F
	v_mul_i32_i24_sdwa v183, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000215FC: 136E6AF9 0909064F
	v_add3_u32 v201, v50, v183, v201                           // 000000021604: D76D00C9 07276F32
	v_mul_i32_i24_sdwa v50, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002160C: 12646AF9 0A0A064F
	v_mul_i32_i24_sdwa v183, sext(v79), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021614: 136E6AF9 0B0B064F
	v_add3_u32 v201, v50, v183, v201                           // 00000002161C: D76D00C9 07276F32
	v_mul_i32_i24_sdwa v50, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021624: 1264AEF9 0808065B
	v_mul_i32_i24_sdwa v183, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002162C: 136EAEF9 0909065B
	v_add3_u32 v202, v50, v183, v202                           // 000000021634: D76D00CA 072B6F32
	v_mul_i32_i24_sdwa v50, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002163C: 1264AEF9 0A0A065B
	v_mul_i32_i24_sdwa v183, sext(v91), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021644: 136EAEF9 0B0B065B
	v_add3_u32 v202, v50, v183, v202                           // 00000002164C: D76D00CA 072B6F32
	ds_read2_b32 v[99:100], v48 offset0:140 offset1:141        // 000000021654: D8DC8D8C 63000030
	ds_read2_b32 v[107:108], v48 offset0:142 offset1:143       // 00000002165C: D8DC8F8E 6B000030
	s_waitcnt lgkmcnt(37)                                      // 000000021664: BF8CE57F
	v_mul_i32_i24_sdwa v48, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021668: 1260AEF9 08080673
	v_mul_i32_i24_sdwa v183, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021670: 136EAEF9 09090673
	v_add3_u32 v205, v48, v183, v205                           // 000000021678: D76D00CD 07376F30
	v_mul_i32_i24_sdwa v48, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021680: 1260AEF9 0A0A0673
	v_mul_i32_i24_sdwa v183, sext(v115), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021688: 136EAEF9 0B0B0673
	v_add3_u32 v205, v48, v183, v205                           // 000000021690: D76D00CD 07376F30
	s_waitcnt lgkmcnt(32)                                      // 000000021698: BF8CE07F
	v_mul_i32_i24_sdwa v48, sext(v125), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002169C: 1260F6F9 0808067D
	v_mul_i32_i24_sdwa v183, sext(v125), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000216A4: 136EF6F9 0909067D
	v_add3_u32 v206, v48, v183, v206                           // 0000000216AC: D76D00CE 073B6F30
	v_mul_i32_i24_sdwa v48, sext(v125), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000216B4: 1260F6F9 0A0A067D
	v_mul_i32_i24_sdwa v183, sext(v125), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000216BC: 136EF6F9 0B0B067D
	v_add3_u32 v206, v48, v183, v206                           // 0000000216C4: D76D00CE 073B6F30
	s_waitcnt lgkmcnt(28)                                      // 0000000216CC: BF8CDC7F
	v_mul_i32_i24_sdwa v48, sext(v133), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000216D0: 1260F6F9 08080685
	v_mul_i32_i24_sdwa v183, sext(v133), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000216D8: 136EF6F9 09090685
	v_add3_u32 v207, v48, v183, v207                           // 0000000216E0: D76D00CF 073F6F30
	v_mul_i32_i24_sdwa v48, sext(v133), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000216E8: 1260F6F9 0A0A0685
	v_mul_i32_i24_sdwa v183, sext(v133), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000216F0: 136EF6F9 0B0B0685
	v_add3_u32 v207, v48, v183, v207                           // 0000000216F8: D76D00CF 073F6F30
	v_mul_i32_i24_sdwa v50, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021700: 12646CF9 08080648
	v_mul_i32_i24_sdwa v71, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021708: 128E6CF9 09090648
	v_add3_u32 v200, v50, v71, v200                            // 000000021710: D76D00C8 07228F32
	v_mul_i32_i24_sdwa v50, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021718: 12646CF9 0A0A0648
	v_mul_i32_i24_sdwa v71, sext(v72), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021720: 128E6CF9 0B0B0648
	v_add3_u32 v200, v50, v71, v200                            // 000000021728: D76D00C8 07228F32
	v_mul_i32_i24_sdwa v50, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021730: 12646CF9 08080650
	v_mul_i32_i24_sdwa v53, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021738: 126A6CF9 09090650
	v_add3_u32 v201, v50, v53, v201                            // 000000021740: D76D00C9 07266B32
	v_mul_i32_i24_sdwa v50, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021748: 12646CF9 0A0A0650
	v_mul_i32_i24_sdwa v53, sext(v80), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021750: 126A6CF9 0B0B0650
	v_add3_u32 v201, v50, v53, v201                            // 000000021758: D76D00C9 07266B32
	v_mul_i32_i24_sdwa v50, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021760: 1264B0F9 0808065C
	v_mul_i32_i24_sdwa v91, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021768: 12B6B0F9 0909065C
	v_add3_u32 v202, v50, v91, v202                            // 000000021770: D76D00CA 072AB732
	v_mul_i32_i24_sdwa v50, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021778: 1264B0F9 0A0A065C
	v_mul_i32_i24_sdwa v91, sext(v92), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021780: 12B6B0F9 0B0B065C
	v_add3_u32 v202, v50, v91, v202                            // 000000021788: D76D00CA 072AB732
	v_mul_i32_i24_sdwa v48, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021790: 1260B0F9 08080674
	v_mul_i32_i24_sdwa v87, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021798: 12AEB0F9 09090674
	v_add3_u32 v205, v48, v87, v205                            // 0000000217A0: D76D00CD 0736AF30
	v_mul_i32_i24_sdwa v48, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000217A8: 1260B0F9 0A0A0674
	v_mul_i32_i24_sdwa v87, sext(v116), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000217B0: 12AEB0F9 0B0B0674
	v_add3_u32 v205, v48, v87, v205                            // 0000000217B8: D76D00CD 0736AF30
	v_mul_i32_i24_sdwa v48, sext(v126), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000217C0: 1260F8F9 0808067E
	v_mul_i32_i24_sdwa v125, sext(v126), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000217C8: 12FAF8F9 0909067E
	v_add3_u32 v206, v48, v125, v206                           // 0000000217D0: D76D00CE 073AFB30
	v_mul_i32_i24_sdwa v48, sext(v126), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000217D8: 1260F8F9 0A0A067E
	v_mul_i32_i24_sdwa v125, sext(v126), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000217E0: 12FAF8F9 0B0B067E
	v_add3_u32 v206, v48, v125, v206                           // 0000000217E8: D76D00CE 073AFB30
	v_mul_i32_i24_sdwa v48, sext(v134), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000217F0: 1260F8F9 08080686
	v_mul_i32_i24_sdwa v133, sext(v134), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000217F8: 130AF8F9 09090686
	v_add3_u32 v207, v48, v133, v207                           // 000000021800: D76D00CF 073F0B30
	v_mul_i32_i24_sdwa v48, sext(v134), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021808: 1260F8F9 0A0A0686
	v_mul_i32_i24_sdwa v133, sext(v134), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021810: 130AF8F9 0B0B0686
	v_add3_u32 v207, v48, v133, v207                           // 000000021818: D76D00CF 073F0B30
	ds_read2_b32 v[71:72], v49 offset0:140 offset1:141         // 000000021820: D8DC8D8C 47000031
	ds_read2_b32 v[91:92], v47 offset0:140 offset1:141         // 000000021828: D8DC8D8C 5B00002F
	ds_read2_b32 v[49:50], v49 offset0:142 offset1:143         // 000000021830: D8DC8F8E 31000031
	ds_read2_b32 v[47:48], v47 offset0:142 offset1:143         // 000000021838: D8DC8F8E 2F00002F
	s_waitcnt lgkmcnt(28)                                      // 000000021840: BF8CDC7F
	v_mul_i32_i24_sdwa v133, sext(v141), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021844: 130AF6F9 0808068D
	v_mul_i32_i24_sdwa v134, sext(v141), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002184C: 130CF6F9 0909068D
	v_add3_u32 v208, v133, v134, v208                          // 000000021854: D76D00D0 07430D85
	v_mul_i32_i24_sdwa v133, sext(v141), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002185C: 130AF6F9 0A0A068D
	v_mul_i32_i24_sdwa v134, sext(v141), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021864: 130CF6F9 0B0B068D
	v_add3_u32 v208, v133, v134, v208                          // 00000002186C: D76D00D0 07430D85
	v_mul_i32_i24_sdwa v133, sext(v142), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021874: 130AF8F9 0808068E
	v_mul_i32_i24_sdwa v134, sext(v142), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002187C: 130CF8F9 0909068E
	v_add3_u32 v208, v133, v134, v208                          // 000000021884: D76D00D0 07430D85
	v_mul_i32_i24_sdwa v133, sext(v142), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002188C: 130AF8F9 0A0A068E
	v_mul_i32_i24_sdwa v134, sext(v142), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021894: 130CF8F9 0B0B068E
	v_add3_u32 v208, v133, v134, v208                          // 00000002189C: D76D00D0 07430D85
	ds_read2_b32 v[55:56], v46 offset0:130 offset1:131         // 0000000218A4: D8DC8382 3700002E
	ds_read2_b32 v[133:134], v46 offset0:132 offset1:133       // 0000000218AC: D8DC8584 8500002E
	ds_read2_b32 v[141:142], v46 offset0:134 offset1:135       // 0000000218B4: D8DC8786 8D00002E
	s_waitcnt lgkmcnt(19)                                      // 0000000218BC: BF8CD37F
	v_mul_i32_i24_sdwa v46, sext(v157), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000218C0: 125D4AF9 0808069D
	v_mul_i32_i24_sdwa v183, sext(v157), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000218C8: 136F4AF9 0909069D
	v_add3_u32 v210, v46, v183, v210                           // 0000000218D0: D76D00D2 074B6F2E
	v_mul_i32_i24_sdwa v46, sext(v157), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000218D8: 125D4AF9 0A0A069D
	v_mul_i32_i24_sdwa v183, sext(v157), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000218E0: 136F4AF9 0B0B069D
	v_add3_u32 v210, v46, v183, v210                           // 0000000218E8: D76D00D2 074B6F2E
	s_waitcnt lgkmcnt(18)                                      // 0000000218F0: BF8CD27F
	v_mul_i32_i24_sdwa v46, sext(v167), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000218F4: 125D4AF9 080806A7
	v_mul_i32_i24_sdwa v183, sext(v167), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000218FC: 136F4AF9 090906A7
	v_add3_u32 v211, v46, v183, v211                           // 000000021904: D76D00D3 074F6F2E
	v_mul_i32_i24_sdwa v46, sext(v167), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002190C: 125D4AF9 0A0A06A7
	v_mul_i32_i24_sdwa v183, sext(v167), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021914: 136F4AF9 0B0B06A7
	v_add3_u32 v211, v46, v183, v211                           // 00000002191C: D76D00D3 074F6F2E
	v_mul_i32_i24_sdwa v46, sext(v158), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021924: 125D4CF9 0808069E
	v_mul_i32_i24_sdwa v157, sext(v158), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002192C: 133B4CF9 0909069E
	v_add3_u32 v210, v46, v157, v210                           // 000000021934: D76D00D2 074B3B2E
	v_mul_i32_i24_sdwa v46, sext(v158), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002193C: 125D4CF9 0A0A069E
	v_mul_i32_i24_sdwa v157, sext(v158), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021944: 133B4CF9 0B0B069E
	v_add3_u32 v210, v46, v157, v210                           // 00000002194C: D76D00D2 074B3B2E
	v_mul_i32_i24_sdwa v46, sext(v168), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021954: 125D4CF9 080806A8
	v_mul_i32_i24_sdwa v167, sext(v168), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002195C: 134F4CF9 090906A8
	v_add3_u32 v211, v46, v167, v211                           // 000000021964: D76D00D3 074F4F2E
	v_mul_i32_i24_sdwa v46, sext(v168), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002196C: 125D4CF9 0A0A06A8
	v_mul_i32_i24_sdwa v167, sext(v168), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021974: 134F4CF9 0B0B06A8
	v_add3_u32 v211, v46, v167, v211                           // 00000002197C: D76D00D3 074F4F2E
	s_waitcnt lgkmcnt(14)                                      // 000000021984: BF8CCE7F
	v_mul_i32_i24_sdwa v46, sext(v173), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021988: 125D4AF9 080806AD
	v_mul_i32_i24_sdwa v183, sext(v173), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021990: 136F4AF9 090906AD
	v_add3_u32 v212, v46, v183, v212                           // 000000021998: D76D00D4 07536F2E
	v_mul_i32_i24_sdwa v46, sext(v173), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000219A0: 125D4AF9 0A0A06AD
	v_mul_i32_i24_sdwa v183, sext(v173), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000219A8: 136F4AF9 0B0B06AD
	v_add3_u32 v212, v46, v183, v212                           // 0000000219B0: D76D00D4 07536F2E
	ds_read2_b32 v[125:126], v45 offset0:160 offset1:161       // 0000000219B8: D8DCA1A0 7D00002D
	ds_read2_b32 v[157:158], v45 offset0:162 offset1:163       // 0000000219C0: D8DCA3A2 9D00002D
	ds_read2_b32 v[167:168], v45 offset0:164 offset1:165       // 0000000219C8: D8DCA5A4 A700002D
	v_mul_i32_i24_sdwa v46, sext(v149), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000219D0: 125CF6F9 08080695
	v_mul_i32_i24_sdwa v183, sext(v149), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000219D8: 136EF6F9 09090695
	v_add3_u32 v209, v46, v183, v209                           // 0000000219E0: D76D00D1 07476F2E
	v_mul_i32_i24_sdwa v46, sext(v149), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000219E8: 125CF6F9 0A0A0695
	v_mul_i32_i24_sdwa v183, sext(v149), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000219F0: 136EF6F9 0B0B0695
	v_add3_u32 v209, v46, v183, v209                           // 0000000219F8: D76D00D1 07476F2E
	v_mul_i32_i24_sdwa v46, sext(v174), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021A00: 125D4CF9 080806AE
	v_mul_i32_i24_sdwa v173, sext(v174), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021A08: 135B4CF9 090906AE
	v_add3_u32 v212, v46, v173, v212                           // 000000021A10: D76D00D4 07535B2E
	v_mul_i32_i24_sdwa v46, sext(v174), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021A18: 125D4CF9 0A0A06AE
	v_mul_i32_i24_sdwa v173, sext(v174), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021A20: 135B4CF9 0B0B06AE
	v_add3_u32 v212, v46, v173, v212                           // 000000021A28: D76D00D4 07535B2E
	s_waitcnt lgkmcnt(2)                                       // 000000021A30: BF8CC27F
	v_mul_i32_i24_sdwa v173, sext(v125), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021A34: 135B4AF9 0808067D
	v_mul_i32_i24_sdwa v174, sext(v125), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021A3C: 135D4AF9 0909067D
	v_add3_u32 v213, v173, v174, v213                          // 000000021A44: D76D00D5 07575DAD
	v_mul_i32_i24_sdwa v173, sext(v125), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021A4C: 135B4AF9 0A0A067D
	v_mul_i32_i24_sdwa v174, sext(v125), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021A54: 135D4AF9 0B0B067D
	v_add3_u32 v213, v173, v174, v213                          // 000000021A5C: D76D00D5 07575DAD
	v_mul_i32_i24_sdwa v46, sext(v150), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021A64: 125CF8F9 08080696
	v_mul_i32_i24_sdwa v123, sext(v150), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021A6C: 12F6F8F9 09090696
	v_add3_u32 v209, v46, v123, v209                           // 000000021A74: D76D00D1 0746F72E
	v_mul_i32_i24_sdwa v46, sext(v150), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021A7C: 125CF8F9 0A0A0696
	v_mul_i32_i24_sdwa v123, sext(v150), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021A84: 12F6F8F9 0B0B0696
	v_add3_u32 v209, v46, v123, v209                           // 000000021A8C: D76D00D1 0746F72E
	v_mul_i32_i24_sdwa v125, sext(v126), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021A94: 12FB4CF9 0808067E
	v_mul_i32_i24_sdwa v165, sext(v126), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021A9C: 134B4CF9 0909067E
	v_add3_u32 v213, v125, v165, v213                          // 000000021AA4: D76D00D5 07574B7D
	v_mul_i32_i24_sdwa v125, sext(v126), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021AAC: 12FB4CF9 0A0A067E
	v_mul_i32_i24_sdwa v165, sext(v126), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021AB4: 134B4CF9 0B0B067E
	v_add3_u32 v213, v125, v165, v213                          // 000000021ABC: D76D00D5 07574B7D
	ds_read2_b32 v[53:54], v14 offset0:16 offset1:17           // 000000021AC4: D8DC1110 3500000E
	ds_read2_b32 v[79:80], v14 offset0:18 offset1:19           // 000000021ACC: D8DC1312 4F00000E
	ds_read2_b32 v[87:88], v14 offset0:30 offset1:31           // 000000021AD4: D8DC1F1E 5700000E
	ds_read2_b32 v[115:116], v14 offset0:26 offset1:27         // 000000021ADC: D8DC1B1A 7300000E
	ds_read2_b32 v[123:124], v14 offset0:38 offset1:39         // 000000021AE4: D8DC2726 7B00000E
	ds_read2_b32 v[149:150], v14 offset0:34 offset1:35         // 000000021AEC: D8DC2322 9500000E
	ds_read2_b32 v[45:46], v45 offset0:166 offset1:167         // 000000021AF4: D8DCA7A6 2D00002D
	ds_read2_b32 v[125:126], v14 offset0:24 offset1:25         // 000000021AFC: D8DC1918 7D00000E
	ds_read2_b32 v[165:166], v14 offset0:32 offset1:33         // 000000021B04: D8DC2120 A500000E
	v_mul_f32_e32 v173, v63, v177                              // 000000021B0C: 115B633F
	v_mul_f32_e32 v174, v64, v178                              // 000000021B10: 115D6540
	v_mul_f32_e32 v71, v71, v177                               // 000000021B14: 108F6347
	v_mul_f32_e32 v72, v72, v178                               // 000000021B18: 10916548
	v_mul_f32_e32 v99, v99, v177                               // 000000021B1C: 10C76363
	v_mul_f32_e32 v100, v100, v178                             // 000000021B20: 10C96564
	v_mul_f32_e32 v91, v91, v177                               // 000000021B24: 10B7635B
	v_mul_f32_e32 v92, v92, v178                               // 000000021B28: 10B9655C
	ds_read2_b32 v[63:64], v14 offset0:40 offset1:41           // 000000021B2C: D8DC2928 3F00000E
	v_mul_f32_e32 v177, v181, v179                             // 000000021B34: 116367B5
	v_mul_f32_e32 v178, v182, v180                             // 000000021B38: 116569B6
	v_mul_f32_e32 v49, v49, v179                               // 000000021B3C: 10636731
	v_mul_f32_e32 v50, v50, v180                               // 000000021B40: 10656932
	v_mul_f32_e32 v107, v107, v179                             // 000000021B44: 10D7676B
	v_mul_f32_e32 v108, v108, v180                             // 000000021B48: 10D9696C
	v_mul_f32_e32 v179, v47, v179                              // 000000021B4C: 1167672F
	v_mul_f32_e32 v180, v48, v180                              // 000000021B50: 11696930
	ds_read2_b32 v[47:48], v14 offset0:42 offset1:43           // 000000021B54: D8DC2B2A 2F00000E
	v_mul_i32_i24_sdwa v181, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021B5C: 136B5EF9 08080639
	v_mul_i32_i24_sdwa v182, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021B64: 136D5EF9 09090639
	v_add3_u32 v198, v181, v182, v198                          // 000000021B6C: D76D00C6 071B6DB5
	v_mul_i32_i24_sdwa v181, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021B74: 136B5EF9 0A0A0639
	v_mul_i32_i24_sdwa v182, sext(v57), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021B7C: 136D5EF9 0B0B0639
	v_add3_u32 v198, v181, v182, v198                          // 000000021B84: D76D00C6 071B6DB5
	v_mul_i32_i24_sdwa v57, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021B8C: 12735EF9 08080641
	v_mul_i32_i24_sdwa v181, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021B94: 136B5EF9 09090641
	v_add3_u32 v199, v57, v181, v199                           // 000000021B9C: D76D00C7 071F6B39
	v_mul_i32_i24_sdwa v57, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021BA4: 12735EF9 0A0A0641
	v_mul_i32_i24_sdwa v181, sext(v65), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021BAC: 136B5EF9 0B0B0641
	v_add3_u32 v199, v57, v181, v199                           // 000000021BB4: D76D00C7 071F6B39
	v_mul_i32_i24_sdwa v57, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021BBC: 12735EF9 08080649
	v_mul_i32_i24_sdwa v65, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021BC4: 12835EF9 09090649
	v_add3_u32 v200, v57, v65, v200                            // 000000021BCC: D76D00C8 07228339
	v_mul_i32_i24_sdwa v57, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021BD4: 12735EF9 0A0A0649
	v_mul_i32_i24_sdwa v65, sext(v73), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021BDC: 12835EF9 0B0B0649
	v_add3_u32 v200, v57, v65, v200                            // 000000021BE4: D76D00C8 07228339
	v_mul_i32_i24_sdwa v57, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021BEC: 12735EF9 08080651
	v_mul_i32_i24_sdwa v65, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021BF4: 12835EF9 09090651
	v_add3_u32 v201, v57, v65, v201                            // 000000021BFC: D76D00C9 07268339
	v_mul_i32_i24_sdwa v57, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021C04: 12735EF9 0A0A0651
	v_mul_i32_i24_sdwa v65, sext(v81), sext(v175) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021C0C: 12835EF9 0B0B0651
	v_add3_u32 v201, v57, v65, v201                            // 000000021C14: D76D00C9 07268339
	v_mul_i32_i24_sdwa v57, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021C1C: 1272B2F9 0808065D
	v_mul_i32_i24_sdwa v65, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021C24: 1282B2F9 0909065D
	v_add3_u32 v202, v57, v65, v202                            // 000000021C2C: D76D00CA 072A8339
	v_mul_i32_i24_sdwa v57, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021C34: 1272B2F9 0A0A065D
	v_mul_i32_i24_sdwa v65, sext(v93), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021C3C: 1282B2F9 0B0B065D
	v_add3_u32 v202, v57, v65, v202                            // 000000021C44: D76D00CA 072A8339
	v_mul_i32_i24_sdwa v57, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021C4C: 1272B2F9 08080665
	v_mul_i32_i24_sdwa v65, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021C54: 1282B2F9 09090665
	v_add3_u32 v203, v57, v65, v203                            // 000000021C5C: D76D00CB 072E8339
	v_mul_i32_i24_sdwa v57, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021C64: 1272B2F9 0A0A0665
	v_mul_i32_i24_sdwa v65, sext(v101), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021C6C: 1282B2F9 0B0B0665
	v_add3_u32 v203, v57, v65, v203                            // 000000021C74: D76D00CB 072E8339
	v_mul_i32_i24_sdwa v57, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021C7C: 1272B2F9 0808066D
	v_mul_i32_i24_sdwa v65, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021C84: 1282B2F9 0909066D
	v_add3_u32 v204, v57, v65, v204                            // 000000021C8C: D76D00CC 07328339
	v_mul_i32_i24_sdwa v57, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021C94: 1272B2F9 0A0A066D
	v_mul_i32_i24_sdwa v65, sext(v109), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021C9C: 1282B2F9 0B0B066D
	v_add3_u32 v204, v57, v65, v204                            // 000000021CA4: D76D00CC 07328339
	v_mul_i32_i24_sdwa v57, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021CAC: 1272B2F9 08080675
	v_mul_i32_i24_sdwa v65, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021CB4: 1282B2F9 09090675
	v_add3_u32 v205, v57, v65, v205                            // 000000021CBC: D76D00CD 07368339
	v_mul_i32_i24_sdwa v57, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021CC4: 1272B2F9 0A0A0675
	v_mul_i32_i24_sdwa v65, sext(v117), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021CCC: 1282B2F9 0B0B0675
	v_add3_u32 v205, v57, v65, v205                            // 000000021CD4: D76D00CD 07368339
	s_waitcnt lgkmcnt(8)                                       // 000000021CDC: BF8CC87F
	v_mul_i32_i24_sdwa v57, sext(v127), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021CE0: 1272AEF9 0808067F
	v_mul_i32_i24_sdwa v65, sext(v127), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021CE8: 1282AEF9 0909067F
	v_add3_u32 v206, v57, v65, v206                            // 000000021CF0: D76D00CE 073A8339
	v_mul_i32_i24_sdwa v57, sext(v127), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021CF8: 1272AEF9 0A0A067F
	v_mul_i32_i24_sdwa v65, sext(v127), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021D00: 1282AEF9 0B0B067F
	v_add3_u32 v206, v57, v65, v206                            // 000000021D08: D76D00CE 073A8339
	v_mul_i32_i24_sdwa v57, sext(v135), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021D10: 1272AEF9 08080687
	v_mul_i32_i24_sdwa v65, sext(v135), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021D18: 1282AEF9 09090687
	v_add3_u32 v207, v57, v65, v207                            // 000000021D20: D76D00CF 073E8339
	v_mul_i32_i24_sdwa v57, sext(v135), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021D28: 1272AEF9 0A0A0687
	v_mul_i32_i24_sdwa v65, sext(v135), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021D30: 1282AEF9 0B0B0687
	v_add3_u32 v207, v57, v65, v207                            // 000000021D38: D76D00CF 073E8339
	v_mul_i32_i24_sdwa v57, sext(v143), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021D40: 1272AEF9 0808068F
	v_mul_i32_i24_sdwa v65, sext(v143), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021D48: 1282AEF9 0909068F
	v_add3_u32 v208, v57, v65, v208                            // 000000021D50: D76D00D0 07428339
	v_mul_i32_i24_sdwa v57, sext(v143), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021D58: 1272AEF9 0A0A068F
	v_mul_i32_i24_sdwa v65, sext(v143), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021D60: 1282AEF9 0B0B068F
	v_add3_u32 v208, v57, v65, v208                            // 000000021D68: D76D00D0 07428339
	v_mul_i32_i24_sdwa v57, sext(v151), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021D70: 1272AEF9 08080697
	v_mul_i32_i24_sdwa v65, sext(v151), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021D78: 1282AEF9 09090697
	v_add3_u32 v209, v57, v65, v209                            // 000000021D80: D76D00D1 07468339
	v_mul_i32_i24_sdwa v57, sext(v151), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021D88: 1272AEF9 0A0A0697
	v_mul_i32_i24_sdwa v65, sext(v151), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021D90: 1282AEF9 0B0B0697
	v_add3_u32 v209, v57, v65, v209                            // 000000021D98: D76D00D1 07468339
	s_waitcnt lgkmcnt(6)                                       // 000000021DA0: BF8CC67F
	v_mul_i32_i24_sdwa v57, sext(v159), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021DA4: 1272F6F9 0808069F
	v_mul_i32_i24_sdwa v65, sext(v159), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021DAC: 1282F6F9 0909069F
	v_add3_u32 v210, v57, v65, v210                            // 000000021DB4: D76D00D2 074A8339
	v_mul_i32_i24_sdwa v57, sext(v159), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021DBC: 1272F6F9 0A0A069F
	v_mul_i32_i24_sdwa v65, sext(v159), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021DC4: 1282F6F9 0B0B069F
	v_add3_u32 v210, v57, v65, v210                            // 000000021DCC: D76D00D2 074A8339
	v_mul_i32_i24_sdwa v57, sext(v169), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021DD4: 1272F6F9 080806A9
	v_mul_i32_i24_sdwa v65, sext(v169), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021DDC: 1282F6F9 090906A9
	v_add3_u32 v211, v57, v65, v211                            // 000000021DE4: D76D00D3 074E8339
	v_mul_i32_i24_sdwa v57, sext(v169), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021DEC: 1272F6F9 0A0A06A9
	v_mul_i32_i24_sdwa v65, sext(v169), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021DF4: 1282F6F9 0B0B06A9
	v_add3_u32 v211, v57, v65, v211                            // 000000021DFC: D76D00D3 074E8339
	v_mul_i32_i24_sdwa v57, sext(v55), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021E04: 1272F6F9 08080637
	v_mul_i32_i24_sdwa v65, sext(v55), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021E0C: 1282F6F9 09090637
	v_add3_u32 v212, v57, v65, v212                            // 000000021E14: D76D00D4 07528339
	v_mul_i32_i24_sdwa v57, sext(v55), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021E1C: 1272F6F9 0A0A0637
	v_mul_i32_i24_sdwa v65, sext(v55), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021E24: 1282F6F9 0B0B0637
	v_add3_u32 v212, v57, v65, v212                            // 000000021E2C: D76D00D4 07528339
	v_mul_i32_i24_sdwa v55, sext(v157), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021E34: 126EF6F9 0808069D
	v_mul_i32_i24_sdwa v57, sext(v157), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021E3C: 1272F6F9 0909069D
	v_add3_u32 v213, v55, v57, v213                            // 000000021E44: D76D00D5 07567337
	v_mul_i32_i24_sdwa v55, sext(v157), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021E4C: 126EF6F9 0A0A069D
	v_mul_i32_i24_sdwa v57, sext(v157), sext(v123) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021E54: 1272F6F9 0B0B069D
	v_add3_u32 v213, v55, v57, v213                            // 000000021E5C: D76D00D5 07567337
	v_mul_i32_i24_sdwa v55, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021E64: 126F60F9 0808063A
	v_mul_i32_i24_sdwa v57, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021E6C: 127360F9 0909063A
	v_add3_u32 v198, v55, v57, v198                            // 000000021E74: D76D00C6 071A7337
	v_mul_i32_i24_sdwa v55, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021E7C: 126F60F9 0A0A063A
	v_mul_i32_i24_sdwa v57, sext(v58), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021E84: 127360F9 0B0B063A
	v_add3_u32 v198, v55, v57, v198                            // 000000021E8C: D76D00C6 071A7337
	v_mul_i32_i24_sdwa v55, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021E94: 126F60F9 08080642
	v_mul_i32_i24_sdwa v57, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021E9C: 127360F9 09090642
	v_add3_u32 v199, v55, v57, v199                            // 000000021EA4: D76D00C7 071E7337
	v_mul_i32_i24_sdwa v55, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021EAC: 126F60F9 0A0A0642
	v_mul_i32_i24_sdwa v57, sext(v66), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021EB4: 127360F9 0B0B0642
	v_add3_u32 v199, v55, v57, v199                            // 000000021EBC: D76D00C7 071E7337
	v_mul_i32_i24_sdwa v55, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021EC4: 126F60F9 0808064A
	v_mul_i32_i24_sdwa v57, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021ECC: 127360F9 0909064A
	v_add3_u32 v200, v55, v57, v200                            // 000000021ED4: D76D00C8 07227337
	v_mul_i32_i24_sdwa v55, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021EDC: 126F60F9 0A0A064A
	v_mul_i32_i24_sdwa v57, sext(v74), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021EE4: 127360F9 0B0B064A
	v_add3_u32 v200, v55, v57, v200                            // 000000021EEC: D76D00C8 07227337
	v_mul_i32_i24_sdwa v55, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021EF4: 126F60F9 08080652
	v_mul_i32_i24_sdwa v57, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021EFC: 127360F9 09090652
	v_add3_u32 v201, v55, v57, v201                            // 000000021F04: D76D00C9 07267337
	v_mul_i32_i24_sdwa v55, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021F0C: 126F60F9 0A0A0652
	v_mul_i32_i24_sdwa v57, sext(v82), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021F14: 127360F9 0B0B0652
	v_add3_u32 v201, v55, v57, v201                            // 000000021F1C: D76D00C9 07267337
	v_mul_i32_i24_sdwa v55, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021F24: 126EB4F9 0808065E
	v_mul_i32_i24_sdwa v57, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021F2C: 1272B4F9 0909065E
	v_add3_u32 v202, v55, v57, v202                            // 000000021F34: D76D00CA 072A7337
	v_mul_i32_i24_sdwa v55, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021F3C: 126EB4F9 0A0A065E
	v_mul_i32_i24_sdwa v57, sext(v94), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021F44: 1272B4F9 0B0B065E
	v_add3_u32 v202, v55, v57, v202                            // 000000021F4C: D76D00CA 072A7337
	v_mul_i32_i24_sdwa v55, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021F54: 126EB4F9 08080666
	v_mul_i32_i24_sdwa v57, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021F5C: 1272B4F9 09090666
	v_add3_u32 v203, v55, v57, v203                            // 000000021F64: D76D00CB 072E7337
	v_mul_i32_i24_sdwa v55, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021F6C: 126EB4F9 0A0A0666
	v_mul_i32_i24_sdwa v57, sext(v102), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021F74: 1272B4F9 0B0B0666
	v_add3_u32 v203, v55, v57, v203                            // 000000021F7C: D76D00CB 072E7337
	v_mul_i32_i24_sdwa v55, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021F84: 126EB4F9 0808066E
	v_mul_i32_i24_sdwa v57, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021F8C: 1272B4F9 0909066E
	v_add3_u32 v204, v55, v57, v204                            // 000000021F94: D76D00CC 07327337
	v_mul_i32_i24_sdwa v55, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021F9C: 126EB4F9 0A0A066E
	v_mul_i32_i24_sdwa v57, sext(v110), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021FA4: 1272B4F9 0B0B066E
	v_add3_u32 v204, v55, v57, v204                            // 000000021FAC: D76D00CC 07327337
	v_mul_i32_i24_sdwa v55, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021FB4: 126EB4F9 08080676
	v_mul_i32_i24_sdwa v57, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021FBC: 1272B4F9 09090676
	v_add3_u32 v205, v55, v57, v205                            // 000000021FC4: D76D00CD 07367337
	v_mul_i32_i24_sdwa v55, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021FCC: 126EB4F9 0A0A0676
	v_mul_i32_i24_sdwa v57, sext(v118), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000021FD4: 1272B4F9 0B0B0676
	v_add3_u32 v205, v55, v57, v205                            // 000000021FDC: D76D00CD 07367337
	v_mul_i32_i24_sdwa v55, sext(v128), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000021FE4: 126EB0F9 08080680
	v_mul_i32_i24_sdwa v57, sext(v128), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000021FEC: 1272B0F9 09090680
	v_add3_u32 v206, v55, v57, v206                            // 000000021FF4: D76D00CE 073A7337
	v_mul_i32_i24_sdwa v55, sext(v128), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000021FFC: 126EB0F9 0A0A0680
	v_mul_i32_i24_sdwa v57, sext(v128), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022004: 1272B0F9 0B0B0680
	v_add3_u32 v206, v55, v57, v206                            // 00000002200C: D76D00CE 073A7337
	v_mul_i32_i24_sdwa v55, sext(v136), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022014: 126EB0F9 08080688
	v_mul_i32_i24_sdwa v57, sext(v136), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002201C: 1272B0F9 09090688
	v_add3_u32 v207, v55, v57, v207                            // 000000022024: D76D00CF 073E7337
	v_mul_i32_i24_sdwa v55, sext(v136), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002202C: 126EB0F9 0A0A0688
	v_mul_i32_i24_sdwa v57, sext(v136), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022034: 1272B0F9 0B0B0688
	v_add3_u32 v207, v55, v57, v207                            // 00000002203C: D76D00CF 073E7337
	v_mul_i32_i24_sdwa v55, sext(v144), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022044: 126EB0F9 08080690
	v_mul_i32_i24_sdwa v57, sext(v144), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002204C: 1272B0F9 09090690
	v_add3_u32 v208, v55, v57, v208                            // 000000022054: D76D00D0 07427337
	v_mul_i32_i24_sdwa v55, sext(v144), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002205C: 126EB0F9 0A0A0690
	v_mul_i32_i24_sdwa v57, sext(v144), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022064: 1272B0F9 0B0B0690
	v_add3_u32 v208, v55, v57, v208                            // 00000002206C: D76D00D0 07427337
	v_mul_i32_i24_sdwa v55, sext(v152), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022074: 126EB0F9 08080698
	v_mul_i32_i24_sdwa v57, sext(v152), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002207C: 1272B0F9 09090698
	v_add3_u32 v209, v55, v57, v209                            // 000000022084: D76D00D1 07467337
	v_mul_i32_i24_sdwa v55, sext(v152), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002208C: 126EB0F9 0A0A0698
	v_mul_i32_i24_sdwa v57, sext(v152), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022094: 1272B0F9 0B0B0698
	v_add3_u32 v209, v55, v57, v209                            // 00000002209C: D76D00D1 07467337
	v_mul_i32_i24_sdwa v55, sext(v160), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000220A4: 126EF8F9 080806A0
	v_mul_i32_i24_sdwa v57, sext(v160), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000220AC: 1272F8F9 090906A0
	v_add3_u32 v210, v55, v57, v210                            // 0000000220B4: D76D00D2 074A7337
	v_mul_i32_i24_sdwa v55, sext(v160), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000220BC: 126EF8F9 0A0A06A0
	v_mul_i32_i24_sdwa v57, sext(v160), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000220C4: 1272F8F9 0B0B06A0
	v_add3_u32 v210, v55, v57, v210                            // 0000000220CC: D76D00D2 074A7337
	v_mul_i32_i24_sdwa v55, sext(v170), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000220D4: 126EF8F9 080806AA
	v_mul_i32_i24_sdwa v57, sext(v170), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000220DC: 1272F8F9 090906AA
	v_add3_u32 v211, v55, v57, v211                            // 0000000220E4: D76D00D3 074E7337
	v_mul_i32_i24_sdwa v55, sext(v170), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000220EC: 126EF8F9 0A0A06AA
	v_mul_i32_i24_sdwa v57, sext(v170), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000220F4: 1272F8F9 0B0B06AA
	v_add3_u32 v211, v55, v57, v211                            // 0000000220FC: D76D00D3 074E7337
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022104: 126EF8F9 08080638
	v_mul_i32_i24_sdwa v57, sext(v56), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002210C: 1272F8F9 09090638
	v_add3_u32 v212, v55, v57, v212                            // 000000022114: D76D00D4 07527337
	v_mul_i32_i24_sdwa v55, sext(v56), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002211C: 126EF8F9 0A0A0638
	v_mul_i32_i24_sdwa v57, sext(v56), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022124: 1272F8F9 0B0B0638
	v_add3_u32 v212, v55, v57, v212                            // 00000002212C: D76D00D4 07527337
	v_mul_i32_i24_sdwa v55, sext(v158), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022134: 126EF8F9 0808069E
	v_mul_i32_i24_sdwa v56, sext(v158), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002213C: 1270F8F9 0909069E
	v_add3_u32 v213, v55, v56, v213                            // 000000022144: D76D00D5 07567137
	v_mul_i32_i24_sdwa v55, sext(v158), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002214C: 126EF8F9 0A0A069E
	v_mul_i32_i24_sdwa v56, sext(v158), sext(v124) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022154: 1270F8F9 0B0B069E
	v_add3_u32 v213, v55, v56, v213                            // 00000002215C: D76D00D5 07567137
	v_mul_i32_i24_sdwa v55, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022164: 126E6AF9 0808063B
	v_mul_i32_i24_sdwa v56, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002216C: 12706AF9 0909063B
	v_add3_u32 v198, v55, v56, v198                            // 000000022174: D76D00C6 071A7137
	v_mul_i32_i24_sdwa v55, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002217C: 126E6AF9 0A0A063B
	v_mul_i32_i24_sdwa v56, sext(v59), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022184: 12706AF9 0B0B063B
	v_add3_u32 v198, v55, v56, v198                            // 00000002218C: D76D00C6 071A7137
	v_mul_i32_i24_sdwa v55, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022194: 126E6AF9 08080643
	v_mul_i32_i24_sdwa v56, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 00000002219C: 12706AF9 09090643
	v_add3_u32 v199, v55, v56, v199                            // 0000000221A4: D76D00C7 071E7137
	v_mul_i32_i24_sdwa v55, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000221AC: 126E6AF9 0A0A0643
	v_mul_i32_i24_sdwa v56, sext(v67), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000221B4: 12706AF9 0B0B0643
	v_add3_u32 v199, v55, v56, v199                            // 0000000221BC: D76D00C7 071E7137
	v_mul_i32_i24_sdwa v55, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000221C4: 126E6AF9 0808064B
	v_mul_i32_i24_sdwa v56, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000221CC: 12706AF9 0909064B
	v_add3_u32 v200, v55, v56, v200                            // 0000000221D4: D76D00C8 07227137
	v_mul_i32_i24_sdwa v55, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000221DC: 126E6AF9 0A0A064B
	v_mul_i32_i24_sdwa v56, sext(v75), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000221E4: 12706AF9 0B0B064B
	v_add3_u32 v200, v55, v56, v200                            // 0000000221EC: D76D00C8 07227137
	v_mul_i32_i24_sdwa v55, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000221F4: 126E6AF9 08080653
	v_mul_i32_i24_sdwa v56, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000221FC: 12706AF9 09090653
	v_add3_u32 v201, v55, v56, v201                            // 000000022204: D76D00C9 07267137
	v_mul_i32_i24_sdwa v55, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002220C: 126E6AF9 0A0A0653
	v_mul_i32_i24_sdwa v56, sext(v83), sext(v53) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022214: 12706AF9 0B0B0653
	v_add3_u32 v201, v55, v56, v201                            // 00000002221C: D76D00C9 07267137
	s_waitcnt lgkmcnt(3)                                       // 000000022224: BF8CC37F
	v_mul_i32_i24_sdwa v53, sext(v95), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022228: 126AFAF9 0808065F
	v_mul_i32_i24_sdwa v55, sext(v95), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022230: 126EFAF9 0909065F
	v_add3_u32 v202, v53, v55, v202                            // 000000022238: D76D00CA 072A6F35
	v_mul_i32_i24_sdwa v53, sext(v95), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022240: 126AFAF9 0A0A065F
	v_mul_i32_i24_sdwa v55, sext(v95), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022248: 126EFAF9 0B0B065F
	v_add3_u32 v202, v53, v55, v202                            // 000000022250: D76D00CA 072A6F35
	v_mul_i32_i24_sdwa v53, sext(v103), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022258: 126AFAF9 08080667
	v_mul_i32_i24_sdwa v55, sext(v103), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022260: 126EFAF9 09090667
	v_add3_u32 v203, v53, v55, v203                            // 000000022268: D76D00CB 072E6F35
	v_mul_i32_i24_sdwa v53, sext(v103), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022270: 126AFAF9 0A0A0667
	v_mul_i32_i24_sdwa v55, sext(v103), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022278: 126EFAF9 0B0B0667
	v_add3_u32 v203, v53, v55, v203                            // 000000022280: D76D00CB 072E6F35
	v_mul_i32_i24_sdwa v53, sext(v111), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022288: 126AFAF9 0808066F
	v_mul_i32_i24_sdwa v55, sext(v111), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022290: 126EFAF9 0909066F
	v_add3_u32 v204, v53, v55, v204                            // 000000022298: D76D00CC 07326F35
	v_mul_i32_i24_sdwa v53, sext(v111), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000222A0: 126AFAF9 0A0A066F
	v_mul_i32_i24_sdwa v55, sext(v111), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000222A8: 126EFAF9 0B0B066F
	v_add3_u32 v204, v53, v55, v204                            // 0000000222B0: D76D00CC 07326F35
	v_mul_i32_i24_sdwa v53, sext(v119), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000222B8: 126AFAF9 08080677
	v_mul_i32_i24_sdwa v55, sext(v119), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000222C0: 126EFAF9 09090677
	v_add3_u32 v205, v53, v55, v205                            // 0000000222C8: D76D00CD 07366F35
	v_mul_i32_i24_sdwa v53, sext(v119), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000222D0: 126AFAF9 0A0A0677
	v_mul_i32_i24_sdwa v55, sext(v119), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000222D8: 126EFAF9 0B0B0677
	v_add3_u32 v205, v53, v55, v205                            // 0000000222E0: D76D00CD 07366F35
	s_waitcnt lgkmcnt(2)                                       // 0000000222E8: BF8CC27F
	v_mul_i32_i24_sdwa v53, sext(v129), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000222EC: 126B4AF9 08080681
	v_mul_i32_i24_sdwa v55, sext(v129), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000222F4: 126F4AF9 09090681
	v_add3_u32 v206, v53, v55, v206                            // 0000000222FC: D76D00CE 073A6F35
	v_mul_i32_i24_sdwa v53, sext(v129), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022304: 126B4AF9 0A0A0681
	v_mul_i32_i24_sdwa v55, sext(v129), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002230C: 126F4AF9 0B0B0681
	v_add3_u32 v206, v53, v55, v206                            // 000000022314: D76D00CE 073A6F35
	v_mul_i32_i24_sdwa v53, sext(v137), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002231C: 126B4AF9 08080689
	v_mul_i32_i24_sdwa v55, sext(v137), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022324: 126F4AF9 09090689
	v_add3_u32 v207, v53, v55, v207                            // 00000002232C: D76D00CF 073E6F35
	v_mul_i32_i24_sdwa v53, sext(v137), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022334: 126B4AF9 0A0A0689
	v_mul_i32_i24_sdwa v55, sext(v137), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002233C: 126F4AF9 0B0B0689
	v_add3_u32 v207, v53, v55, v207                            // 000000022344: D76D00CF 073E6F35
	v_mul_i32_i24_sdwa v53, sext(v145), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002234C: 126B4AF9 08080691
	v_mul_i32_i24_sdwa v55, sext(v145), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022354: 126F4AF9 09090691
	v_add3_u32 v208, v53, v55, v208                            // 00000002235C: D76D00D0 07426F35
	v_mul_i32_i24_sdwa v53, sext(v145), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022364: 126B4AF9 0A0A0691
	v_mul_i32_i24_sdwa v55, sext(v145), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002236C: 126F4AF9 0B0B0691
	v_add3_u32 v208, v53, v55, v208                            // 000000022374: D76D00D0 07426F35
	v_mul_i32_i24_sdwa v53, sext(v153), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002237C: 126B4AF9 08080699
	v_mul_i32_i24_sdwa v55, sext(v153), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022384: 126F4AF9 09090699
	v_add3_u32 v209, v53, v55, v209                            // 00000002238C: D76D00D1 07466F35
	v_mul_i32_i24_sdwa v53, sext(v153), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022394: 126B4AF9 0A0A0699
	v_mul_i32_i24_sdwa v55, sext(v153), sext(v165) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002239C: 126F4AF9 0B0B0699
	v_add3_u32 v209, v53, v55, v209                            // 0000000223A4: D76D00D1 07466F35
	s_waitcnt lgkmcnt(1)                                       // 0000000223AC: BF8CC17F
	v_mul_i32_i24_sdwa v53, sext(v161), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000223B0: 126A7EF9 080806A1
	v_mul_i32_i24_sdwa v55, sext(v161), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000223B8: 126E7EF9 090906A1
	v_add3_u32 v210, v53, v55, v210                            // 0000000223C0: D76D00D2 074A6F35
	v_mul_i32_i24_sdwa v53, sext(v161), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000223C8: 126A7EF9 0A0A06A1
	v_mul_i32_i24_sdwa v55, sext(v161), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000223D0: 126E7EF9 0B0B06A1
	v_add3_u32 v210, v53, v55, v210                            // 0000000223D8: D76D00D2 074A6F35
	v_mul_i32_i24_sdwa v53, sext(v171), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000223E0: 126A7EF9 080806AB
	v_mul_i32_i24_sdwa v55, sext(v171), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000223E8: 126E7EF9 090906AB
	v_add3_u32 v211, v53, v55, v211                            // 0000000223F0: D76D00D3 074E6F35
	v_mul_i32_i24_sdwa v53, sext(v171), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000223F8: 126A7EF9 0A0A06AB
	v_mul_i32_i24_sdwa v55, sext(v171), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022400: 126E7EF9 0B0B06AB
	v_add3_u32 v211, v53, v55, v211                            // 000000022408: D76D00D3 074E6F35
	v_mul_i32_i24_sdwa v53, sext(v133), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022410: 126A7EF9 08080685
	v_mul_i32_i24_sdwa v55, sext(v133), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022418: 126E7EF9 09090685
	v_add3_u32 v212, v53, v55, v212                            // 000000022420: D76D00D4 07526F35
	v_mul_i32_i24_sdwa v53, sext(v133), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022428: 126A7EF9 0A0A0685
	v_mul_i32_i24_sdwa v55, sext(v133), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022430: 126E7EF9 0B0B0685
	v_add3_u32 v212, v53, v55, v212                            // 000000022438: D76D00D4 07526F35
	v_mul_i32_i24_sdwa v53, sext(v167), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022440: 126A7EF9 080806A7
	v_mul_i32_i24_sdwa v55, sext(v167), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022448: 126E7EF9 090906A7
	v_add3_u32 v213, v53, v55, v213                            // 000000022450: D76D00D5 07566F35
	v_mul_i32_i24_sdwa v53, sext(v167), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022458: 126A7EF9 0A0A06A7
	v_mul_i32_i24_sdwa v55, sext(v167), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022460: 126E7EF9 0B0B06A7
	v_add3_u32 v213, v53, v55, v213                            // 000000022468: D76D00D5 07566F35
	v_mul_i32_i24_sdwa v53, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022470: 126A6CF9 0808063C
	v_mul_i32_i24_sdwa v55, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022478: 126E6CF9 0909063C
	v_add3_u32 v198, v53, v55, v198                            // 000000022480: D76D00C6 071A6F35
	v_mul_i32_i24_sdwa v53, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022488: 126A6CF9 0A0A063C
	v_mul_i32_i24_sdwa v55, sext(v60), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022490: 126E6CF9 0B0B063C
	v_add3_u32 v198, v53, v55, v198                            // 000000022498: D76D00C6 071A6F35
	v_mul_i32_i24_sdwa v53, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000224A0: 126A6CF9 08080644
	v_mul_i32_i24_sdwa v55, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000224A8: 126E6CF9 09090644
	v_add3_u32 v199, v53, v55, v199                            // 0000000224B0: D76D00C7 071E6F35
	v_mul_i32_i24_sdwa v53, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000224B8: 126A6CF9 0A0A0644
	v_mul_i32_i24_sdwa v55, sext(v68), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000224C0: 126E6CF9 0B0B0644
	v_add3_u32 v199, v53, v55, v199                            // 0000000224C8: D76D00C7 071E6F35
	v_mul_i32_i24_sdwa v53, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000224D0: 126A6CF9 0808064C
	v_mul_i32_i24_sdwa v55, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000224D8: 126E6CF9 0909064C
	v_add3_u32 v200, v53, v55, v200                            // 0000000224E0: D76D00C8 07226F35
	v_mul_i32_i24_sdwa v53, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000224E8: 126A6CF9 0A0A064C
	v_mul_i32_i24_sdwa v55, sext(v76), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000224F0: 126E6CF9 0B0B064C
	v_add3_u32 v200, v53, v55, v200                            // 0000000224F8: D76D00C8 07226F35
	v_mul_i32_i24_sdwa v53, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022500: 126A6CF9 08080654
	v_mul_i32_i24_sdwa v55, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022508: 126E6CF9 09090654
	v_add3_u32 v201, v53, v55, v201                            // 000000022510: D76D00C9 07266F35
	v_mul_i32_i24_sdwa v53, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022518: 126A6CF9 0A0A0654
	v_mul_i32_i24_sdwa v55, sext(v84), sext(v54) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022520: 126E6CF9 0B0B0654
	v_add3_u32 v201, v53, v55, v201                            // 000000022528: D76D00C9 07266F35
	v_mul_i32_i24_sdwa v53, sext(v96), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022530: 126AFCF9 08080660
	v_mul_i32_i24_sdwa v54, sext(v96), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022538: 126CFCF9 09090660
	v_add3_u32 v202, v53, v54, v202                            // 000000022540: D76D00CA 072A6D35
	v_mul_i32_i24_sdwa v53, sext(v96), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022548: 126AFCF9 0A0A0660
	v_mul_i32_i24_sdwa v54, sext(v96), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022550: 126CFCF9 0B0B0660
	v_add3_u32 v202, v53, v54, v202                            // 000000022558: D76D00CA 072A6D35
	v_mul_i32_i24_sdwa v53, sext(v104), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022560: 126AFCF9 08080668
	v_mul_i32_i24_sdwa v54, sext(v104), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022568: 126CFCF9 09090668
	v_add3_u32 v203, v53, v54, v203                            // 000000022570: D76D00CB 072E6D35
	v_mul_i32_i24_sdwa v53, sext(v104), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022578: 126AFCF9 0A0A0668
	v_mul_i32_i24_sdwa v54, sext(v104), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022580: 126CFCF9 0B0B0668
	v_add3_u32 v203, v53, v54, v203                            // 000000022588: D76D00CB 072E6D35
	v_mul_i32_i24_sdwa v53, sext(v112), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022590: 126AFCF9 08080670
	v_mul_i32_i24_sdwa v54, sext(v112), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022598: 126CFCF9 09090670
	v_add3_u32 v204, v53, v54, v204                            // 0000000225A0: D76D00CC 07326D35
	v_mul_i32_i24_sdwa v53, sext(v112), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000225A8: 126AFCF9 0A0A0670
	v_mul_i32_i24_sdwa v54, sext(v112), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000225B0: 126CFCF9 0B0B0670
	v_add3_u32 v204, v53, v54, v204                            // 0000000225B8: D76D00CC 07326D35
	v_mul_i32_i24_sdwa v53, sext(v120), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000225C0: 126AFCF9 08080678
	v_mul_i32_i24_sdwa v54, sext(v120), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000225C8: 126CFCF9 09090678
	v_add3_u32 v205, v53, v54, v205                            // 0000000225D0: D76D00CD 07366D35
	v_mul_i32_i24_sdwa v53, sext(v120), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000225D8: 126AFCF9 0A0A0678
	v_mul_i32_i24_sdwa v54, sext(v120), sext(v126) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000225E0: 126CFCF9 0B0B0678
	v_add3_u32 v205, v53, v54, v205                            // 0000000225E8: D76D00CD 07366D35
	v_mul_i32_i24_sdwa v53, sext(v130), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000225F0: 126B4CF9 08080682
	v_mul_i32_i24_sdwa v54, sext(v130), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000225F8: 126D4CF9 09090682
	v_add3_u32 v206, v53, v54, v206                            // 000000022600: D76D00CE 073A6D35
	v_mul_i32_i24_sdwa v53, sext(v130), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022608: 126B4CF9 0A0A0682
	v_mul_i32_i24_sdwa v54, sext(v130), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022610: 126D4CF9 0B0B0682
	v_add3_u32 v206, v53, v54, v206                            // 000000022618: D76D00CE 073A6D35
	v_mul_i32_i24_sdwa v53, sext(v138), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022620: 126B4CF9 0808068A
	v_mul_i32_i24_sdwa v54, sext(v138), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022628: 126D4CF9 0909068A
	v_add3_u32 v207, v53, v54, v207                            // 000000022630: D76D00CF 073E6D35
	v_mul_i32_i24_sdwa v53, sext(v138), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022638: 126B4CF9 0A0A068A
	v_mul_i32_i24_sdwa v54, sext(v138), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022640: 126D4CF9 0B0B068A
	v_add3_u32 v207, v53, v54, v207                            // 000000022648: D76D00CF 073E6D35
	v_mul_i32_i24_sdwa v53, sext(v146), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022650: 126B4CF9 08080692
	v_mul_i32_i24_sdwa v54, sext(v146), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022658: 126D4CF9 09090692
	v_add3_u32 v208, v53, v54, v208                            // 000000022660: D76D00D0 07426D35
	v_mul_i32_i24_sdwa v53, sext(v146), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022668: 126B4CF9 0A0A0692
	v_mul_i32_i24_sdwa v54, sext(v146), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022670: 126D4CF9 0B0B0692
	v_add3_u32 v208, v53, v54, v208                            // 000000022678: D76D00D0 07426D35
	v_mul_i32_i24_sdwa v53, sext(v154), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022680: 126B4CF9 0808069A
	v_mul_i32_i24_sdwa v54, sext(v154), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022688: 126D4CF9 0909069A
	v_add3_u32 v209, v53, v54, v209                            // 000000022690: D76D00D1 07466D35
	v_mul_i32_i24_sdwa v53, sext(v154), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022698: 126B4CF9 0A0A069A
	v_mul_i32_i24_sdwa v54, sext(v154), sext(v166) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000226A0: 126D4CF9 0B0B069A
	v_add3_u32 v209, v53, v54, v209                            // 0000000226A8: D76D00D1 07466D35
	v_mul_i32_i24_sdwa v53, sext(v162), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000226B0: 126A80F9 080806A2
	v_mul_i32_i24_sdwa v54, sext(v162), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000226B8: 126C80F9 090906A2
	v_add3_u32 v210, v53, v54, v210                            // 0000000226C0: D76D00D2 074A6D35
	v_mul_i32_i24_sdwa v53, sext(v162), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000226C8: 126A80F9 0A0A06A2
	v_mul_i32_i24_sdwa v54, sext(v162), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000226D0: 126C80F9 0B0B06A2
	v_add3_u32 v210, v53, v54, v210                            // 0000000226D8: D76D00D2 074A6D35
	v_mul_i32_i24_sdwa v53, sext(v172), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000226E0: 126A80F9 080806AC
	v_mul_i32_i24_sdwa v54, sext(v172), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000226E8: 126C80F9 090906AC
	v_add3_u32 v211, v53, v54, v211                            // 0000000226F0: D76D00D3 074E6D35
	v_mul_i32_i24_sdwa v53, sext(v172), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000226F8: 126A80F9 0A0A06AC
	v_mul_i32_i24_sdwa v54, sext(v172), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022700: 126C80F9 0B0B06AC
	v_add3_u32 v211, v53, v54, v211                            // 000000022708: D76D00D3 074E6D35
	v_mul_i32_i24_sdwa v53, sext(v134), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022710: 126A80F9 08080686
	v_mul_i32_i24_sdwa v54, sext(v134), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022718: 126C80F9 09090686
	v_add3_u32 v212, v53, v54, v212                            // 000000022720: D76D00D4 07526D35
	v_mul_i32_i24_sdwa v53, sext(v134), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022728: 126A80F9 0A0A0686
	v_mul_i32_i24_sdwa v54, sext(v134), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022730: 126C80F9 0B0B0686
	v_add3_u32 v212, v53, v54, v212                            // 000000022738: D76D00D4 07526D35
	v_mul_i32_i24_sdwa v53, sext(v168), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022740: 126A80F9 080806A8
	v_mul_i32_i24_sdwa v54, sext(v168), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022748: 126C80F9 090906A8
	v_add3_u32 v213, v53, v54, v213                            // 000000022750: D76D00D5 07566D35
	v_mul_i32_i24_sdwa v53, sext(v168), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022758: 126A80F9 0A0A06A8
	v_mul_i32_i24_sdwa v54, sext(v168), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022760: 126C80F9 0B0B06A8
	v_add3_u32 v213, v53, v54, v213                            // 000000022768: D76D00D5 07566D35
	v_mul_i32_i24_sdwa v53, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022770: 126A9EF9 0808063D
	v_mul_i32_i24_sdwa v54, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022778: 126C9EF9 0909063D
	v_add3_u32 v198, v53, v54, v198                            // 000000022780: D76D00C6 071A6D35
	v_mul_i32_i24_sdwa v53, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022788: 126A9EF9 0A0A063D
	v_mul_i32_i24_sdwa v54, sext(v61), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022790: 126C9EF9 0B0B063D
	v_add3_u32 v198, v53, v54, v198                            // 000000022798: D76D00C6 071A6D35
	v_mul_i32_i24_sdwa v53, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000227A0: 126A9EF9 08080645
	v_mul_i32_i24_sdwa v54, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000227A8: 126C9EF9 09090645
	v_add3_u32 v199, v53, v54, v199                            // 0000000227B0: D76D00C7 071E6D35
	v_mul_i32_i24_sdwa v53, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000227B8: 126A9EF9 0A0A0645
	v_mul_i32_i24_sdwa v54, sext(v69), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000227C0: 126C9EF9 0B0B0645
	v_add3_u32 v199, v53, v54, v199                            // 0000000227C8: D76D00C7 071E6D35
	v_mul_i32_i24_sdwa v53, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000227D0: 126A9EF9 0808064D
	v_mul_i32_i24_sdwa v54, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000227D8: 126C9EF9 0909064D
	v_add3_u32 v200, v53, v54, v200                            // 0000000227E0: D76D00C8 07226D35
	v_mul_i32_i24_sdwa v53, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000227E8: 126A9EF9 0A0A064D
	v_mul_i32_i24_sdwa v54, sext(v77), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000227F0: 126C9EF9 0B0B064D
	v_add3_u32 v200, v53, v54, v200                            // 0000000227F8: D76D00C8 07226D35
	v_mul_i32_i24_sdwa v53, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022800: 126A9EF9 08080655
	v_mul_i32_i24_sdwa v54, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022808: 126C9EF9 09090655
	v_add3_u32 v201, v53, v54, v201                            // 000000022810: D76D00C9 07266D35
	v_mul_i32_i24_sdwa v53, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022818: 126A9EF9 0A0A0655
	v_mul_i32_i24_sdwa v54, sext(v85), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022820: 126C9EF9 0B0B0655
	v_add3_u32 v201, v53, v54, v201                            // 000000022828: D76D00C9 07266D35
	v_mul_i32_i24_sdwa v53, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022830: 126AE6F9 08080661
	v_mul_i32_i24_sdwa v54, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022838: 126CE6F9 09090661
	v_add3_u32 v202, v53, v54, v202                            // 000000022840: D76D00CA 072A6D35
	v_mul_i32_i24_sdwa v53, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022848: 126AE6F9 0A0A0661
	v_mul_i32_i24_sdwa v54, sext(v97), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022850: 126CE6F9 0B0B0661
	v_add3_u32 v202, v53, v54, v202                            // 000000022858: D76D00CA 072A6D35
	v_mul_i32_i24_sdwa v53, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022860: 126AE6F9 08080669
	v_mul_i32_i24_sdwa v54, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022868: 126CE6F9 09090669
	v_add3_u32 v203, v53, v54, v203                            // 000000022870: D76D00CB 072E6D35
	v_mul_i32_i24_sdwa v53, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022878: 126AE6F9 0A0A0669
	v_mul_i32_i24_sdwa v54, sext(v105), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022880: 126CE6F9 0B0B0669
	v_add3_u32 v203, v53, v54, v203                            // 000000022888: D76D00CB 072E6D35
	v_mul_i32_i24_sdwa v53, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022890: 126AE6F9 08080671
	v_mul_i32_i24_sdwa v54, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022898: 126CE6F9 09090671
	v_add3_u32 v204, v53, v54, v204                            // 0000000228A0: D76D00CC 07326D35
	v_mul_i32_i24_sdwa v53, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000228A8: 126AE6F9 0A0A0671
	v_mul_i32_i24_sdwa v54, sext(v113), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000228B0: 126CE6F9 0B0B0671
	v_add3_u32 v204, v53, v54, v204                            // 0000000228B8: D76D00CC 07326D35
	v_mul_i32_i24_sdwa v53, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000228C0: 126AE6F9 08080679
	v_mul_i32_i24_sdwa v54, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000228C8: 126CE6F9 09090679
	v_add3_u32 v205, v53, v54, v205                            // 0000000228D0: D76D00CD 07366D35
	v_mul_i32_i24_sdwa v53, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000228D8: 126AE6F9 0A0A0679
	v_mul_i32_i24_sdwa v54, sext(v121), sext(v115) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000228E0: 126CE6F9 0B0B0679
	v_add3_u32 v205, v53, v54, v205                            // 0000000228E8: D76D00CD 07366D35
	v_mul_i32_i24_sdwa v53, sext(v131), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000228F0: 126B2AF9 08080683
	v_mul_i32_i24_sdwa v54, sext(v131), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000228F8: 126D2AF9 09090683
	v_add3_u32 v206, v53, v54, v206                            // 000000022900: D76D00CE 073A6D35
	v_mul_i32_i24_sdwa v53, sext(v131), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022908: 126B2AF9 0A0A0683
	v_mul_i32_i24_sdwa v54, sext(v131), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022910: 126D2AF9 0B0B0683
	v_add3_u32 v206, v53, v54, v206                            // 000000022918: D76D00CE 073A6D35
	v_mul_i32_i24_sdwa v53, sext(v139), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022920: 126B2AF9 0808068B
	v_mul_i32_i24_sdwa v54, sext(v139), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022928: 126D2AF9 0909068B
	v_add3_u32 v207, v53, v54, v207                            // 000000022930: D76D00CF 073E6D35
	v_mul_i32_i24_sdwa v53, sext(v139), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022938: 126B2AF9 0A0A068B
	v_mul_i32_i24_sdwa v54, sext(v139), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022940: 126D2AF9 0B0B068B
	v_add3_u32 v207, v53, v54, v207                            // 000000022948: D76D00CF 073E6D35
	v_mul_i32_i24_sdwa v53, sext(v147), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022950: 126B2AF9 08080693
	v_mul_i32_i24_sdwa v54, sext(v147), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022958: 126D2AF9 09090693
	v_add3_u32 v208, v53, v54, v208                            // 000000022960: D76D00D0 07426D35
	v_mul_i32_i24_sdwa v53, sext(v147), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022968: 126B2AF9 0A0A0693
	v_mul_i32_i24_sdwa v54, sext(v147), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022970: 126D2AF9 0B0B0693
	v_add3_u32 v208, v53, v54, v208                            // 000000022978: D76D00D0 07426D35
	v_mul_i32_i24_sdwa v53, sext(v155), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022980: 126B2AF9 0808069B
	v_mul_i32_i24_sdwa v54, sext(v155), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022988: 126D2AF9 0909069B
	v_add3_u32 v209, v53, v54, v209                            // 000000022990: D76D00D1 07466D35
	v_mul_i32_i24_sdwa v53, sext(v155), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022998: 126B2AF9 0A0A069B
	v_mul_i32_i24_sdwa v54, sext(v155), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000229A0: 126D2AF9 0B0B069B
	v_add3_u32 v209, v53, v54, v209                            // 0000000229A8: D76D00D1 07466D35
	s_waitcnt lgkmcnt(0)                                       // 0000000229B0: BF8CC07F
	v_mul_i32_i24_sdwa v53, sext(v163), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000229B4: 126A5EF9 080806A3
	v_mul_i32_i24_sdwa v54, sext(v163), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000229BC: 126C5EF9 090906A3
	v_add3_u32 v210, v53, v54, v210                            // 0000000229C4: D76D00D2 074A6D35
	v_mul_i32_i24_sdwa v53, sext(v163), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000229CC: 126A5EF9 0A0A06A3
	v_mul_i32_i24_sdwa v54, sext(v163), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000229D4: 126C5EF9 0B0B06A3
	v_add3_u32 v210, v53, v54, v210                            // 0000000229DC: D76D00D2 074A6D35
	v_mul_i32_i24_sdwa v53, sext(v51), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000229E4: 126A5EF9 08080633
	v_mul_i32_i24_sdwa v54, sext(v51), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 0000000229EC: 126C5EF9 09090633
	v_add3_u32 v211, v53, v54, v211                            // 0000000229F4: D76D00D3 074E6D35
	v_mul_i32_i24_sdwa v53, sext(v51), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000229FC: 126A5EF9 0A0A0633
	v_mul_i32_i24_sdwa v54, sext(v51), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022A04: 126C5EF9 0B0B0633
	v_add3_u32 v211, v53, v54, v211                            // 000000022A0C: D76D00D3 074E6D35
	v_mul_i32_i24_sdwa v51, sext(v141), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022A14: 12665EF9 0808068D
	v_mul_i32_i24_sdwa v53, sext(v141), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022A1C: 126A5EF9 0909068D
	v_add3_u32 v212, v51, v53, v212                            // 000000022A24: D76D00D4 07526B33
	v_mul_i32_i24_sdwa v51, sext(v141), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022A2C: 12665EF9 0A0A068D
	v_mul_i32_i24_sdwa v53, sext(v141), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022A34: 126A5EF9 0B0B068D
	v_add3_u32 v212, v51, v53, v212                            // 000000022A3C: D76D00D4 07526B33
	v_mul_i32_i24_sdwa v51, sext(v45), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022A44: 12665EF9 0808062D
	v_mul_i32_i24_sdwa v53, sext(v45), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022A4C: 126A5EF9 0909062D
	v_add3_u32 v213, v51, v53, v213                            // 000000022A54: D76D00D5 07566B33
	v_mul_i32_i24_sdwa v51, sext(v45), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022A5C: 12665EF9 0A0A062D
	v_mul_i32_i24_sdwa v53, sext(v45), sext(v47) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022A64: 126A5EF9 0B0B062D
	v_add3_u32 v213, v51, v53, v213                            // 000000022A6C: D76D00D5 07566B33
	v_mul_i32_i24_sdwa v45, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022A74: 125AA0F9 0808063E
	v_mul_i32_i24_sdwa v47, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022A7C: 125EA0F9 0909063E
	v_add3_u32 v198, v45, v47, v198                            // 000000022A84: D76D00C6 071A5F2D
	v_mul_i32_i24_sdwa v45, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022A8C: 125AA0F9 0A0A063E
	v_mul_i32_i24_sdwa v47, sext(v62), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022A94: 125EA0F9 0B0B063E
	v_add3_u32 v198, v45, v47, v198                            // 000000022A9C: D76D00C6 071A5F2D
	v_mul_i32_i24_sdwa v45, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022AA4: 125AA0F9 08080646
	v_mul_i32_i24_sdwa v47, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022AAC: 125EA0F9 09090646
	v_add3_u32 v199, v45, v47, v199                            // 000000022AB4: D76D00C7 071E5F2D
	v_mul_i32_i24_sdwa v45, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022ABC: 125AA0F9 0A0A0646
	v_mul_i32_i24_sdwa v47, sext(v70), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022AC4: 125EA0F9 0B0B0646
	v_add3_u32 v199, v45, v47, v199                            // 000000022ACC: D76D00C7 071E5F2D
	v_mul_i32_i24_sdwa v45, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022AD4: 125AA0F9 0808064E
	v_mul_i32_i24_sdwa v47, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022ADC: 125EA0F9 0909064E
	v_add3_u32 v200, v45, v47, v200                            // 000000022AE4: D76D00C8 07225F2D
	v_mul_i32_i24_sdwa v45, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022AEC: 125AA0F9 0A0A064E
	v_mul_i32_i24_sdwa v47, sext(v78), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022AF4: 125EA0F9 0B0B064E
	v_add3_u32 v200, v45, v47, v200                            // 000000022AFC: D76D00C8 07225F2D
	v_mul_i32_i24_sdwa v45, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022B04: 125AA0F9 08080656
	v_mul_i32_i24_sdwa v47, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022B0C: 125EA0F9 09090656
	v_add3_u32 v201, v45, v47, v201                            // 000000022B14: D76D00C9 07265F2D
	v_mul_i32_i24_sdwa v45, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022B1C: 125AA0F9 0A0A0656
	v_mul_i32_i24_sdwa v47, sext(v86), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022B24: 125EA0F9 0B0B0656
	v_add3_u32 v201, v45, v47, v201                            // 000000022B2C: D76D00C9 07265F2D
	v_mul_i32_i24_sdwa v45, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022B34: 125AE8F9 08080662
	v_mul_i32_i24_sdwa v47, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022B3C: 125EE8F9 09090662
	v_add3_u32 v202, v45, v47, v202                            // 000000022B44: D76D00CA 072A5F2D
	v_mul_i32_i24_sdwa v45, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022B4C: 125AE8F9 0A0A0662
	v_mul_i32_i24_sdwa v47, sext(v98), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022B54: 125EE8F9 0B0B0662
	v_add3_u32 v202, v45, v47, v202                            // 000000022B5C: D76D00CA 072A5F2D
	v_mul_i32_i24_sdwa v45, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022B64: 125AE8F9 0808066A
	v_mul_i32_i24_sdwa v47, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022B6C: 125EE8F9 0909066A
	v_add3_u32 v203, v45, v47, v203                            // 000000022B74: D76D00CB 072E5F2D
	v_mul_i32_i24_sdwa v45, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022B7C: 125AE8F9 0A0A066A
	v_mul_i32_i24_sdwa v47, sext(v106), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022B84: 125EE8F9 0B0B066A
	v_add3_u32 v203, v45, v47, v203                            // 000000022B8C: D76D00CB 072E5F2D
	v_mul_i32_i24_sdwa v45, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022B94: 125AE8F9 08080672
	v_mul_i32_i24_sdwa v47, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022B9C: 125EE8F9 09090672
	v_add3_u32 v204, v45, v47, v204                            // 000000022BA4: D76D00CC 07325F2D
	v_mul_i32_i24_sdwa v45, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022BAC: 125AE8F9 0A0A0672
	v_mul_i32_i24_sdwa v47, sext(v114), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022BB4: 125EE8F9 0B0B0672
	v_add3_u32 v204, v45, v47, v204                            // 000000022BBC: D76D00CC 07325F2D
	v_mul_i32_i24_sdwa v45, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022BC4: 125AE8F9 0808067A
	v_mul_i32_i24_sdwa v47, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022BCC: 125EE8F9 0909067A
	v_add3_u32 v205, v45, v47, v205                            // 000000022BD4: D76D00CD 07365F2D
	v_mul_i32_i24_sdwa v45, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022BDC: 125AE8F9 0A0A067A
	v_mul_i32_i24_sdwa v47, sext(v122), sext(v116) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022BE4: 125EE8F9 0B0B067A
	v_add3_u32 v205, v45, v47, v205                            // 000000022BEC: D76D00CD 07365F2D
	v_mul_i32_i24_sdwa v45, sext(v132), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022BF4: 125B2CF9 08080684
	v_mul_i32_i24_sdwa v47, sext(v132), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022BFC: 125F2CF9 09090684
	v_add3_u32 v206, v45, v47, v206                            // 000000022C04: D76D00CE 073A5F2D
	v_mul_i32_i24_sdwa v45, sext(v132), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022C0C: 125B2CF9 0A0A0684
	v_mul_i32_i24_sdwa v47, sext(v132), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022C14: 125F2CF9 0B0B0684
	v_add3_u32 v206, v45, v47, v206                            // 000000022C1C: D76D00CE 073A5F2D
	v_mul_i32_i24_sdwa v45, sext(v140), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022C24: 125B2CF9 0808068C
	v_mul_i32_i24_sdwa v47, sext(v140), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022C2C: 125F2CF9 0909068C
	v_add3_u32 v207, v45, v47, v207                            // 000000022C34: D76D00CF 073E5F2D
	v_mul_i32_i24_sdwa v45, sext(v140), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022C3C: 125B2CF9 0A0A068C
	v_mul_i32_i24_sdwa v47, sext(v140), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022C44: 125F2CF9 0B0B068C
	v_add3_u32 v207, v45, v47, v207                            // 000000022C4C: D76D00CF 073E5F2D
	v_mul_i32_i24_sdwa v45, sext(v148), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022C54: 125B2CF9 08080694
	v_mul_i32_i24_sdwa v47, sext(v148), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022C5C: 125F2CF9 09090694
	v_add3_u32 v208, v45, v47, v208                            // 000000022C64: D76D00D0 07425F2D
	v_mul_i32_i24_sdwa v45, sext(v148), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022C6C: 125B2CF9 0A0A0694
	v_mul_i32_i24_sdwa v47, sext(v148), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022C74: 125F2CF9 0B0B0694
	v_add3_u32 v208, v45, v47, v208                            // 000000022C7C: D76D00D0 07425F2D
	v_mul_i32_i24_sdwa v45, sext(v156), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022C84: 125B2CF9 0808069C
	v_mul_i32_i24_sdwa v47, sext(v156), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022C8C: 125F2CF9 0909069C
	v_add3_u32 v209, v45, v47, v209                            // 000000022C94: D76D00D1 07465F2D
	v_mul_i32_i24_sdwa v45, sext(v156), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022C9C: 125B2CF9 0A0A069C
	v_mul_i32_i24_sdwa v47, sext(v156), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022CA4: 125F2CF9 0B0B069C
	v_add3_u32 v209, v45, v47, v209                            // 000000022CAC: D76D00D1 07465F2D
	v_mul_i32_i24_sdwa v45, sext(v164), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022CB4: 125A60F9 080806A4
	v_mul_i32_i24_sdwa v47, sext(v164), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022CBC: 125E60F9 090906A4
	v_add3_u32 v210, v45, v47, v210                            // 000000022CC4: D76D00D2 074A5F2D
	v_mul_i32_i24_sdwa v45, sext(v164), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022CCC: 125A60F9 0A0A06A4
	v_mul_i32_i24_sdwa v47, sext(v164), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022CD4: 125E60F9 0B0B06A4
	v_add3_u32 v210, v45, v47, v210                            // 000000022CDC: D76D00D2 074A5F2D
	v_mul_i32_i24_sdwa v45, sext(v52), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022CE4: 125A60F9 08080634
	v_mul_i32_i24_sdwa v47, sext(v52), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022CEC: 125E60F9 09090634
	v_add3_u32 v211, v45, v47, v211                            // 000000022CF4: D76D00D3 074E5F2D
	v_mul_i32_i24_sdwa v45, sext(v52), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022CFC: 125A60F9 0A0A0634
	v_mul_i32_i24_sdwa v47, sext(v52), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022D04: 125E60F9 0B0B0634
	v_add3_u32 v211, v45, v47, v211                            // 000000022D0C: D76D00D3 074E5F2D
	v_mul_i32_i24_sdwa v45, sext(v142), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022D14: 125A60F9 0808068E
	v_mul_i32_i24_sdwa v47, sext(v142), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022D1C: 125E60F9 0909068E
	v_add3_u32 v212, v45, v47, v212                            // 000000022D24: D76D00D4 07525F2D
	v_mul_i32_i24_sdwa v45, sext(v142), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022D2C: 125A60F9 0A0A068E
	v_mul_i32_i24_sdwa v47, sext(v142), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022D34: 125E60F9 0B0B068E
	v_add3_u32 v212, v45, v47, v212                            // 000000022D3C: D76D00D4 07525F2D
	v_mul_i32_i24_sdwa v45, sext(v46), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000022D44: 125A60F9 0808062E
	v_mul_i32_i24_sdwa v47, sext(v46), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000022D4C: 125E60F9 0909062E
	v_add3_u32 v213, v45, v47, v213                            // 000000022D54: D76D00D5 07565F2D
	v_mul_i32_i24_sdwa v45, sext(v46), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000022D5C: 125A60F9 0A0A062E
	v_mul_i32_i24_sdwa v47, sext(v46), sext(v48) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000022D64: 125E60F9 0B0B062E
	v_add3_u32 v213, v45, v47, v213                            // 000000022D6C: D76D00D5 07565F2D
	v_cvt_f32_i32_e32 v45, v198                                // 000000022D74: 7E5A0BC6
	v_cvt_f32_i32_e32 v46, v199                                // 000000022D78: 7E5C0BC7
	v_cvt_f32_i32_e32 v47, v200                                // 000000022D7C: 7E5E0BC8
	v_cvt_f32_i32_e32 v48, v201                                // 000000022D80: 7E600BC9
	v_cvt_f32_i32_e32 v51, v202                                // 000000022D84: 7E660BCA
	v_cvt_f32_i32_e32 v52, v203                                // 000000022D88: 7E680BCB
	v_cvt_f32_i32_e32 v53, v204                                // 000000022D8C: 7E6A0BCC
	v_cvt_f32_i32_e32 v54, v205                                // 000000022D90: 7E6C0BCD
	v_fmac_f32_e32 v6, v173, v45                               // 000000022D94: 560C5BAD
	v_fmac_f32_e32 v11, v71, v46                               // 000000022D98: 56165D47
	v_fmac_f32_e32 v18, v99, v47                               // 000000022D9C: 56245F63
	v_fmac_f32_e32 v15, v91, v48                               // 000000022DA0: 561E615B
	v_cvt_f32_i32_e32 v55, v206                                // 000000022DA4: 7E6E0BCE
	v_cvt_f32_i32_e32 v56, v207                                // 000000022DA8: 7E700BCF
	v_cvt_f32_i32_e32 v57, v208                                // 000000022DAC: 7E720BD0
	v_cvt_f32_i32_e32 v58, v209                                // 000000022DB0: 7E740BD1
	v_fmac_f32_e32 v6, v174, v51                               // 000000022DB4: 560C67AE
	v_fmac_f32_e32 v11, v72, v52                               // 000000022DB8: 56166948
	v_fmac_f32_e32 v18, v100, v53                              // 000000022DBC: 56246B64
	v_fmac_f32_e32 v15, v92, v54                               // 000000022DC0: 561E6D5C
	v_cvt_f32_i32_e32 v59, v210                                // 000000022DC4: 7E760BD2
	v_cvt_f32_i32_e32 v60, v211                                // 000000022DC8: 7E780BD3
	v_cvt_f32_i32_e32 v45, v212                                // 000000022DCC: 7E5A0BD4
	v_cvt_f32_i32_e32 v46, v213                                // 000000022DD0: 7E5C0BD5
	v_fmac_f32_e32 v6, v177, v55                               // 000000022DD4: 560C6FB1
	v_fmac_f32_e32 v11, v49, v56                               // 000000022DD8: 56167131
	v_fmac_f32_e32 v18, v107, v57                              // 000000022DDC: 5624736B
	v_fmac_f32_e32 v15, v179, v58                              // 000000022DE0: 561E75B3
	v_fmac_f32_e32 v6, v178, v59                               // 000000022DE4: 560C77B2
	v_fmac_f32_e32 v11, v50, v60                               // 000000022DE8: 56167932
	v_fmac_f32_e32 v18, v108, v45                              // 000000022DEC: 56245B6C
	v_fmac_f32_e32 v15, v180, v46                              // 000000022DF0: 561E5DB4
	s_barrier                                                  // 000000022DF4: BF8A0000
	buffer_gl0_inv                                             // 000000022DF8: E1C40000 00000000
	s_cbranch_scc1 61607                                       // 000000022E00: BF85F0A7 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x3a0>
	s_branch 4                                                 // 000000022E04: BF820004 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x4118>
	v_mov_b32_e32 v15, 0                                       // 000000022E08: 7E1E0280
	v_mov_b32_e32 v18, 0                                       // 000000022E0C: 7E240280
	v_mov_b32_e32 v11, 0                                       // 000000022E10: 7E160280
	v_mov_b32_e32 v6, 0                                        // 000000022E14: 7E0C0280
	s_not_b32 s1, s14                                          // 000000022E18: BE81070E
	s_add_i32 s0, s0, s1                                       // 000000022E1C: 81000100
	v_cmp_ge_i32_e32 vcc_lo, s0, v1                            // 000000022E20: 7D0C0200
	s_and_saveexec_b32 s0, vcc_lo                              // 000000022E24: BE803C6A
	s_cbranch_execz 28                                         // 000000022E28: BF88001C <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x419c>
	v_lshl_add_u32 v1, v1, 2, 0                                // 000000022E2C: D7460001 02010501
	s_waitcnt_vscnt null, 0x0                                  // 000000022E34: BBFD0000
	ds_read_b32 v1, v1                                         // 000000022E38: D8D80000 01000001
	s_waitcnt lgkmcnt(0)                                       // 000000022E40: BF8CC07F
	v_mad_u64_u32 v[0:1], s0, v1, s3, v[0:1]                   // 000000022E44: D5760000 04000701
	s_add_i32 s0, s16, s17                                     // 000000022E4C: 81001110
	s_ashr_i32 s1, s0, 31                                      // 000000022E50: 91019F00
	s_lshl_b64 s[0:1], s[0:1], 2                               // 000000022E54: 8F808200
	s_add_u32 s0, s12, s0                                      // 000000022E58: 8000000C
	v_ashrrev_i32_e32 v1, 31, v0                               // 000000022E5C: 3002009F
	s_addc_u32 s1, s13, s1                                     // 000000022E60: 8201010D
	v_lshlrev_b64 v[0:1], 2, v[0:1]                            // 000000022E64: D6FF0000 00020082
	v_add_co_u32 v0, vcc_lo, s0, v0                            // 000000022E6C: D70F6A00 00020000
	v_add_co_ci_u32_e32 v1, vcc_lo, s1, v1, vcc_lo             // 000000022E74: 50020201
	global_store_dword v[0:1], v6, off                         // 000000022E78: DC708000 007D0600
	global_store_dword v[0:1], v11, off offset:128             // 000000022E80: DC708080 007D0B00
	global_store_dword v[0:1], v18, off offset:256             // 000000022E88: DC708100 007D1200
	global_store_dword v[0:1], v15, off offset:384             // 000000022E90: DC708180 007D0F00
	s_endpgm                                                   // 000000022E98: BF810000
	s_endpgm                                                   // 000000022E9C: BF810000
		...

