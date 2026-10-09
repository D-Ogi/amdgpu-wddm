; mmq-instance-q8_0.nopatch.o: the disassembly of one kernel, nothing else.
; llvm-objdump -d, llvm-amdgpu 22.1.8, target gfx1013.
;
000000000001ed00 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_>:
	s_clause 0x3                                               // 00000001ED00: BFA10003
	s_load_dwordx2 s[24:25], s[4:5], 0x58                      // 00000001ED04: F4040602 FA000058
	s_load_dwordx2 s[10:11], s[4:5], 0x10                      // 00000001ED0C: F4040282 FA000010
	s_load_dwordx4 s[0:3], s[4:5], 0x48                        // 00000001ED14: F4080002 FA000048
	s_load_dwordx8 s[12:19], s[4:5], 0x64                      // 00000001ED1C: F40C0302 FA000064
	v_lshlrev_b32_e32 v2, 5, v1                                // 00000001ED24: 34040285
	v_add_nc_u32_e32 v4, v2, v0                                // 00000001ED28: 4A080102
	v_cmp_gt_u32_e32 vcc_lo, 8, v4                             // 00000001ED2C: 7D880888
	s_and_saveexec_b32 s9, vcc_lo                              // 00000001ED30: BE893C6A
	v_lshl_add_u32 v3, v4, 2, 0                                // 00000001ED34: D7460003 02010504
	ds_write_b32 v3, v4                                        // 00000001ED3C: D8340000 00000403
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
	s_mov_b32 s7, 0                                            // 00000001ED90: BE870380
	s_lshl_b64 s[12:13], s[26:27], 2                           // 00000001ED94: 8F8C821A
	s_mov_b32 s22, 0                                           // 00000001ED98: BE960380
	s_waitcnt lgkmcnt(0)                                       // 00000001ED9C: BF8CC07F
	s_add_u32 s12, s8, s12                                     // 00000001EDA0: 800C0C08
	s_addc_u32 s13, s9, s13                                    // 00000001EDA4: 820D0D09
	s_load_dwordx2 s[8:9], s[12:13], null                      // 00000001EDA8: F4040206 FA000000
	s_waitcnt lgkmcnt(0)                                       // 00000001EDB0: BF8CC07F
	s_sub_i32 s0, s9, s8                                       // 00000001EDB4: 81800809
	s_mov_b32 s9, 0                                            // 00000001EDB8: BE890380
	s_cmp_lt_i32 s14, s0                                       // 00000001EDBC: BF04000E
	s_cbranch_scc0 25                                          // 00000001EDC0: BF840019 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x128>
	s_and_saveexec_b32 s9, vcc_lo                              // 00000001EDC4: BE893C6A
	s_cbranch_execz 15                                         // 00000001EDC8: BF88000F <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x108>
	v_or_b32_e32 v3, s14, v0                                   // 00000001EDCC: 3806000E
	v_add_nc_u32_e32 v5, s8, v3                                // 00000001EDD0: 4A0A0608
	v_ashrrev_i32_e32 v6, 31, v5                               // 00000001EDD4: 300C0A9F
	v_lshlrev_b64 v[5:6], 2, v[5:6]                            // 00000001EDD8: D6FF0005 00020A82
	v_add_co_u32 v5, vcc_lo, s10, v5                           // 00000001EDE0: D70F6A05 00020A0A
	v_add_co_ci_u32_e32 v6, vcc_lo, s11, v6, vcc_lo            // 00000001EDE8: 500C0C0B
	global_load_dword v3, v[5:6], off                          // 00000001EDEC: DC308000 037D0005
	v_lshl_add_u32 v5, v4, 2, 0                                // 00000001EDF4: D7460005 02010504
	s_waitcnt vmcnt(0)                                         // 00000001EDFC: BF8C3F70
	ds_write_b32 v5, v3                                        // 00000001EE00: D8340000 00000305
	s_waitcnt_depctr depctr_vm_vsrc(0)                         // 00000001EE08: BFA3FF03
	s_or_b32 exec_lo, exec_lo, s9                              // 00000001EE0C: 887E097E
	s_waitcnt lgkmcnt(0)                                       // 00000001EE10: BF8CC07F
	s_barrier                                                  // 00000001EE14: BF8A0000
	s_mov_b32 s9, -1                                           // 00000001EE18: BE8903C1
	s_mov_b32 s22, s8                                          // 00000001EE1C: BE960308
	buffer_gl0_inv                                             // 00000001EE20: E1C40000 00000000
	s_mov_b32 s16, 0                                           // 00000001EE28: BE900380
	s_and_b32 vcc_lo, exec_lo, s9                              // 00000001EE2C: 876A097E
	s_cbranch_vccz 1657                                        // 00000001EE30: BF860679 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1b18>
	s_branch 10                                                // 00000001EE34: BF82000A <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x160>
	s_mul_i32 s9, s23, s22                                     // 00000001EE38: 93091617
	s_mul_i32 s10, s3, s14                                     // 00000001EE3C: 930A0E03
	s_mul_i32 s7, s23, s21                                     // 00000001EE40: 93071517
	s_mul_i32 s8, s26, s16                                     // 00000001EE44: 9308101A
	s_mul_i32 s11, s26, s17                                    // 00000001EE48: 930B111A
	s_add_i32 s9, s9, s10                                      // 00000001EE4C: 81090A09
	s_add_i32 s7, s8, s7                                       // 00000001EE50: 81070708
	s_add_i32 s16, s9, s11                                     // 00000001EE54: 81100B09
	s_mov_b32 s22, 0                                           // 00000001EE58: BE960380
	s_cbranch_execz 1646                                       // 00000001EE5C: BF88066E <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1b18>
	s_clause 0x1                                               // 00000001EE60: BFA10001
	s_load_dword s21, s[4:5], 0x40                             // 00000001EE64: F4000542 FA000040
	s_load_dwordx2 s[12:13], s[4:5], 0x20                      // 00000001EE6C: F4040302 FA000020
	s_lshl_b32 s17, s6, 7                                      // 00000001EE74: 8F118706
	s_waitcnt lgkmcnt(0)                                       // 00000001EE78: BF8CC07F
	s_cmp_lt_i32 s21, 1                                        // 00000001EE7C: BF048115
	s_cbranch_scc1 1600                                        // 00000001EE80: BF850640 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1a84>
	s_load_dwordx4 s[8:11], s[4:5], null                       // 00000001EE84: F4080202 FA000000
	s_add_i32 s5, s22, s14                                     // 00000001EE8C: 81050E16
	s_mul_i32 s4, s2, 36                                       // 00000001EE90: 9304A402
	s_mul_i32 s2, s5, 36                                       // 00000001EE94: 9302A405
	v_mul_lo_u32 v7, v1, s1                                    // 00000001EE98: D5690007 00000301
	s_add_i32 s28, s2, s7                                      // 00000001EEA0: 811C0702
	v_mul_u32_u24_e32 v10, 0x41, v1                            // 00000001EEA4: 161402FF 00000041
	s_ashr_i32 s29, s28, 31                                    // 00000001EEAC: 911D9F1C
	v_mul_u32_u24_e32 v11, 36, v1                              // 00000001EEB0: 161602A4
	s_lshl_b64 s[28:29], s[28:29], 2                           // 00000001EEB4: 8F9C821C
	v_add_nc_u32_e32 v14, 64, v0                               // 00000001EEB8: 4A1C00C0
	v_lshlrev_b32_e32 v19, 2, v10                              // 00000001EEBC: 34261482
	v_lshlrev_b32_e32 v13, 2, v0                               // 00000001EEC0: 341A0082
	v_lshlrev_b32_e32 v24, 2, v11                              // 00000001EEC4: 34301682
	v_lshlrev_b32_e32 v20, 5, v0                               // 00000001EEC8: 34280085
	v_and_b32_e32 v16, 0xfc, v0                                // 00000001EECC: 362000FF 000000FC
	v_and_b32_e32 v25, 0x1fc, v14                              // 00000001EED4: 36321CFF 000001FC
	v_add_nc_u32_e32 v12, 32, v0                               // 00000001EEDC: 4A1800A0
	v_add_nc_u32_e32 v15, 0x60, v0                             // 00000001EEE0: 4A1E00FF 00000060
	v_add_nc_u32_e32 v17, 0, v13                               // 00000001EEE8: 4A221A80
	s_waitcnt lgkmcnt(0)                                       // 00000001EEEC: BF8CC07F
	s_add_u32 s2, s10, s28                                     // 00000001EEF0: 80021C0A
	s_addc_u32 s10, s11, s29                                   // 00000001EEF4: 820A1D0B
	s_lshl_b32 s7, s1, 3                                       // 00000001EEF8: 8F078301
	v_add_nc_u32_e32 v44, v20, v16                             // 00000001EEFC: 4A582114
	v_add_nc_u32_e32 v8, s7, v7                                // 00000001EF00: 4A100E07
	v_lshrrev_b32_e32 v3, 3, v0                                // 00000001EF04: 2C060083
	v_and_b32_e32 v21, 0x1fc, v15                              // 00000001EF08: 362A1EFF 000001FC
	v_and_b32_e32 v26, 0x1fc, v12                              // 00000001EF10: 363418FF 000001FC
	v_add_nc_u32_e32 v12, v17, v19                             // 00000001EF18: 4A182711
	v_add_nc_u32_e32 v9, s7, v8                                // 00000001EF1C: 4A121007
	v_add3_u32 v13, 0, v19, v13                                // 00000001EF20: D76D000D 04362680
	v_lshl_add_u32 v18, v1, 2, v3                              // 00000001EF28: D7460012 040D0501
	v_add_nc_u32_e32 v41, v20, v21                             // 00000001EF30: 4A522B14
	v_and_b32_e32 v5, 7, v0                                    // 00000001EF34: 360A0087
	v_add_nc_u32_e32 v10, s7, v9                               // 00000001EF38: 4A141207
	v_add_nc_u32_e32 v43, v20, v26                             // 00000001EF3C: 4A563514
	v_add_nc_u32_e32 v28, 32, v18                              // 00000001EF40: 4A3824A0
	v_add_nc_u32_e32 v29, 64, v18                              // 00000001EF44: 4A3A24C0
	v_add_nc_u32_e32 v31, 0x60, v18                            // 00000001EF48: 4A3E24FF 00000060
	v_add_nc_u32_e32 v11, s7, v10                              // 00000001EF50: 4A161407
	v_lshlrev_b32_e32 v23, 2, v5                               // 00000001EF54: 342E0A82
	v_lshl_add_u32 v30, v18, 5, 0                              // 00000001EF58: D746001E 02010B12
	v_and_b32_e32 v27, 0x7fc, v18                              // 00000001EF60: 363624FF 000007FC
	v_and_b32_e32 v32, 0xffc, v28                              // 00000001EF68: 364038FF 00000FFC
	v_add_nc_u32_e32 v14, s7, v11                              // 00000001EF70: 4A1C1607
	v_and_b32_e32 v33, 0xffc, v29                              // 00000001EF74: 36423AFF 00000FFC
	v_and_b32_e32 v31, 0xffc, v31                              // 00000001EF7C: 363E3EFF 00000FFC
	v_add_nc_u32_e32 v42, v20, v25                             // 00000001EF84: 4A543314
	v_add3_u32 v20, v30, v27, v23                              // 00000001EF88: D76D0014 045E371E
	v_add_nc_u32_e32 v16, s7, v14                              // 00000001EF90: 4A201C07
	v_add_nc_u32_e32 v34, v30, v32                             // 00000001EF94: 4A44411E
	v_add_nc_u32_e32 v35, v30, v33                             // 00000001EF98: 4A46431E
	v_add_nc_u32_e32 v36, v30, v31                             // 00000001EF9C: 4A483F1E
	v_mul_lo_u32 v15, v18, s1                                  // 00000001EFA0: D569000F 00000312
	v_add_nc_u32_e32 v19, s7, v16                              // 00000001EFA8: 4A262007
	s_mul_i32 s6, s1, s17                                      // 00000001EFAC: 93061101
	s_lshl_b32 s1, s1, 5                                       // 00000001EFB0: 8F018501
	s_mul_hi_u32 s22, s26, s24                                 // 00000001EFB4: 9A96181A
	s_mul_hi_u32 s24, s23, s18                                 // 00000001EFB8: 9A981217
	v_add_nc_u32_e32 v21, s7, v19                              // 00000001EFBC: 4A2A2607
	s_add_i32 s22, s26, s22                                    // 00000001EFC0: 8116161A
	v_add_nc_u32_e32 v32, s1, v15                              // 00000001EFC4: 4A401E01
	s_add_i32 s23, s23, s24                                    // 00000001EFC8: 81171817
	v_lshlrev_b32_e32 v22, 1, v5                               // 00000001EFCC: 342C0A81
	v_add_nc_u32_e32 v26, s7, v21                              // 00000001EFD0: 4A342A07
	s_lshr_b32 s22, s22, s25                                   // 00000001EFD4: 90161916
	v_add_nc_u32_e32 v37, s1, v32                              // 00000001EFD8: 4A4A4001
	s_lshr_b32 s19, s23, s19                                   // 00000001EFDC: 90131317
	v_lshl_add_u32 v17, v2, 2, v17                             // 00000001EFE0: D7460011 04450502
	v_add_nc_u32_e32 v29, s7, v26                              // 00000001EFE8: 4A3A3407
	v_mad_u64_u32 v[2:3], s5, v3, 34, s[8:9]                   // 00000001EFEC: D5760502 00214503
	v_lshlrev_b32_e32 v46, 2, v4                               // 00000001EFF4: 345C0882
	v_mad_u64_u32 v[4:5], s8, v5, 34, s[8:9]                   // 00000001EFF8: D5760804 00214505
	v_add_nc_u32_e32 v30, s7, v29                              // 00000001F000: 4A3C3A07
	s_movk_i32 s11, 0x104                                      // 00000001F004: B00B0104
	s_mul_i32 s22, s22, s15                                    // 00000001F008: 93160F16
	s_mul_i32 s15, s19, s20                                    // 00000001F00C: 930F1413
	s_movk_i32 s19, 0x8a0                                      // 00000001F010: B01308A0
	v_add_nc_u32_e32 v31, s7, v30                              // 00000001F014: 4A3E3C07
	v_mov_b32_e32 v6, 0                                        // 00000001F018: 7E0C0280
	v_add_nc_u32_e32 v18, 48, v24                              // 00000001F01C: 4A2430B0
	v_add_nc_u32_e32 v25, 0x9620, v41                          // 00000001F020: 4A3252FF 00009620
	v_add_nc_u32_e32 v27, 0x9220, v42                          // 00000001F028: 4A3654FF 00009220
	v_add_nc_u32_e32 v33, s7, v31                              // 00000001F030: 4A423E07
	v_add_nc_u32_e32 v28, 0x8e20, v43                          // 00000001F034: 4A3856FF 00008E20
	v_add3_u32 v34, v34, v23, 0x400                            // 00000001F03C: D76D0022 03FE2F22 00000400
	v_add3_u32 v35, v35, v23, 0x800                            // 00000001F048: D76D0023 03FE2F23 00000800
	v_add3_u32 v36, v36, v23, 0xc00                            // 00000001F054: D76D0024 03FE2F24 00000C00
	v_add_nc_u32_e32 v38, s7, v33                              // 00000001F060: 4A4C4207
	v_add_nc_u32_e32 v39, 0x8a20, v44                          // 00000001F064: 4A4E58FF 00008A20
	v_add_nc_u32_e32 v40, 32, v24                              // 00000001F06C: 4A5030A0
	v_add_nc_u32_e32 v41, 0x9630, v41                          // 00000001F070: 4A5252FF 00009630
	v_add_nc_u32_e32 v42, 0x9230, v42                          // 00000001F078: 4A5454FF 00009230
	v_add_nc_u32_e32 v43, 0x8e30, v43                          // 00000001F080: 4A5656FF 00008E30
	v_add_nc_u32_e32 v44, 0x8a30, v44                          // 00000001F088: 4A5858FF 00008A30
	v_lshlrev_b32_e32 v45, 1, v22                              // 00000001F090: 345A2C81
	v_add_nc_u32_e32 v47, s7, v38                              // 00000001F094: 4A5E4C07
	v_add_nc_u32_e32 v48, s1, v37                              // 00000001F098: 4A604A01
	v_mad_u32_u24 v49, v0, s11, 0x820                          // 00000001F09C: D5430031 03FC1700 00000820
	v_mad_u32_u24 v50, 0x104, v0, s19                          // 00000001F0A8: D5430032 004E00FF 00000104
	v_mov_b32_e32 v24, 0                                       // 00000001F0B4: 7E300280
	v_mov_b32_e32 v23, 0                                       // 00000001F0B8: 7E2E0280
	v_mov_b32_e32 v22, 0                                       // 00000001F0BC: 7E2C0280
	s_ashr_i32 s5, s4, 31                                      // 00000001F0C0: 91059F04
	s_add_i32 s1, s15, s6                                      // 00000001F0C4: 8101060F
	s_mov_b32 s18, 0                                           // 00000001F0C8: BE920380
	s_lshl_b64 s[6:7], s[4:5], 2                               // 00000001F0CC: 8F868204
	s_add_i32 s1, s1, s22                                      // 00000001F0D0: 81011601
	s_add_i32 s5, s1, s18                                      // 00000001F0D4: 81051201
	v_mad_i64_i32 v[51:52], s8, s5, 34, v[2:3]                 // 00000001F0D8: D5770833 04094405
	v_mad_i64_i32 v[53:54], s8, v7, 34, v[51:52]               // 00000001F0E0: D5770835 04CD4507
	v_mad_i64_i32 v[55:56], s8, v8, 34, v[51:52]               // 00000001F0E8: D5770837 04CD4508
	v_mad_i64_i32 v[57:58], s8, v9, 34, v[51:52]               // 00000001F0F0: D5770839 04CD4509
	v_mad_i64_i32 v[59:60], s8, v10, 34, v[51:52]              // 00000001F0F8: D577083B 04CD450A
	v_add_co_u32 v53, vcc_lo, v53, v45                         // 00000001F100: D70F6A35 00025B35
	v_add_co_ci_u32_e32 v54, vcc_lo, 0, v54, vcc_lo            // 00000001F108: 506C6C80
	v_add_co_u32 v55, vcc_lo, v55, v45                         // 00000001F10C: D70F6A37 00025B37
	v_mad_i64_i32 v[61:62], s8, v11, 34, v[51:52]              // 00000001F114: D577083D 04CD450B
	v_add_co_ci_u32_e32 v56, vcc_lo, 0, v56, vcc_lo            // 00000001F11C: 50707080
	v_add_co_u32 v57, vcc_lo, v57, v45                         // 00000001F120: D70F6A39 00025B39
	v_mad_i64_i32 v[63:64], s8, v14, 34, v[51:52]              // 00000001F128: D577083F 04CD450E
	v_add_co_ci_u32_e32 v58, vcc_lo, 0, v58, vcc_lo            // 00000001F130: 50747480
	v_add_co_u32 v59, vcc_lo, v59, v45                         // 00000001F134: D70F6A3B 00025B3B
	v_mad_i64_i32 v[65:66], s8, v16, 34, v[51:52]              // 00000001F13C: D5770841 04CD4510
	v_add_co_ci_u32_e32 v60, vcc_lo, 0, v60, vcc_lo            // 00000001F144: 50787880
	v_add_co_u32 v61, vcc_lo, v61, v45                         // 00000001F148: D70F6A3D 00025B3D
	v_mad_i64_i32 v[67:68], s8, v19, 34, v[51:52]              // 00000001F150: D5770843 04CD4513
	v_add_co_ci_u32_e32 v62, vcc_lo, 0, v62, vcc_lo            // 00000001F158: 507C7C80
	v_add_co_u32 v63, vcc_lo, v63, v45                         // 00000001F15C: D70F6A3F 00025B3F
	v_mad_i64_i32 v[69:70], s8, v21, 34, v[51:52]              // 00000001F164: D5770845 04CD4515
	v_add_co_ci_u32_e32 v64, vcc_lo, 0, v64, vcc_lo            // 00000001F16C: 50808080
	v_add_co_u32 v65, vcc_lo, v65, v45                         // 00000001F170: D70F6A41 00025B41
	v_mad_i64_i32 v[71:72], s8, v26, 34, v[51:52]              // 00000001F178: D5770847 04CD451A
	v_add_co_ci_u32_e32 v66, vcc_lo, 0, v66, vcc_lo            // 00000001F180: 50848480
	v_add_co_u32 v67, vcc_lo, v67, v45                         // 00000001F184: D70F6A43 00025B43
	v_mad_i64_i32 v[73:74], s8, v29, 34, v[51:52]              // 00000001F18C: D5770849 04CD451D
	v_add_co_ci_u32_e32 v68, vcc_lo, 0, v68, vcc_lo            // 00000001F194: 50888880
	v_add_co_u32 v69, vcc_lo, v69, v45                         // 00000001F198: D70F6A45 00025B45
	v_mad_i64_i32 v[75:76], s8, v30, 34, v[51:52]              // 00000001F1A0: D577084B 04CD451E
	v_add_co_ci_u32_e32 v70, vcc_lo, 0, v70, vcc_lo            // 00000001F1A8: 508C8C80
	v_add_co_u32 v71, vcc_lo, v71, v45                         // 00000001F1AC: D70F6A47 00025B47
	v_mad_i64_i32 v[77:78], s8, v31, 34, v[51:52]              // 00000001F1B4: D577084D 04CD451F
	v_add_co_ci_u32_e32 v72, vcc_lo, 0, v72, vcc_lo            // 00000001F1BC: 50909080
	s_clause 0x7                                               // 00000001F1C0: BFA10007
	global_load_dword v83, v[53:54], off offset:2              // 00000001F1C4: DC308002 537D0035
	global_load_dword v84, v[53:54], off offset:138            // 00000001F1CC: DC30808A 547D0035
	global_load_dword v85, v[55:56], off offset:2              // 00000001F1D4: DC308002 557D0037
	global_load_dword v86, v[55:56], off offset:138            // 00000001F1DC: DC30808A 567D0037
	global_load_dword v87, v[57:58], off offset:2              // 00000001F1E4: DC308002 577D0039
	global_load_dword v88, v[57:58], off offset:138            // 00000001F1EC: DC30808A 587D0039
	global_load_dword v89, v[59:60], off offset:2              // 00000001F1F4: DC308002 597D003B
	global_load_dword v90, v[59:60], off offset:138            // 00000001F1FC: DC30808A 5A7D003B
	v_mad_i64_i32 v[53:54], s5, s5, 34, v[4:5]                 // 00000001F204: D5770535 04114405
	v_add_co_u32 v73, vcc_lo, v73, v45                         // 00000001F20C: D70F6A49 00025B49
	v_mad_i64_i32 v[79:80], s8, v33, 34, v[51:52]              // 00000001F214: D577084F 04CD4521
	v_add_co_ci_u32_e32 v74, vcc_lo, 0, v74, vcc_lo            // 00000001F21C: 50949480
	v_add_co_u32 v75, vcc_lo, v75, v45                         // 00000001F220: D70F6A4B 00025B4B
	v_mad_i64_i32 v[81:82], s8, v38, 34, v[51:52]              // 00000001F228: D5770851 04CD4526
	v_add_co_ci_u32_e32 v76, vcc_lo, 0, v76, vcc_lo            // 00000001F230: 50989880
	v_add_co_u32 v77, vcc_lo, v77, v45                         // 00000001F234: D70F6A4D 00025B4D
	v_mad_i64_i32 v[51:52], s8, v47, 34, v[51:52]              // 00000001F23C: D5770833 04CD452F
	v_mad_i64_i32 v[55:56], s5, v15, 34, v[53:54]              // 00000001F244: D5770537 04D5450F
	v_add_co_ci_u32_e32 v78, vcc_lo, 0, v78, vcc_lo            // 00000001F24C: 509C9C80
	s_lshr_b32 s5, s18, 2                                      // 00000001F250: 90058212
	v_add_co_u32 v79, vcc_lo, v79, v45                         // 00000001F254: D70F6A4F 00025B4F
	v_mad_i64_i32 v[57:58], s8, v32, 34, v[53:54]              // 00000001F25C: D5770839 04D54520
	s_mul_i32 s8, s4, s5                                       // 00000001F264: 93080504
	v_add_co_ci_u32_e32 v80, vcc_lo, 0, v80, vcc_lo            // 00000001F268: 50A0A080
	v_mad_i64_i32 v[59:60], s5, v37, 34, v[53:54]              // 00000001F26C: D577053B 04D54525
	v_add_co_u32 v81, vcc_lo, v81, v45                         // 00000001F274: D70F6A51 00025B51
	s_ashr_i32 s9, s8, 31                                      // 00000001F27C: 91099F08
	v_mad_i64_i32 v[53:54], s5, v48, 34, v[53:54]              // 00000001F280: D5770535 04D54530
	v_add_co_ci_u32_e32 v82, vcc_lo, 0, v82, vcc_lo            // 00000001F288: 50A4A480
	s_lshl_b64 s[8:9], s[8:9], 2                               // 00000001F28C: 8F888208
	v_add_co_u32 v51, vcc_lo, v51, v45                         // 00000001F290: D70F6A33 00025B33
	s_add_u32 s8, s2, s8                                       // 00000001F298: 80080802
	v_add_co_ci_u32_e32 v52, vcc_lo, 0, v52, vcc_lo            // 00000001F29C: 50686880
	s_clause 0xf                                               // 00000001F2A0: BFA1000F
	global_load_dword v91, v[61:62], off offset:2              // 00000001F2A4: DC308002 5B7D003D
	global_load_dword v92, v[61:62], off offset:138            // 00000001F2AC: DC30808A 5C7D003D
	global_load_dword v93, v[63:64], off offset:2              // 00000001F2B4: DC308002 5D7D003F
	global_load_dword v94, v[63:64], off offset:138            // 00000001F2BC: DC30808A 5E7D003F
	global_load_dword v95, v[65:66], off offset:2              // 00000001F2C4: DC308002 5F7D0041
	global_load_dword v96, v[65:66], off offset:138            // 00000001F2CC: DC30808A 607D0041
	global_load_dword v97, v[67:68], off offset:2              // 00000001F2D4: DC308002 617D0043
	global_load_dword v98, v[67:68], off offset:138            // 00000001F2DC: DC30808A 627D0043
	global_load_dword v61, v[69:70], off offset:2              // 00000001F2E4: DC308002 3D7D0045
	global_load_dword v62, v[69:70], off offset:138            // 00000001F2EC: DC30808A 3E7D0045
	global_load_dword v63, v[71:72], off offset:2              // 00000001F2F4: DC308002 3F7D0047
	global_load_dword v64, v[71:72], off offset:138            // 00000001F2FC: DC30808A 407D0047
	global_load_dword v65, v[73:74], off offset:2              // 00000001F304: DC308002 417D0049
	global_load_dword v66, v[73:74], off offset:138            // 00000001F30C: DC30808A 427D0049
	global_load_dword v67, v[75:76], off offset:2              // 00000001F314: DC308002 437D004B
	global_load_dword v68, v[75:76], off offset:138            // 00000001F31C: DC30808A 447D004B
	s_addc_u32 s9, s10, s9                                     // 00000001F324: 8209090A
	s_clause 0x3                                               // 00000001F328: BFA10003
	global_load_ushort v69, v[55:56], off                      // 00000001F32C: DC288000 457D0037
	global_load_ushort v70, v[57:58], off                      // 00000001F334: DC288000 467D0039
	global_load_ushort v71, v[59:60], off                      // 00000001F33C: DC288000 477D003B
	global_load_ushort v72, v[53:54], off                      // 00000001F344: DC288000 487D0035
	s_clause 0x1                                               // 00000001F34C: BFA10001
	global_load_dword v73, v46, s[8:9]                         // 00000001F350: DC308000 4908002E
	global_load_dword v74, v46, s[8:9] offset:1024             // 00000001F358: DC308400 4A08002E
	s_clause 0x7                                               // 00000001F360: BFA10007
	global_load_dword v75, v[77:78], off offset:2              // 00000001F364: DC308002 4B7D004D
	global_load_dword v76, v[77:78], off offset:138            // 00000001F36C: DC30808A 4C7D004D
	global_load_dword v99, v[79:80], off offset:2              // 00000001F374: DC308002 637D004F
	global_load_dword v100, v[79:80], off offset:138           // 00000001F37C: DC30808A 647D004F
	global_load_dword v101, v[81:82], off offset:2             // 00000001F384: DC308002 657D0051
	global_load_dword v102, v[81:82], off offset:138           // 00000001F38C: DC30808A 667D0051
	global_load_dword v103, v[51:52], off offset:2             // 00000001F394: DC308002 677D0033
	global_load_dword v104, v[51:52], off offset:138           // 00000001F39C: DC30808A 687D0033
	v_add_nc_u32_e32 v51, 32, v17                              // 00000001F3A4: 4A6622A0
	v_mov_b32_e32 v52, v40                                     // 00000001F3A8: 7E680328
	v_mov_b32_e32 v53, v39                                     // 00000001F3AC: 7E6A0327
	v_mov_b32_e32 v54, v28                                     // 00000001F3B0: 7E6C031C
	v_mov_b32_e32 v55, v27                                     // 00000001F3B4: 7E6E031B
	v_mov_b32_e32 v56, v25                                     // 00000001F3B8: 7E700319
	v_mov_b32_e32 v57, v49                                     // 00000001F3BC: 7E720331
	v_mov_b32_e32 v58, v18                                     // 00000001F3C0: 7E740312
	s_mov_b32 s5, -8                                           // 00000001F3C4: BE8503C8
	s_waitcnt vmcnt(13)                                        // 00000001F3C8: BF8C3F7D
	v_cvt_f32_f16_e32 v59, v69                                 // 00000001F3CC: 7E761745
	s_waitcnt vmcnt(12)                                        // 00000001F3D0: BF8C3F7C
	v_cvt_f32_f16_e32 v60, v70                                 // 00000001F3D4: 7E781746
	s_waitcnt vmcnt(11)                                        // 00000001F3D8: BF8C3F7B
	v_cvt_f32_f16_e32 v69, v71                                 // 00000001F3DC: 7E8A1747
	s_waitcnt vmcnt(10)                                        // 00000001F3E0: BF8C3F7A
	v_cvt_f32_f16_e32 v70, v72                                 // 00000001F3E4: 7E8C1748
	ds_write_b32 v12, v83 offset:2080                          // 00000001F3E8: D8340820 0000530C
	ds_write_b32 v13, v84 offset:2208                          // 00000001F3F0: D83408A0 0000540D
	ds_write_b32 v12, v85 offset:4160                          // 00000001F3F8: D8341040 0000550C
	ds_write_b32 v13, v86 offset:4288                          // 00000001F400: D83410C0 0000560D
	ds_write_b32 v12, v87 offset:6240                          // 00000001F408: D8341860 0000570C
	ds_write_b32 v13, v88 offset:6368                          // 00000001F410: D83418E0 0000580D
	ds_write_b32 v12, v89 offset:8320                          // 00000001F418: D8342080 0000590C
	ds_write_b32 v13, v90 offset:8448                          // 00000001F420: D8342100 00005A0D
	ds_write_b32 v12, v91 offset:10400                         // 00000001F428: D83428A0 00005B0C
	ds_write_b32 v13, v92 offset:10528                         // 00000001F430: D8342920 00005C0D
	ds_write_b32 v12, v93 offset:12480                         // 00000001F438: D83430C0 00005D0C
	ds_write_b32 v13, v94 offset:12608                         // 00000001F440: D8343140 00005E0D
	ds_write_b32 v12, v95 offset:14560                         // 00000001F448: D83438E0 00005F0C
	ds_write_b32 v13, v96 offset:14688                         // 00000001F450: D8343960 0000600D
	ds_write_b32 v12, v97 offset:16640                         // 00000001F458: D8344100 0000610C
	ds_write_b32 v13, v98 offset:16768                         // 00000001F460: D8344180 0000620D
	ds_write_b32 v12, v61 offset:18720                         // 00000001F468: D8344920 00003D0C
	ds_write_b32 v13, v62 offset:18848                         // 00000001F470: D83449A0 00003E0D
	ds_write_b32 v12, v63 offset:20800                         // 00000001F478: D8345140 00003F0C
	ds_write_b32 v13, v64 offset:20928                         // 00000001F480: D83451C0 0000400D
	ds_write_b32 v12, v65 offset:22880                         // 00000001F488: D8345960 0000410C
	ds_write_b32 v13, v66 offset:23008                         // 00000001F490: D83459E0 0000420D
	ds_write_b32 v12, v67 offset:24960                         // 00000001F498: D8346180 0000430C
	ds_write_b32 v13, v68 offset:25088                         // 00000001F4A0: D8346200 0000440D
	s_waitcnt vmcnt(7)                                         // 00000001F4A8: BF8C3F77
	ds_write_b32 v12, v75 offset:27040                         // 00000001F4AC: D83469A0 00004B0C
	s_waitcnt vmcnt(6)                                         // 00000001F4B4: BF8C3F76
	ds_write_b32 v13, v76 offset:27168                         // 00000001F4B8: D8346A20 00004C0D
	s_waitcnt vmcnt(5)                                         // 00000001F4C0: BF8C3F75
	ds_write_b32 v12, v99 offset:29120                         // 00000001F4C4: D83471C0 0000630C
	s_waitcnt vmcnt(4)                                         // 00000001F4CC: BF8C3F74
	ds_write_b32 v13, v100 offset:29248                        // 00000001F4D0: D8347240 0000640D
	s_waitcnt vmcnt(3)                                         // 00000001F4D8: BF8C3F73
	ds_write_b32 v12, v101 offset:31200                        // 00000001F4DC: D83479E0 0000650C
	s_waitcnt vmcnt(2)                                         // 00000001F4E4: BF8C3F72
	ds_write_b32 v13, v102 offset:31328                        // 00000001F4E8: D8347A60 0000660D
	s_waitcnt vmcnt(1)                                         // 00000001F4F0: BF8C3F71
	ds_write_b32 v12, v103 offset:33280                        // 00000001F4F4: D8348200 0000670C
	s_waitcnt vmcnt(0)                                         // 00000001F4FC: BF8C3F70
	ds_write_b32 v13, v104 offset:33408                        // 00000001F500: D8348280 0000680D
	ds_write_b32 v20, v59 offset:35360                         // 00000001F508: D8348A20 00003B14
	ds_write_b32 v34, v60 offset:35360                         // 00000001F510: D8348A20 00003C22
	ds_write_b32 v35, v69 offset:35360                         // 00000001F518: D8348A20 00004523
	ds_write_b32 v36, v70 offset:35360                         // 00000001F520: D8348A20 00004624
	ds_write2st64_b32 v51, v73, v74 offset1:4                  // 00000001F528: D83C0400 004A4933
	s_waitcnt lgkmcnt(0)                                       // 00000001F530: BF8CC07F
	s_barrier                                                  // 00000001F534: BF8A0000
	buffer_gl0_inv                                             // 00000001F538: E1C40000 00000000
	v_add_nc_u32_e32 v59, 0, v57                               // 00000001F540: 4A767280
	v_add_nc_u32_e32 v60, 0, v58                               // 00000001F544: 4A787480
	v_add_nc_u32_e32 v77, 0, v52                               // 00000001F548: 4A9A6880
	v_add_nc_u32_e32 v78, 0, v53                               // 00000001F54C: 4A9C6A80
	v_add_nc_u32_e32 v79, 0, v54                               // 00000001F550: 4A9E6C80
	ds_read2_b32 v[61:62], v59 offset1:1                       // 00000001F554: D8DC0100 3D00003B
	ds_read2_b32 v[63:64], v59 offset0:2 offset1:3             // 00000001F55C: D8DC0302 3F00003B
	ds_read2_b32 v[65:66], v59 offset0:4 offset1:5             // 00000001F564: D8DC0504 4100003B
	ds_read2_b32 v[67:68], v59 offset0:6 offset1:7             // 00000001F56C: D8DC0706 4300003B
	ds_read2_b32 v[69:70], v60 offset1:1                       // 00000001F574: D8DC0100 4500003C
	ds_read2_b32 v[71:72], v60 offset0:2 offset1:3             // 00000001F57C: D8DC0302 4700003C
	ds_read2_b32 v[73:74], v60 offset0:4 offset1:5             // 00000001F584: D8DC0504 4900003C
	ds_read2_b32 v[75:76], v60 offset0:6 offset1:7             // 00000001F58C: D8DC0706 4B00003C
	v_add_nc_u32_e32 v80, 0, v55                               // 00000001F594: 4AA06E80
	v_add_nc_u32_e32 v81, 0, v56                               // 00000001F598: 4AA27080
	v_add_nc_u32_e32 v82, 0x2000, v59                          // 00000001F59C: 4AA476FF 00002000
	v_add_nc_u32_e32 v89, 0x4000, v59                          // 00000001F5A4: 4AB276FF 00004000
	v_add_nc_u32_e32 v97, 0x6000, v59                          // 00000001F5AC: 4AC276FF 00006000
	ds_read_b32 v99, v77                                       // 00000001F5B4: D8D80000 6300004D
	ds_read_b32 v100, v78                                      // 00000001F5BC: D8D80000 6400004E
	ds_read_b32 v101, v79                                      // 00000001F5C4: D8D80000 6500004F
	ds_read_b32 v102, v80                                      // 00000001F5CC: D8D80000 66000050
	ds_read_b32 v103, v81                                      // 00000001F5D4: D8D80000 67000051
	ds_read2_b32 v[59:60], v82 offset0:32 offset1:33           // 00000001F5DC: D8DC2120 3B000052
	ds_read2_b32 v[77:78], v82 offset0:34 offset1:35           // 00000001F5E4: D8DC2322 4D000052
	ds_read2_b32 v[79:80], v82 offset0:36 offset1:37           // 00000001F5EC: D8DC2524 4F000052
	ds_read2_b32 v[81:82], v82 offset0:38 offset1:39           // 00000001F5F4: D8DC2726 51000052
	ds_read2_b32 v[83:84], v89 offset0:64 offset1:65           // 00000001F5FC: D8DC4140 53000059
	ds_read2_b32 v[85:86], v89 offset0:66 offset1:67           // 00000001F604: D8DC4342 55000059
	ds_read2_b32 v[87:88], v89 offset0:68 offset1:69           // 00000001F60C: D8DC4544 57000059
	ds_read2_b32 v[89:90], v89 offset0:70 offset1:71           // 00000001F614: D8DC4746 59000059
	ds_read2_b32 v[91:92], v97 offset0:96 offset1:97           // 00000001F61C: D8DC6160 5B000061
	ds_read2_b32 v[93:94], v97 offset0:98 offset1:99           // 00000001F624: D8DC6362 5D000061
	ds_read2_b32 v[95:96], v97 offset0:100 offset1:101         // 00000001F62C: D8DC6564 5F000061
	ds_read2_b32 v[97:98], v97 offset0:102 offset1:103         // 00000001F634: D8DC6766 61000061
	v_add_nc_u32_e32 v58, 32, v58                              // 00000001F63C: 4A7474A0
	v_add_nc_u32_e32 v57, 32, v57                              // 00000001F640: 4A7272A0
	v_add_nc_u32_e32 v56, 4, v56                               // 00000001F644: 4A707084
	v_add_nc_u32_e32 v55, 4, v55                               // 00000001F648: 4A6E6E84
	s_waitcnt lgkmcnt(15)                                      // 00000001F64C: BF8CCF7F
	v_mul_f32_e32 v100, v100, v99                              // 00000001F650: 10C8C764
	s_waitcnt lgkmcnt(14)                                      // 00000001F654: BF8CCE7F
	v_mul_f32_e32 v101, v101, v99                              // 00000001F658: 10CAC765
	v_add_nc_u32_e32 v54, 4, v54                               // 00000001F65C: 4A6C6C84
	v_add_nc_u32_e32 v53, 4, v53                               // 00000001F660: 4A6A6A84
	s_waitcnt lgkmcnt(11)                                      // 00000001F664: BF8CCB7F
	v_lshrrev_b16 v138, 8, v60                                 // 00000001F668: D707008A 00027888
	v_lshrrev_b16 v104, 8, v61                                 // 00000001F670: D7070068 00027A88
	v_lshrrev_b16 v105, 8, v69                                 // 00000001F678: D7070069 00028A88
	v_bfe_i32 v106, v61, 16, 8                                 // 00000001F680: D549006A 0221213D
	v_bfe_i32 v107, v61, 0, 8                                  // 00000001F688: D549006B 0221013D
	v_ashrrev_i32_e32 v61, 24, v61                             // 00000001F690: 307A7A98
	v_bfe_i32 v108, v69, 16, 8                                 // 00000001F694: D549006C 02212145
	v_bfe_i32 v109, v69, 0, 8                                  // 00000001F69C: D549006D 02210145
	v_ashrrev_i32_e32 v69, 24, v69                             // 00000001F6A4: 308A8A98
	v_lshrrev_b16 v110, 8, v62                                 // 00000001F6A8: D707006E 00027C88
	v_lshrrev_b16 v111, 8, v70                                 // 00000001F6B0: D707006F 00028C88
	v_mul_i32_i24_sdwa v112, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F6B8: 12E07CF9 08080646
	v_mul_i32_i24_sdwa v113, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F6C0: 12E27CF9 0A0A0646
	v_mul_i32_i24_sdwa v62, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F6C8: 127C7CF9 0B0B0646
	v_lshrrev_b16 v114, 8, v63                                 // 00000001F6D0: D7070072 00027E88
	v_lshrrev_b16 v115, 8, v71                                 // 00000001F6D8: D7070073 00028E88
	v_mul_i32_i24_sdwa v116, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F6E0: 12E87EF9 08080647
	v_mul_i32_i24_sdwa v117, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F6E8: 12EA7EF9 0A0A0647
	v_mul_i32_i24_sdwa v63, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F6F0: 127E7EF9 0B0B0647
	v_lshrrev_b16 v118, 8, v64                                 // 00000001F6F8: D7070076 00028088
	v_lshrrev_b16 v119, 8, v72                                 // 00000001F700: D7070077 00029088
	v_mul_i32_i24_sdwa v120, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F708: 12F080F9 08080648
	v_mul_i32_i24_sdwa v121, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F710: 12F280F9 0A0A0648
	v_mul_i32_i24_sdwa v64, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F718: 128080F9 0B0B0648
	v_lshrrev_b16 v122, 8, v65                                 // 00000001F720: D707007A 00028288
	v_lshrrev_b16 v134, 8, v73                                 // 00000001F728: D7070086 00029288
	v_mul_i32_i24_sdwa v104, sext(v105), sext(v104) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F730: 12D0D0F9 08080669
	v_mul_i32_i24_sdwa v110, sext(v111), sext(v110) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F738: 12DCDCF9 0808066F
	v_mul_i32_i24_sdwa v114, sext(v115), sext(v114) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F740: 12E4E4F9 08080673
	v_mad_i32_i24 v61, v69, v61, v62                           // 00000001F748: D542003D 04FA7B45
	v_mad_i32_i24 v62, v109, v107, v112                        // 00000001F750: D542003E 05C2D76D
	v_mad_i32_i24 v106, v108, v106, v113                       // 00000001F758: D542006A 05C6D56C
	v_lshrrev_b16 v107, 8, v59                                 // 00000001F760: D707006B 00027688
	v_bfe_i32 v112, v59, 16, 8                                 // 00000001F768: D5490070 0221213B
	v_bfe_i32 v113, v59, 0, 8                                  // 00000001F770: D5490071 0221013B
	v_ashrrev_i32_e32 v59, 24, v59                             // 00000001F778: 30767698
	v_mul_i32_i24_sdwa v139, sext(v70), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F77C: 131678F9 08080646
	v_mul_i32_i24_sdwa v140, sext(v70), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F784: 131878F9 0A0A0646
	v_mul_i32_i24_sdwa v60, sext(v70), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F78C: 127878F9 0B0B0646
	s_waitcnt lgkmcnt(10)                                      // 00000001F794: BF8CCA7F
	v_lshrrev_b16 v141, 8, v77                                 // 00000001F798: D707008D 00029A88
	v_mul_i32_i24_sdwa v142, sext(v71), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F7A0: 131C9AF9 08080647
	v_mul_i32_i24_sdwa v143, sext(v71), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F7A8: 131E9AF9 0A0A0647
	v_mul_i32_i24_sdwa v77, sext(v71), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F7B0: 129A9AF9 0B0B0647
	s_waitcnt lgkmcnt(7)                                       // 00000001F7B8: BF8CC77F
	v_ashrrev_i32_e32 v153, 24, v83                            // 00000001F7BC: 3132A698
	v_mul_i32_i24_sdwa v154, sext(v70), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F7C0: 1334A8F9 0B0B0646
	s_waitcnt lgkmcnt(3)                                       // 00000001F7C8: BF8CC37F
	v_ashrrev_i32_e32 v155, 24, v91                            // 00000001F7CC: 3136B698
	v_mul_i32_i24_sdwa v156, sext(v70), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F7D0: 1338B8F9 0B0B0646
	v_bfe_i32 v157, v83, 0, 8                                  // 00000001F7D8: D549009D 02210153
	v_mul_i32_i24_sdwa v158, sext(v70), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F7E0: 133CA8F9 08080646
	v_bfe_i32 v159, v91, 0, 8                                  // 00000001F7E8: D549009F 0221015B
	v_mul_i32_i24_sdwa v160, sext(v70), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F7F0: 1340B8F9 08080646
	v_bfe_i32 v161, v83, 16, 8                                 // 00000001F7F8: D54900A1 02212153
	v_mul_i32_i24_sdwa v162, sext(v70), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F800: 1344A8F9 0A0A0646
	v_mul_i32_i24_sdwa v70, sext(v70), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F808: 128CB8F9 0A0A0646
	v_bfe_i32 v163, v91, 16, 8                                 // 00000001F810: D54900A3 0221215B
	v_lshrrev_b16 v83, 8, v83                                  // 00000001F818: D7070053 0002A688
	v_lshrrev_b16 v84, 8, v84                                  // 00000001F820: D7070054 0002A888
	v_lshrrev_b16 v170, 8, v85                                 // 00000001F828: D70700AA 0002AA88
	v_mul_i32_i24_sdwa v171, sext(v71), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F830: 1356AAF9 08080647
	v_mul_i32_i24_sdwa v172, sext(v71), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F838: 1358AAF9 0A0A0647
	v_mul_i32_i24_sdwa v85, sext(v71), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F840: 12AAAAF9 0B0B0647
	v_lshrrev_b16 v91, 8, v91                                  // 00000001F848: D707005B 0002B688
	v_lshrrev_b16 v92, 8, v92                                  // 00000001F850: D707005C 0002B888
	s_waitcnt lgkmcnt(2)                                       // 00000001F858: BF8CC27F
	v_mul_i32_i24_sdwa v188, sext(v71), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F85C: 1378BAF9 08080647
	v_mul_i32_i24_sdwa v189, sext(v71), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F864: 137ABAF9 0A0A0647
	v_mul_i32_i24_sdwa v71, sext(v71), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F86C: 128EBAF9 0B0B0647
	v_lshrrev_b16 v93, 8, v93                                  // 00000001F874: D707005D 0002BA88
	v_mul_i32_i24_sdwa v123, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F87C: 12F682F9 08080649
	v_mul_i32_i24_sdwa v124, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F884: 12F882F9 0A0A0649
	v_mul_i32_i24_sdwa v65, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F88C: 128282F9 0B0B0649
	v_lshrrev_b16 v125, 8, v66                                 // 00000001F894: D707007D 00028488
	v_mul_i32_i24_sdwa v126, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F89C: 12FC84F9 0808064A
	v_mul_i32_i24_sdwa v127, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F8A4: 12FE84F9 0A0A064A
	v_mul_i32_i24_sdwa v66, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F8AC: 128484F9 0B0B064A
	v_lshrrev_b16 v128, 8, v67                                 // 00000001F8B4: D7070080 00028688
	v_lshrrev_b16 v135, 8, v74                                 // 00000001F8BC: D7070087 00029488
	v_lshrrev_b16 v136, 8, v75                                 // 00000001F8C4: D7070088 00029688
	v_lshrrev_b16 v144, 8, v78                                 // 00000001F8CC: D7070090 00029C88
	v_mul_i32_i24_sdwa v145, sext(v72), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F8D4: 13229CF9 08080648
	v_mul_i32_i24_sdwa v146, sext(v72), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F8DC: 13249CF9 0A0A0648
	v_mul_i32_i24_sdwa v78, sext(v72), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F8E4: 129C9CF9 0B0B0648
	v_lshrrev_b16 v147, 8, v79                                 // 00000001F8EC: D7070093 00029E88
	v_mul_i32_i24_sdwa v148, sext(v73), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F8F4: 13289EF9 08080649
	v_mul_i32_i24_sdwa v149, sext(v73), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F8FC: 132A9EF9 0A0A0649
	v_mul_i32_i24_sdwa v79, sext(v73), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F904: 129E9EF9 0B0B0649
	v_mul_i32_i24_sdwa v118, sext(v119), sext(v118) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F90C: 12ECECF9 08080677
	v_mul_i32_i24_sdwa v122, sext(v134), sext(v122) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F914: 12F4F4F9 08080686
	v_lshrrev_b16 v173, 8, v86                                 // 00000001F91C: D70700AD 0002AC88
	v_mul_i32_i24_sdwa v174, sext(v72), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F924: 135CACF9 08080648
	v_mul_i32_i24_sdwa v175, sext(v72), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F92C: 135EACF9 0A0A0648
	v_mul_i32_i24_sdwa v86, sext(v72), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F934: 12ACACF9 0B0B0648
	v_lshrrev_b16 v176, 8, v87                                 // 00000001F93C: D70700B0 0002AE88
	v_mul_i32_i24_sdwa v177, sext(v73), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F944: 1362AEF9 08080649
	v_mul_i32_i24_sdwa v178, sext(v73), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F94C: 1364AEF9 0A0A0649
	v_mul_i32_i24_sdwa v87, sext(v73), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F954: 12AEAEF9 0B0B0649
	v_mul_i32_i24_sdwa v190, sext(v72), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F95C: 137CBCF9 08080648
	v_mul_i32_i24_sdwa v191, sext(v72), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F964: 137EBCF9 0A0A0648
	v_mul_i32_i24_sdwa v72, sext(v72), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F96C: 1290BCF9 0B0B0648
	v_lshrrev_b16 v94, 8, v94                                  // 00000001F974: D707005E 0002BC88
	s_waitcnt lgkmcnt(1)                                       // 00000001F97C: BF8CC17F
	v_mul_i32_i24_sdwa v192, sext(v73), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F980: 1380BEF9 08080649
	v_mul_i32_i24_sdwa v193, sext(v73), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001F988: 1382BEF9 0A0A0649
	v_mul_i32_i24_sdwa v73, sext(v73), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001F990: 1292BEF9 0B0B0649
	v_lshrrev_b16 v95, 8, v95                                  // 00000001F998: D707005F 0002BE88
	v_add3_u32 v104, v104, v110, v114                          // 00000001F9A0: D76D0068 05CADD68
	v_add3_u32 v61, v61, v63, v64                              // 00000001F9A8: D76D003D 05027F3D
	v_mad_i32_i24 v59, v69, v59, v60                           // 00000001F9B0: D542003B 04F27745
	v_mad_i32_i24 v60, v69, v153, v154                         // 00000001F9B8: D542003C 066B3345
	v_mad_i32_i24 v63, v69, v155, v156                         // 00000001F9C0: D542003F 06733745
	v_mad_i32_i24 v64, v109, v113, v139                        // 00000001F9C8: D5420040 062EE36D
	v_mad_i32_i24 v110, v108, v112, v140                       // 00000001F9D0: D542006E 0632E16C
	v_mad_i32_i24 v112, v108, v161, v162                       // 00000001F9D8: D5420070 068B436C
	v_mad_i32_i24 v70, v108, v163, v70                         // 00000001F9E0: D5420046 051B476C
	v_mul_i32_i24_sdwa v107, sext(v105), sext(v107) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F9E8: 12D6D6F9 08080669
	v_mul_i32_i24_sdwa v108, sext(v111), sext(v138) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F9F0: 12D914F9 0808066F
	v_mul_i32_i24_sdwa v113, sext(v115), sext(v141) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001F9F8: 12E31AF9 08080673
	v_mul_i32_i24_sdwa v83, sext(v105), sext(v83) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA00: 12A6A6F9 08080669
	v_mul_i32_i24_sdwa v84, sext(v111), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA08: 12A8A8F9 0808066F
	v_mul_i32_i24_sdwa v114, sext(v115), sext(v170) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA10: 12E554F9 08080673
	v_mul_i32_i24_sdwa v91, sext(v105), sext(v91) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA18: 12B6B6F9 08080669
	v_mul_i32_i24_sdwa v92, sext(v111), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA20: 12B8B8F9 0808066F
	v_mul_i32_i24_sdwa v93, sext(v115), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA28: 12BABAF9 08080673
	v_mul_i32_i24_sdwa v129, sext(v75), sext(v67) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA30: 130286F9 0808064B
	v_mul_i32_i24_sdwa v130, sext(v75), sext(v67) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA38: 130486F9 0A0A064B
	v_mul_i32_i24_sdwa v67, sext(v75), sext(v67) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA40: 128686F9 0B0B064B
	v_lshrrev_b16 v131, 8, v68                                 // 00000001FA48: D7070083 00028888
	v_mul_i32_i24_sdwa v132, sext(v76), sext(v68) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA50: 130888F9 0808064C
	v_mul_i32_i24_sdwa v133, sext(v76), sext(v68) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA58: 130A88F9 0A0A064C
	v_mul_i32_i24_sdwa v68, sext(v76), sext(v68) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA60: 128888F9 0B0B064C
	v_lshrrev_b16 v137, 8, v76                                 // 00000001FA68: D7070089 00029888
	v_lshrrev_b16 v150, 8, v80                                 // 00000001FA70: D7070096 0002A088
	v_mul_i32_i24_sdwa v151, sext(v74), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA78: 132EA0F9 0808064A
	v_mul_i32_i24_sdwa v152, sext(v74), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FA80: 1330A0F9 0A0A064A
	v_mul_i32_i24_sdwa v80, sext(v74), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FA88: 12A0A0F9 0B0B064A
	v_lshrrev_b16 v164, 8, v81                                 // 00000001FA90: D70700A4 0002A288
	v_mul_i32_i24_sdwa v165, sext(v75), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FA98: 134AA2F9 0808064B
	v_mul_i32_i24_sdwa v166, sext(v75), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAA0: 134CA2F9 0A0A064B
	v_mul_i32_i24_sdwa v81, sext(v75), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FAA8: 12A2A2F9 0B0B064B
	v_lshrrev_b16 v179, 8, v88                                 // 00000001FAB0: D70700B3 0002B088
	v_mul_i32_i24_sdwa v180, sext(v74), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FAB8: 1368B0F9 0808064A
	v_mul_i32_i24_sdwa v181, sext(v74), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAC0: 136AB0F9 0A0A064A
	v_mul_i32_i24_sdwa v88, sext(v74), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FAC8: 12B0B0F9 0B0B064A
	v_lshrrev_b16 v182, 8, v89                                 // 00000001FAD0: D70700B6 0002B288
	v_mul_i32_i24_sdwa v183, sext(v75), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FAD8: 136EB2F9 0808064B
	v_mul_i32_i24_sdwa v184, sext(v75), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAE0: 1370B2F9 0A0A064B
	v_mul_i32_i24_sdwa v89, sext(v75), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FAE8: 12B2B2F9 0B0B064B
	v_mul_i32_i24_sdwa v194, sext(v74), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FAF0: 1384C0F9 0808064A
	v_mul_i32_i24_sdwa v195, sext(v74), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FAF8: 1386C0F9 0A0A064A
	v_mul_i32_i24_sdwa v74, sext(v74), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FB00: 1294C0F9 0B0B064A
	v_lshrrev_b16 v96, 8, v96                                  // 00000001FB08: D7070060 0002C088
	s_waitcnt lgkmcnt(0)                                       // 00000001FB10: BF8CC07F
	v_mul_i32_i24_sdwa v196, sext(v75), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB14: 1388C2F9 0808064B
	v_mul_i32_i24_sdwa v197, sext(v75), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FB1C: 138AC2F9 0A0A064B
	v_mul_i32_i24_sdwa v75, sext(v75), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FB24: 1296C2F9 0B0B064B
	v_lshrrev_b16 v97, 8, v97                                  // 00000001FB2C: D7070061 0002C288
	v_mul_i32_i24_sdwa v125, sext(v135), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB34: 12FAFAF9 08080687
	v_mul_i32_i24_sdwa v128, sext(v136), sext(v128) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB3C: 130100F9 08080688
	v_add3_u32 v106, v106, v117, v121                          // 00000001FB44: D76D006A 05E6EB6A
	v_add3_u32 v62, v62, v116, v120                            // 00000001FB4C: D76D003E 05E2E93E
	v_mad_i32_i24 v69, v109, v157, v158                        // 00000001FB54: D5420045 067B3B6D
	v_mad_i32_i24 v109, v109, v159, v160                       // 00000001FB5C: D542006D 06833F6D
	v_mul_i32_i24_sdwa v105, sext(v119), sext(v144) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB64: 12D320F9 08080677
	v_mul_i32_i24_sdwa v111, sext(v119), sext(v173) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB6C: 12DF5AF9 08080677
	v_mul_i32_i24_sdwa v94, sext(v119), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB74: 12BCBCF9 08080677
	v_mul_i32_i24_sdwa v115, sext(v134), sext(v147) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB7C: 12E726F9 08080686
	v_mul_i32_i24_sdwa v116, sext(v134), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB84: 12E960F9 08080686
	v_mul_i32_i24_sdwa v95, sext(v134), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FB8C: 12BEBEF9 08080686
	v_add3_u32 v104, v104, v118, v122                          // 00000001FB94: D76D0068 05EAED68
	v_add3_u32 v61, v61, v65, v66                              // 00000001FB9C: D76D003D 050A833D
	v_add3_u32 v66, v107, v108, v113                           // 00000001FBA4: D76D0042 05C6D96B
	v_add3_u32 v59, v59, v77, v78                              // 00000001FBAC: D76D003B 053A9B3B
	v_add3_u32 v77, v83, v84, v114                             // 00000001FBB4: D76D004D 05CAA953
	v_add3_u32 v78, v112, v172, v175                           // 00000001FBBC: D76D004E 06BF5970
	v_add3_u32 v60, v60, v85, v86                              // 00000001FBC4: D76D003C 055AAB3C
	v_add3_u32 v83, v91, v92, v93                              // 00000001FBCC: D76D0053 0576B95B
	v_add3_u32 v63, v63, v71, v72                              // 00000001FBD4: D76D003F 05228F3F
	v_lshrrev_b16 v167, 8, v82                                 // 00000001FBDC: D70700A7 0002A488
	v_mul_i32_i24_sdwa v168, sext(v76), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FBE4: 1350A4F9 0808064C
	v_mul_i32_i24_sdwa v169, sext(v76), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FBEC: 1352A4F9 0A0A064C
	v_mul_i32_i24_sdwa v82, sext(v76), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FBF4: 12A4A4F9 0B0B064C
	v_lshrrev_b16 v185, 8, v90                                 // 00000001FBFC: D70700B9 0002B488
	v_mul_i32_i24_sdwa v186, sext(v76), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC04: 1374B4F9 0808064C
	v_mul_i32_i24_sdwa v187, sext(v76), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FC0C: 1376B4F9 0A0A064C
	v_mul_i32_i24_sdwa v90, sext(v76), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FC14: 12B4B4F9 0B0B064C
	v_mul_i32_i24_sdwa v198, sext(v76), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC1C: 138CC4F9 0808064C
	v_mul_i32_i24_sdwa v199, sext(v76), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000001FC24: 138EC4F9 0A0A064C
	v_mul_i32_i24_sdwa v76, sext(v76), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000001FC2C: 1298C4F9 0B0B064C
	v_lshrrev_b16 v98, 8, v98                                  // 00000001FC34: D7070062 0002C488
	v_mul_i32_i24_sdwa v131, sext(v137), sext(v131) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC3C: 130706F9 08080689
	v_mul_i32_i24_sdwa v117, sext(v135), sext(v150) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC44: 12EB2CF9 08080687
	v_mul_i32_i24_sdwa v119, sext(v135), sext(v179) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC4C: 12EF66F9 08080687
	v_mul_i32_i24_sdwa v96, sext(v135), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC54: 12C0C0F9 08080687
	v_mul_i32_i24_sdwa v120, sext(v136), sext(v164) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC5C: 12F148F9 08080688
	v_mul_i32_i24_sdwa v121, sext(v136), sext(v182) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC64: 12F36CF9 08080688
	v_mul_i32_i24_sdwa v97, sext(v136), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FC6C: 12C2C2F9 08080688
	v_add3_u32 v65, v106, v124, v127                           // 00000001FC74: D76D0041 05FEF96A
	v_add3_u32 v106, v110, v143, v146                          // 00000001FC7C: D76D006A 064B1F6E
	v_add3_u32 v64, v64, v142, v145                            // 00000001FC84: D76D0040 06471D40
	v_add3_u32 v69, v69, v171, v174                            // 00000001FC8C: D76D0045 06BB5745
	v_add3_u32 v70, v70, v189, v191                            // 00000001FC94: D76D0046 06FF7B46
	v_add3_u32 v84, v109, v188, v190                           // 00000001FC9C: D76D0054 06FB796D
	v_add3_u32 v71, v104, v125, v128                           // 00000001FCA4: D76D0047 0602FB68
	v_add3_u32 v61, v61, v67, v68                              // 00000001FCAC: D76D003D 0512873D
	v_add3_u32 v66, v66, v105, v115                            // 00000001FCB4: D76D0042 05CED342
	v_add3_u32 v59, v59, v79, v80                              // 00000001FCBC: D76D003B 05429F3B
	v_add3_u32 v68, v77, v111, v116                            // 00000001FCC4: D76D0044 05D2DF4D
	v_add3_u32 v60, v60, v87, v88                              // 00000001FCCC: D76D003C 0562AF3C
	v_add3_u32 v72, v78, v178, v181                            // 00000001FCD4: D76D0048 06D7654E
	v_add3_u32 v77, v83, v94, v95                              // 00000001FCDC: D76D004D 057EBD53
	v_add3_u32 v63, v63, v73, v74                              // 00000001FCE4: D76D003F 052A933F
	v_mul_i32_i24_sdwa v134, sext(v137), sext(v167) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FCEC: 130D4EF9 08080689
	v_mul_i32_i24_sdwa v135, sext(v137), sext(v185) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FCF4: 130F72F9 08080689
	v_mul_i32_i24_sdwa v98, sext(v137), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FCFC: 12C4C4F9 08080689
	v_add3_u32 v62, v62, v123, v126                            // 00000001FD04: D76D003E 05FAF73E
	v_add3_u32 v64, v64, v148, v151                            // 00000001FD0C: D76D0040 065F2940
	v_add3_u32 v67, v106, v149, v152                           // 00000001FD14: D76D0043 06632B6A
	v_add3_u32 v69, v69, v177, v180                            // 00000001FD1C: D76D0045 06D36345
	v_add3_u32 v73, v84, v192, v194                            // 00000001FD24: D76D0049 070B8154
	v_add3_u32 v70, v70, v193, v195                            // 00000001FD2C: D76D0046 070F8346
	v_add3_u32 v61, v71, v131, v61                             // 00000001FD34: D76D003D 04F70747
	v_add3_u32 v66, v66, v117, v120                            // 00000001FD3C: D76D0042 05E2EB42
	v_add3_u32 v59, v59, v81, v82                              // 00000001FD44: D76D003B 054AA33B
	v_add3_u32 v68, v68, v119, v121                            // 00000001FD4C: D76D0044 05E6EF44
	v_add3_u32 v71, v72, v184, v187                            // 00000001FD54: D76D0047 06EF7148
	v_add3_u32 v60, v60, v89, v90                              // 00000001FD5C: D76D003C 056AB33C
	v_add3_u32 v72, v77, v96, v97                              // 00000001FD64: D76D0048 0586C14D
	v_add3_u32 v63, v63, v75, v76                              // 00000001FD6C: D76D003F 0532973F
	v_add3_u32 v65, v65, v130, v133                            // 00000001FD74: D76D0041 06170541
	v_add3_u32 v62, v62, v129, v132                            // 00000001FD7C: D76D003E 0613033E
	v_add3_u32 v67, v67, v166, v169                            // 00000001FD84: D76D0043 06A74D43
	v_add3_u32 v64, v64, v165, v168                            // 00000001FD8C: D76D0040 06A34B40
	v_add3_u32 v69, v69, v183, v186                            // 00000001FD94: D76D0045 06EB6F45
	v_add3_u32 v70, v70, v197, v199                            // 00000001FD9C: D76D0046 071F8B46
	v_add3_u32 v73, v73, v196, v198                            // 00000001FDA4: D76D0049 071B8949
	v_add3_u32 v59, v66, v134, v59                             // 00000001FDAC: D76D003B 04EF0D42
	v_add3_u32 v60, v68, v135, v60                             // 00000001FDB4: D76D003C 04F30F44
	v_add3_u32 v63, v72, v98, v63                              // 00000001FDBC: D76D003F 04FEC548
	v_add3_u32 v61, v62, v65, v61                              // 00000001FDC4: D76D003D 04F6833E
	v_mul_f32_e32 v66, v102, v99                               // 00000001FDCC: 1084C766
	v_add3_u32 v59, v64, v67, v59                              // 00000001FDD0: D76D003B 04EE8740
	v_add3_u32 v60, v69, v71, v60                              // 00000001FDD8: D76D003C 04F28F45
	v_add3_u32 v62, v73, v70, v63                              // 00000001FDE0: D76D003E 04FE8D49
	v_mul_f32_e32 v63, v103, v99                               // 00000001FDE8: 107EC767
	v_cvt_f32_i32_e32 v61, v61                                 // 00000001FDEC: 7E7A0B3D
	v_cvt_f32_i32_e32 v59, v59                                 // 00000001FDF0: 7E760B3B
	v_cvt_f32_i32_e32 v60, v60                                 // 00000001FDF4: 7E780B3C
	v_cvt_f32_i32_e32 v62, v62                                 // 00000001FDF8: 7E7C0B3E
	v_add_nc_u32_e32 v52, 4, v52                               // 00000001FDFC: 4A686884
	v_fmac_f32_e32 v6, v100, v61                               // 00000001FE00: 560C7B64
	v_fmac_f32_e32 v24, v101, v59                              // 00000001FE04: 56307765
	v_fmac_f32_e32 v23, v66, v60                               // 00000001FE08: 562E7942
	v_fmac_f32_e32 v22, v63, v62                               // 00000001FE0C: 562C7D3F
	s_add_i32 s5, s5, 8                                        // 00000001FE10: 81058805
	s_cmp_lt_u32 s5, 24                                        // 00000001FE14: BF0A9805
	s_cbranch_scc1 64969                                       // 00000001FE18: BF85FDC9 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x840>
	s_add_u32 s8, s8, s6                                       // 00000001FE1C: 80080608
	s_addc_u32 s9, s9, s7                                      // 00000001FE20: 82090709
	s_barrier                                                  // 00000001FE24: BF8A0000
	s_waitcnt_vscnt null, 0x0                                  // 00000001FE28: BBFD0000
	buffer_gl0_inv                                             // 00000001FE2C: E1C40000 00000000
	s_clause 0x1                                               // 00000001FE34: BFA10001
	global_load_dword v59, v46, s[8:9]                         // 00000001FE38: DC308000 3B08002E
	global_load_dword v60, v46, s[8:9] offset:1024             // 00000001FE40: DC308400 3C08002E
	v_mov_b32_e32 v52, v40                                     // 00000001FE48: 7E680328
	v_mov_b32_e32 v53, v44                                     // 00000001FE4C: 7E6A032C
	v_mov_b32_e32 v54, v43                                     // 00000001FE50: 7E6C032B
	v_mov_b32_e32 v55, v42                                     // 00000001FE54: 7E6E032A
	v_mov_b32_e32 v56, v41                                     // 00000001FE58: 7E700329
	v_mov_b32_e32 v57, v50                                     // 00000001FE5C: 7E720332
	v_mov_b32_e32 v58, v18                                     // 00000001FE60: 7E740312
	s_mov_b32 s5, -8                                           // 00000001FE64: BE8503C8
	s_waitcnt vmcnt(0)                                         // 00000001FE68: BF8C3F70
	ds_write2st64_b32 v51, v59, v60 offset1:4                  // 00000001FE6C: D83C0400 003C3B33
	s_waitcnt lgkmcnt(0)                                       // 00000001FE74: BF8CC07F
	s_barrier                                                  // 00000001FE78: BF8A0000
	buffer_gl0_inv                                             // 00000001FE7C: E1C40000 00000000
	v_add_nc_u32_e32 v51, 0, v57                               // 00000001FE84: 4A667280
	v_add_nc_u32_e32 v73, 0, v58                               // 00000001FE88: 4A927480
	v_add_nc_u32_e32 v75, 0, v52                               // 00000001FE8C: 4A966880
	v_add_nc_u32_e32 v76, 0, v53                               // 00000001FE90: 4A986A80
	v_add_nc_u32_e32 v77, 0, v54                               // 00000001FE94: 4A9A6C80
	ds_read2_b32 v[59:60], v51 offset1:1                       // 00000001FE98: D8DC0100 3B000033
	ds_read2_b32 v[61:62], v51 offset0:2 offset1:3             // 00000001FEA0: D8DC0302 3D000033
	ds_read2_b32 v[63:64], v51 offset0:4 offset1:5             // 00000001FEA8: D8DC0504 3F000033
	ds_read2_b32 v[65:66], v51 offset0:6 offset1:7             // 00000001FEB0: D8DC0706 41000033
	ds_read2_b32 v[67:68], v73 offset1:1                       // 00000001FEB8: D8DC0100 43000049
	ds_read2_b32 v[69:70], v73 offset0:2 offset1:3             // 00000001FEC0: D8DC0302 45000049
	ds_read2_b32 v[71:72], v73 offset0:4 offset1:5             // 00000001FEC8: D8DC0504 47000049
	ds_read2_b32 v[73:74], v73 offset0:6 offset1:7             // 00000001FED0: D8DC0706 49000049
	v_add_nc_u32_e32 v78, 0, v55                               // 00000001FED8: 4A9C6E80
	v_add_nc_u32_e32 v79, 0, v56                               // 00000001FEDC: 4A9E7080
	v_add_nc_u32_e32 v81, 0x2000, v51                          // 00000001FEE0: 4AA266FF 00002000
	v_add_nc_u32_e32 v89, 0x4000, v51                          // 00000001FEE8: 4AB266FF 00004000
	v_add_nc_u32_e32 v51, 0x6000, v51                          // 00000001FEF0: 4A6666FF 00006000
	ds_read_b32 v99, v75                                       // 00000001FEF8: D8D80000 6300004B
	ds_read_b32 v100, v76                                      // 00000001FF00: D8D80000 6400004C
	ds_read_b32 v101, v77                                      // 00000001FF08: D8D80000 6500004D
	ds_read_b32 v102, v78                                      // 00000001FF10: D8D80000 6600004E
	ds_read_b32 v103, v79                                      // 00000001FF18: D8D80000 6700004F
	ds_read2_b32 v[75:76], v81 offset0:32 offset1:33           // 00000001FF20: D8DC2120 4B000051
	ds_read2_b32 v[77:78], v81 offset0:34 offset1:35           // 00000001FF28: D8DC2322 4D000051
	ds_read2_b32 v[79:80], v81 offset0:36 offset1:37           // 00000001FF30: D8DC2524 4F000051
	ds_read2_b32 v[81:82], v81 offset0:38 offset1:39           // 00000001FF38: D8DC2726 51000051
	ds_read2_b32 v[83:84], v89 offset0:64 offset1:65           // 00000001FF40: D8DC4140 53000059
	ds_read2_b32 v[85:86], v89 offset0:66 offset1:67           // 00000001FF48: D8DC4342 55000059
	ds_read2_b32 v[87:88], v89 offset0:68 offset1:69           // 00000001FF50: D8DC4544 57000059
	ds_read2_b32 v[89:90], v89 offset0:70 offset1:71           // 00000001FF58: D8DC4746 59000059
	ds_read2_b32 v[91:92], v51 offset0:96 offset1:97           // 00000001FF60: D8DC6160 5B000033
	ds_read2_b32 v[93:94], v51 offset0:98 offset1:99           // 00000001FF68: D8DC6362 5D000033
	ds_read2_b32 v[95:96], v51 offset0:100 offset1:101         // 00000001FF70: D8DC6564 5F000033
	ds_read2_b32 v[97:98], v51 offset0:102 offset1:103         // 00000001FF78: D8DC6766 61000033
	v_add_nc_u32_e32 v58, 32, v58                              // 00000001FF80: 4A7474A0
	v_add_nc_u32_e32 v57, 32, v57                              // 00000001FF84: 4A7272A0
	v_add_nc_u32_e32 v56, 4, v56                               // 00000001FF88: 4A707084
	v_add_nc_u32_e32 v55, 4, v55                               // 00000001FF8C: 4A6E6E84
	s_waitcnt lgkmcnt(15)                                      // 00000001FF90: BF8CCF7F
	v_mul_f32_e32 v100, v100, v99                              // 00000001FF94: 10C8C764
	s_waitcnt lgkmcnt(14)                                      // 00000001FF98: BF8CCE7F
	v_mul_f32_e32 v101, v101, v99                              // 00000001FF9C: 10CAC765
	v_add_nc_u32_e32 v54, 4, v54                               // 00000001FFA0: 4A6C6C84
	v_add_nc_u32_e32 v53, 4, v53                               // 00000001FFA4: 4A6A6A84
	s_waitcnt lgkmcnt(11)                                      // 00000001FFA8: BF8CCB7F
	v_lshrrev_b16 v137, 8, v76                                 // 00000001FFAC: D7070089 00029888
	v_lshrrev_b16 v51, 8, v59                                  // 00000001FFB4: D7070033 00027688
	v_lshrrev_b16 v104, 8, v67                                 // 00000001FFBC: D7070068 00028688
	v_bfe_i32 v105, v59, 16, 8                                 // 00000001FFC4: D5490069 0221213B
	v_bfe_i32 v106, v59, 0, 8                                  // 00000001FFCC: D549006A 0221013B
	v_ashrrev_i32_e32 v59, 24, v59                             // 00000001FFD4: 30767698
	v_bfe_i32 v107, v67, 16, 8                                 // 00000001FFD8: D549006B 02212143
	v_bfe_i32 v108, v67, 0, 8                                  // 00000001FFE0: D549006C 02210143
	v_ashrrev_i32_e32 v67, 24, v67                             // 00000001FFE8: 30868698
	v_lshrrev_b16 v109, 8, v60                                 // 00000001FFEC: D707006D 00027888
	v_lshrrev_b16 v110, 8, v68                                 // 00000001FFF4: D707006E 00028888
	v_mul_i32_i24_sdwa v111, sext(v68), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000001FFFC: 12DE78F9 08080644
	v_mul_i32_i24_sdwa v112, sext(v68), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020004: 12E078F9 0A0A0644
	v_mul_i32_i24_sdwa v60, sext(v68), sext(v60) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002000C: 127878F9 0B0B0644
	v_lshrrev_b16 v113, 8, v61                                 // 000000020014: D7070071 00027A88
	v_lshrrev_b16 v114, 8, v69                                 // 00000002001C: D7070072 00028A88
	v_mul_i32_i24_sdwa v115, sext(v69), sext(v61) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020024: 12E67AF9 08080645
	v_mul_i32_i24_sdwa v116, sext(v69), sext(v61) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002002C: 12E87AF9 0A0A0645
	v_mul_i32_i24_sdwa v61, sext(v69), sext(v61) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020034: 127A7AF9 0B0B0645
	v_lshrrev_b16 v117, 8, v62                                 // 00000002003C: D7070075 00027C88
	v_lshrrev_b16 v118, 8, v70                                 // 000000020044: D7070076 00028C88
	v_mul_i32_i24_sdwa v119, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002004C: 12EE7CF9 08080646
	v_mul_i32_i24_sdwa v120, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020054: 12F07CF9 0A0A0646
	v_mul_i32_i24_sdwa v62, sext(v70), sext(v62) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002005C: 127C7CF9 0B0B0646
	v_lshrrev_b16 v121, 8, v63                                 // 000000020064: D7070079 00027E88
	v_lshrrev_b16 v122, 8, v71                                 // 00000002006C: D707007A 00028E88
	v_mul_i32_i24_sdwa v51, sext(v104), sext(v51) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020074: 126666F9 08080668
	v_mul_i32_i24_sdwa v109, sext(v110), sext(v109) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002007C: 12DADAF9 0808066E
	v_mul_i32_i24_sdwa v113, sext(v114), sext(v113) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020084: 12E2E2F9 08080672
	v_mad_i32_i24 v59, v67, v59, v60                           // 00000002008C: D542003B 04F27743
	v_mad_i32_i24 v60, v108, v106, v111                        // 000000020094: D542003C 05BED56C
	v_mad_i32_i24 v105, v107, v105, v112                       // 00000002009C: D5420069 05C2D36B
	v_lshrrev_b16 v106, 8, v75                                 // 0000000200A4: D707006A 00029688
	v_bfe_i32 v111, v75, 16, 8                                 // 0000000200AC: D549006F 0221214B
	v_bfe_i32 v112, v75, 0, 8                                  // 0000000200B4: D5490070 0221014B
	v_ashrrev_i32_e32 v75, 24, v75                             // 0000000200BC: 30969698
	v_mul_i32_i24_sdwa v138, sext(v68), sext(v76) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000200C0: 131498F9 08080644
	v_mul_i32_i24_sdwa v139, sext(v68), sext(v76) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000200C8: 131698F9 0A0A0644
	v_mul_i32_i24_sdwa v76, sext(v68), sext(v76) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000200D0: 129898F9 0B0B0644
	s_waitcnt lgkmcnt(10)                                      // 0000000200D8: BF8CCA7F
	v_lshrrev_b16 v140, 8, v77                                 // 0000000200DC: D707008C 00029A88
	v_mul_i32_i24_sdwa v141, sext(v69), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000200E4: 131A9AF9 08080645
	v_mul_i32_i24_sdwa v142, sext(v69), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000200EC: 131C9AF9 0A0A0645
	v_mul_i32_i24_sdwa v77, sext(v69), sext(v77) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000200F4: 129A9AF9 0B0B0645
	s_waitcnt lgkmcnt(7)                                       // 0000000200FC: BF8CC77F
	v_lshrrev_b16 v158, 8, v83                                 // 000000020100: D707009E 0002A688
	v_bfe_i32 v159, v83, 16, 8                                 // 000000020108: D549009F 02212153
	v_bfe_i32 v160, v83, 0, 8                                  // 000000020110: D54900A0 02210153
	v_ashrrev_i32_e32 v83, 24, v83                             // 000000020118: 30A6A698
	v_lshrrev_b16 v161, 8, v84                                 // 00000002011C: D70700A1 0002A888
	v_mul_i32_i24_sdwa v162, sext(v68), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020124: 1344A8F9 08080644
	v_mul_i32_i24_sdwa v163, sext(v68), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002012C: 1346A8F9 0A0A0644
	v_mul_i32_i24_sdwa v84, sext(v68), sext(v84) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020134: 12A8A8F9 0B0B0644
	s_waitcnt lgkmcnt(6)                                       // 00000002013C: BF8CC67F
	v_lshrrev_b16 v164, 8, v85                                 // 000000020140: D70700A4 0002AA88
	v_mul_i32_i24_sdwa v165, sext(v69), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020148: 134AAAF9 08080645
	v_mul_i32_i24_sdwa v166, sext(v69), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020150: 134CAAF9 0A0A0645
	v_mul_i32_i24_sdwa v85, sext(v69), sext(v85) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020158: 12AAAAF9 0B0B0645
	s_waitcnt lgkmcnt(3)                                       // 000000020160: BF8CC37F
	v_lshrrev_b16 v182, 8, v91                                 // 000000020164: D70700B6 0002B688
	v_bfe_i32 v183, v91, 16, 8                                 // 00000002016C: D54900B7 0221215B
	v_bfe_i32 v184, v91, 0, 8                                  // 000000020174: D54900B8 0221015B
	v_ashrrev_i32_e32 v91, 24, v91                             // 00000002017C: 30B6B698
	v_lshrrev_b16 v185, 8, v92                                 // 000000020180: D70700B9 0002B888
	v_mul_i32_i24_sdwa v186, sext(v68), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020188: 1374B8F9 08080644
	v_mul_i32_i24_sdwa v187, sext(v68), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020190: 1376B8F9 0A0A0644
	v_mul_i32_i24_sdwa v68, sext(v68), sext(v92) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020198: 1288B8F9 0B0B0644
	s_waitcnt lgkmcnt(2)                                       // 0000000201A0: BF8CC27F
	v_mul_i32_i24_sdwa v92, sext(v69), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000201A4: 12B8BAF9 08080645
	v_mul_i32_i24_sdwa v188, sext(v69), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000201AC: 1378BAF9 0A0A0645
	v_mul_i32_i24_sdwa v69, sext(v69), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000201B4: 128ABAF9 0B0B0645
	v_lshrrev_b16 v93, 8, v93                                  // 0000000201BC: D707005D 0002BA88
	v_mul_i32_i24_sdwa v123, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000201C4: 12F67EF9 08080647
	v_mul_i32_i24_sdwa v124, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000201CC: 12F87EF9 0A0A0647
	v_mul_i32_i24_sdwa v63, sext(v71), sext(v63) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000201D4: 127E7EF9 0B0B0647
	v_lshrrev_b16 v125, 8, v64                                 // 0000000201DC: D707007D 00028088
	v_lshrrev_b16 v126, 8, v72                                 // 0000000201E4: D707007E 00029088
	v_mul_i32_i24_sdwa v127, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000201EC: 12FE80F9 08080648
	v_mul_i32_i24_sdwa v128, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000201F4: 130080F9 0A0A0648
	v_mul_i32_i24_sdwa v64, sext(v72), sext(v64) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000201FC: 128080F9 0B0B0648
	v_lshrrev_b16 v129, 8, v65                                 // 000000020204: D7070081 00028288
	v_lshrrev_b16 v130, 8, v73                                 // 00000002020C: D7070082 00029288
	v_lshrrev_b16 v143, 8, v78                                 // 000000020214: D707008F 00029C88
	v_mul_i32_i24_sdwa v144, sext(v70), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002021C: 13209CF9 08080646
	v_mul_i32_i24_sdwa v145, sext(v70), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020224: 13229CF9 0A0A0646
	v_mul_i32_i24_sdwa v78, sext(v70), sext(v78) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002022C: 129C9CF9 0B0B0646
	v_lshrrev_b16 v146, 8, v79                                 // 000000020234: D7070092 00029E88
	v_mul_i32_i24_sdwa v147, sext(v71), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002023C: 13269EF9 08080647
	v_mul_i32_i24_sdwa v148, sext(v71), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020244: 13289EF9 0A0A0647
	v_mul_i32_i24_sdwa v79, sext(v71), sext(v79) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002024C: 129E9EF9 0B0B0647
	v_lshrrev_b16 v149, 8, v80                                 // 000000020254: D7070095 0002A088
	v_lshrrev_b16 v167, 8, v86                                 // 00000002025C: D70700A7 0002AC88
	v_mul_i32_i24_sdwa v168, sext(v70), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020264: 1350ACF9 08080646
	v_mul_i32_i24_sdwa v169, sext(v70), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002026C: 1352ACF9 0A0A0646
	v_mul_i32_i24_sdwa v86, sext(v70), sext(v86) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020274: 12ACACF9 0B0B0646
	v_lshrrev_b16 v170, 8, v87                                 // 00000002027C: D70700AA 0002AE88
	v_mul_i32_i24_sdwa v171, sext(v71), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020284: 1356AEF9 08080647
	v_mul_i32_i24_sdwa v172, sext(v71), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002028C: 1358AEF9 0A0A0647
	v_mul_i32_i24_sdwa v87, sext(v71), sext(v87) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020294: 12AEAEF9 0B0B0647
	v_mul_i32_i24_sdwa v189, sext(v70), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002029C: 137ABCF9 08080646
	v_mul_i32_i24_sdwa v190, sext(v70), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000202A4: 137CBCF9 0A0A0646
	v_mul_i32_i24_sdwa v70, sext(v70), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000202AC: 128CBCF9 0B0B0646
	v_lshrrev_b16 v94, 8, v94                                  // 0000000202B4: D707005E 0002BC88
	s_waitcnt lgkmcnt(1)                                       // 0000000202BC: BF8CC17F
	v_mul_i32_i24_sdwa v191, sext(v71), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000202C0: 137EBEF9 08080647
	v_mul_i32_i24_sdwa v192, sext(v71), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000202C8: 1380BEF9 0A0A0647
	v_mul_i32_i24_sdwa v71, sext(v71), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000202D0: 128EBEF9 0B0B0647
	v_lshrrev_b16 v95, 8, v95                                  // 0000000202D8: D707005F 0002BE88
	v_mul_i32_i24_sdwa v117, sext(v118), sext(v117) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000202E0: 12EAEAF9 08080676
	v_mul_i32_i24_sdwa v121, sext(v122), sext(v121) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000202E8: 12F2F2F9 0808067A
	v_add3_u32 v51, v51, v109, v113                            // 0000000202F0: D76D0033 05C6DB33
	v_add3_u32 v105, v105, v116, v120                          // 0000000202F8: D76D0069 05E2E969
	v_add3_u32 v60, v60, v115, v119                            // 000000020300: D76D003C 05DEE73C
	v_add3_u32 v59, v59, v61, v62                              // 000000020308: D76D003B 04FA7B3B
	v_mad_i32_i24 v61, v67, v75, v76                           // 000000020310: D542003D 05329743
	v_mad_i32_i24 v75, v107, v111, v139                        // 000000020318: D542004B 062EDF6B
	v_mad_i32_i24 v76, v67, v83, v84                           // 000000020320: D542004C 0552A743
	v_mul_i32_i24_sdwa v83, sext(v104), sext(v106) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020328: 12A6D4F9 08080668
	v_mul_i32_i24_sdwa v84, sext(v110), sext(v137) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020330: 12A912F9 0808066E
	v_mul_i32_i24_sdwa v106, sext(v114), sext(v140) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020338: 12D518F9 08080672
	v_mul_i32_i24_sdwa v116, sext(v104), sext(v158) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020340: 12E93CF9 08080668
	v_mul_i32_i24_sdwa v119, sext(v110), sext(v161) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020348: 12EF42F9 0808066E
	v_mul_i32_i24_sdwa v120, sext(v114), sext(v164) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020350: 12F148F9 08080672
	v_mul_i32_i24_sdwa v104, sext(v104), sext(v182) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020358: 12D16CF9 08080668
	v_mul_i32_i24_sdwa v110, sext(v110), sext(v185) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020360: 12DD72F9 0808066E
	v_mul_i32_i24_sdwa v93, sext(v114), sext(v93) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020368: 12BABAF9 08080672
	v_mad_i32_i24 v67, v67, v91, v68                           // 000000020370: D5420043 0512B743
	v_mul_i32_i24_sdwa v131, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020378: 130682F9 08080649
	v_mul_i32_i24_sdwa v132, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020380: 130882F9 0A0A0649
	v_mul_i32_i24_sdwa v65, sext(v73), sext(v65) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020388: 128282F9 0B0B0649
	v_lshrrev_b16 v133, 8, v66                                 // 000000020390: D7070085 00028488
	v_lshrrev_b16 v134, 8, v74                                 // 000000020398: D7070086 00029488
	v_mul_i32_i24_sdwa v135, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203A0: 130E84F9 0808064A
	v_mul_i32_i24_sdwa v136, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000203A8: 131084F9 0A0A064A
	v_mul_i32_i24_sdwa v66, sext(v74), sext(v66) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000203B0: 128484F9 0B0B064A
	v_mul_i32_i24_sdwa v150, sext(v72), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203B8: 132CA0F9 08080648
	v_mul_i32_i24_sdwa v151, sext(v72), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000203C0: 132EA0F9 0A0A0648
	v_mul_i32_i24_sdwa v80, sext(v72), sext(v80) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000203C8: 12A0A0F9 0B0B0648
	v_lshrrev_b16 v152, 8, v81                                 // 0000000203D0: D7070098 0002A288
	v_mul_i32_i24_sdwa v153, sext(v73), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203D8: 1332A2F9 08080649
	v_mul_i32_i24_sdwa v154, sext(v73), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 0000000203E0: 1334A2F9 0A0A0649
	v_mul_i32_i24_sdwa v81, sext(v73), sext(v81) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 0000000203E8: 12A2A2F9 0B0B0649
	v_lshrrev_b16 v173, 8, v88                                 // 0000000203F0: D70700AD 0002B088
	v_mul_i32_i24_sdwa v174, sext(v72), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000203F8: 135CB0F9 08080648
	v_mul_i32_i24_sdwa v175, sext(v72), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020400: 135EB0F9 0A0A0648
	v_mul_i32_i24_sdwa v88, sext(v72), sext(v88) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020408: 12B0B0F9 0B0B0648
	v_lshrrev_b16 v176, 8, v89                                 // 000000020410: D70700B0 0002B288
	v_mul_i32_i24_sdwa v177, sext(v73), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020418: 1362B2F9 08080649
	v_mul_i32_i24_sdwa v178, sext(v73), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020420: 1364B2F9 0A0A0649
	v_mul_i32_i24_sdwa v89, sext(v73), sext(v89) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020428: 12B2B2F9 0B0B0649
	v_mul_i32_i24_sdwa v193, sext(v72), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020430: 1382C0F9 08080648
	v_mul_i32_i24_sdwa v194, sext(v72), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020438: 1384C0F9 0A0A0648
	v_mul_i32_i24_sdwa v72, sext(v72), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020440: 1290C0F9 0B0B0648
	v_lshrrev_b16 v96, 8, v96                                  // 000000020448: D7070060 0002C088
	s_waitcnt lgkmcnt(0)                                       // 000000020450: BF8CC07F
	v_mul_i32_i24_sdwa v195, sext(v73), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020454: 1386C2F9 08080649
	v_mul_i32_i24_sdwa v196, sext(v73), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002045C: 1388C2F9 0A0A0649
	v_mul_i32_i24_sdwa v73, sext(v73), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020464: 1292C2F9 0B0B0649
	v_lshrrev_b16 v97, 8, v97                                  // 00000002046C: D7070061 0002C288
	v_mul_i32_i24_sdwa v125, sext(v126), sext(v125) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020474: 12FAFAF9 0808067E
	v_mul_i32_i24_sdwa v129, sext(v130), sext(v129) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002047C: 130302F9 08080682
	v_mad_i32_i24 v62, v108, v112, v138                        // 000000020484: D542003E 062AE16C
	v_mul_i32_i24_sdwa v109, sext(v118), sext(v143) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002048C: 12DB1EF9 08080676
	v_mul_i32_i24_sdwa v111, sext(v122), sext(v146) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020494: 12DF24F9 0808067A
	v_mul_i32_i24_sdwa v112, sext(v126), sext(v149) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002049C: 12E12AF9 0808067E
	v_mul_i32_i24_sdwa v137, sext(v118), sext(v167) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204A4: 13134EF9 08080676
	v_mul_i32_i24_sdwa v138, sext(v122), sext(v170) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204AC: 131554F9 0808067A
	v_mad_i32_i24 v146, v108, v160, v162                       // 0000000204B4: D5420092 068B416C
	v_mad_i32_i24 v149, v107, v159, v163                       // 0000000204BC: D5420095 068F3F6B
	v_mul_i32_i24_sdwa v94, sext(v118), sext(v94) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204C4: 12BCBCF9 08080676
	v_mul_i32_i24_sdwa v95, sext(v122), sext(v95) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000204CC: 12BEBEF9 0808067A
	v_mad_i32_i24 v68, v108, v184, v186                        // 0000000204D4: D5420044 06EB716C
	v_mad_i32_i24 v91, v107, v183, v187                        // 0000000204DC: D542005B 06EF6F6B
	v_add3_u32 v51, v51, v117, v121                            // 0000000204E4: D76D0033 05E6EB33
	v_add3_u32 v59, v59, v63, v64                              // 0000000204EC: D76D003B 05027F3B
	v_add3_u32 v64, v83, v84, v106                             // 0000000204F4: D76D0040 05AAA953
	v_add3_u32 v75, v75, v142, v145                            // 0000000204FC: D76D004B 06471D4B
	v_add3_u32 v61, v61, v77, v78                              // 000000020504: D76D003D 053A9B3D
	v_add3_u32 v77, v116, v119, v120                           // 00000002050C: D76D004D 05E2EF74
	v_add3_u32 v76, v76, v85, v86                              // 000000020514: D76D004C 055AAB4C
	v_add3_u32 v84, v104, v110, v93                            // 00000002051C: D76D0054 0576DD68
	v_add3_u32 v67, v67, v69, v70                              // 000000020524: D76D0043 051A8B43
	v_lshrrev_b16 v155, 8, v82                                 // 00000002052C: D707009B 0002A488
	v_mul_i32_i24_sdwa v156, sext(v74), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020534: 1338A4F9 0808064A
	v_mul_i32_i24_sdwa v157, sext(v74), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002053C: 133AA4F9 0A0A064A
	v_mul_i32_i24_sdwa v82, sext(v74), sext(v82) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020544: 12A4A4F9 0B0B064A
	v_lshrrev_b16 v179, 8, v90                                 // 00000002054C: D70700B3 0002B488
	v_mul_i32_i24_sdwa v180, sext(v74), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020554: 1368B4F9 0808064A
	v_mul_i32_i24_sdwa v181, sext(v74), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 00000002055C: 136AB4F9 0A0A064A
	v_mul_i32_i24_sdwa v90, sext(v74), sext(v90) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000020564: 12B4B4F9 0B0B064A
	v_mul_i32_i24_sdwa v197, sext(v74), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002056C: 138AC4F9 0808064A
	v_mul_i32_i24_sdwa v198, sext(v74), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000020574: 138CC4F9 0A0A064A
	v_mul_i32_i24_sdwa v74, sext(v74), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 00000002057C: 1294C4F9 0B0B064A
	v_lshrrev_b16 v98, 8, v98                                  // 000000020584: D7070062 0002C488
	v_mul_i32_i24_sdwa v133, sext(v134), sext(v133) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002058C: 130B0AF9 08080686
	v_mul_i32_i24_sdwa v113, sext(v130), sext(v152) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020594: 12E330F9 08080682
	v_mul_i32_i24_sdwa v139, sext(v126), sext(v173) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002059C: 13175AF9 0808067E
	v_mul_i32_i24_sdwa v140, sext(v130), sext(v176) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000205A4: 131960F9 08080682
	v_mul_i32_i24_sdwa v96, sext(v126), sext(v96) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000205AC: 12C0C0F9 0808067E
	v_mul_i32_i24_sdwa v97, sext(v130), sext(v97) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 0000000205B4: 12C2C2F9 08080682
	v_add3_u32 v62, v62, v141, v144                            // 0000000205BC: D76D003E 06431B3E
	v_add3_u32 v78, v149, v166, v169                           // 0000000205C4: D76D004E 06A74D95
	v_add3_u32 v83, v146, v165, v168                           // 0000000205CC: D76D0053 06A34B92
	v_add3_u32 v85, v91, v188, v190                            // 0000000205D4: D76D0055 06FB795B
	v_add3_u32 v68, v68, v92, v189                             // 0000000205DC: D76D0044 06F6B944
	v_add3_u32 v51, v51, v125, v129                            // 0000000205E4: D76D0033 0606FB33
	v_add3_u32 v59, v59, v65, v66                              // 0000000205EC: D76D003B 050A833B
	v_add3_u32 v64, v64, v109, v111                            // 0000000205F4: D76D0040 05BEDB40
	v_add3_u32 v61, v61, v79, v80                              // 0000000205FC: D76D003D 05429F3D
	v_add3_u32 v65, v75, v148, v151                            // 000000020604: D76D0041 065F294B
	v_add3_u32 v66, v77, v137, v138                            // 00000002060C: D76D0042 062B134D
	v_add3_u32 v69, v76, v87, v88                              // 000000020614: D76D0045 0562AF4C
	v_add3_u32 v76, v84, v94, v95                              // 00000002061C: D76D004C 057EBD54
	v_add3_u32 v67, v67, v71, v72                              // 000000020624: D76D0043 05228F43
	v_mul_i32_i24_sdwa v115, sext(v134), sext(v155) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002062C: 12E736F9 08080686
	v_mul_i32_i24_sdwa v143, sext(v134), sext(v179) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000020634: 131F66F9 08080686
	v_mul_i32_i24_sdwa v98, sext(v134), sext(v98) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 00000002063C: 12C4C4F9 08080686
	v_add3_u32 v60, v60, v123, v127                            // 000000020644: D76D003C 05FEF73C
	v_add3_u32 v63, v105, v124, v128                           // 00000002064C: D76D003F 0602F969
	v_add3_u32 v62, v62, v147, v150                            // 000000020654: D76D003E 065B273E
	v_add3_u32 v70, v83, v171, v174                            // 00000002065C: D76D0046 06BB5753
	v_add3_u32 v75, v78, v172, v175                            // 000000020664: D76D004B 06BF594E
	v_add3_u32 v68, v68, v191, v193                            // 00000002066C: D76D0044 07077F44
	v_add3_u32 v71, v85, v192, v194                            // 000000020674: D76D0047 070B8155
	v_add3_u32 v51, v51, v133, v59                             // 00000002067C: D76D0033 04EF0B33
	v_add3_u32 v59, v64, v112, v113                            // 000000020684: D76D003B 05C6E140
	v_add3_u32 v64, v65, v154, v157                            // 00000002068C: D76D0040 06773541
	v_add3_u32 v61, v61, v81, v82                              // 000000020694: D76D003D 054AA33D
	v_add3_u32 v65, v66, v139, v140                            // 00000002069C: D76D0041 06331742
	v_add3_u32 v69, v69, v89, v90                              // 0000000206A4: D76D0045 056AB345
	v_add3_u32 v72, v76, v96, v97                              // 0000000206AC: D76D0048 0586C14C
	v_add3_u32 v67, v67, v73, v74                              // 0000000206B4: D76D0043 052A9343
	v_add3_u32 v63, v63, v132, v136                            // 0000000206BC: D76D003F 0623093F
	v_add3_u32 v60, v60, v131, v135                            // 0000000206C4: D76D003C 061F073C
	v_add3_u32 v62, v62, v153, v156                            // 0000000206CC: D76D003E 0673333E
	v_add3_u32 v66, v75, v178, v181                            // 0000000206D4: D76D0042 06D7654B
	v_add3_u32 v70, v70, v177, v180                            // 0000000206DC: D76D0046 06D36346
	v_add3_u32 v71, v71, v196, v198                            // 0000000206E4: D76D0047 071B8947
	v_add3_u32 v68, v68, v195, v197                            // 0000000206EC: D76D0044 07178744
	v_add3_u32 v59, v59, v115, v61                             // 0000000206F4: D76D003B 04F6E73B
	v_add3_u32 v61, v65, v143, v69                             // 0000000206FC: D76D003D 05171F41
	v_add3_u32 v65, v72, v98, v67                              // 000000020704: D76D0041 050EC548
	v_add3_u32 v51, v60, v63, v51                              // 00000002070C: D76D0033 04CE7F3C
	v_mul_f32_e32 v67, v102, v99                               // 000000020714: 1086C766
	v_add3_u32 v59, v62, v64, v59                              // 000000020718: D76D003B 04EE813E
	v_add3_u32 v60, v70, v66, v61                              // 000000020720: D76D003C 04F68546
	v_add3_u32 v61, v68, v71, v65                              // 000000020728: D76D003D 05068F44
	v_mul_f32_e32 v62, v103, v99                               // 000000020730: 107CC767
	v_cvt_f32_i32_e32 v51, v51                                 // 000000020734: 7E660B33
	v_cvt_f32_i32_e32 v59, v59                                 // 000000020738: 7E760B3B
	v_cvt_f32_i32_e32 v60, v60                                 // 00000002073C: 7E780B3C
	v_cvt_f32_i32_e32 v61, v61                                 // 000000020740: 7E7A0B3D
	v_add_nc_u32_e32 v52, 4, v52                               // 000000020744: 4A686884
	v_fmac_f32_e32 v6, v100, v51                               // 000000020748: 560C6764
	v_fmac_f32_e32 v24, v101, v59                              // 00000002074C: 56307765
	v_fmac_f32_e32 v23, v67, v60                               // 000000020750: 562E7943
	v_fmac_f32_e32 v22, v62, v61                               // 000000020754: 562C7B3E
	s_add_i32 s5, s5, 8                                        // 000000020758: 81058805
	s_cmp_lt_u32 s5, 24                                        // 00000002075C: BF0A9805
	s_cbranch_scc1 64968                                       // 000000020760: BF85FDC8 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1184>
	s_add_i32 s18, s18, 8                                      // 000000020764: 81128812
	s_cmp_lt_i32 s18, s21                                      // 000000020768: BF041512
	s_barrier                                                  // 00000002076C: BF8A0000
	s_waitcnt_vscnt null, 0x0                                  // 000000020770: BBFD0000
	buffer_gl0_inv                                             // 000000020774: E1C40000 00000000
	s_cbranch_scc1 64085                                       // 00000002077C: BF85FA55 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x3d4>
	s_branch 4                                                 // 000000020780: BF820004 <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1a94>
	v_mov_b32_e32 v22, 0                                       // 000000020784: 7E2C0280
	v_mov_b32_e32 v23, 0                                       // 000000020788: 7E2E0280
	v_mov_b32_e32 v24, 0                                       // 00000002078C: 7E300280
	v_mov_b32_e32 v6, 0                                        // 000000020790: 7E0C0280
	s_not_b32 s1, s14                                          // 000000020794: BE81070E
	s_add_i32 s0, s0, s1                                       // 000000020798: 81000100
	v_cmp_ge_i32_e32 vcc_lo, s0, v1                            // 00000002079C: 7D0C0200
	s_and_saveexec_b32 s0, vcc_lo                              // 0000000207A0: BE803C6A
	s_cbranch_execz 28                                         // 0000000207A4: BF88001C <_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_+0x1b18>
	v_lshl_add_u32 v1, v1, 2, 0                                // 0000000207A8: D7460001 02010501
	s_waitcnt_vscnt null, 0x0                                  // 0000000207B0: BBFD0000
	ds_read_b32 v1, v1                                         // 0000000207B4: D8D80000 01000001
	s_waitcnt lgkmcnt(0)                                       // 0000000207BC: BF8CC07F
	v_mad_u64_u32 v[0:1], s0, v1, s3, v[0:1]                   // 0000000207C0: D5760000 04000701
	s_add_i32 s0, s16, s17                                     // 0000000207C8: 81001110
	s_ashr_i32 s1, s0, 31                                      // 0000000207CC: 91019F00
	s_lshl_b64 s[0:1], s[0:1], 2                               // 0000000207D0: 8F808200
	s_add_u32 s0, s12, s0                                      // 0000000207D4: 8000000C
	v_ashrrev_i32_e32 v1, 31, v0                               // 0000000207D8: 3002009F
	s_addc_u32 s1, s13, s1                                     // 0000000207DC: 8201010D
	v_lshlrev_b64 v[0:1], 2, v[0:1]                            // 0000000207E0: D6FF0000 00020082
	v_add_co_u32 v0, vcc_lo, s0, v0                            // 0000000207E8: D70F6A00 00020000
	v_add_co_ci_u32_e32 v1, vcc_lo, s1, v1, vcc_lo             // 0000000207F0: 50020201
	global_store_dword v[0:1], v6, off                         // 0000000207F4: DC708000 007D0600
	global_store_dword v[0:1], v24, off offset:128             // 0000000207FC: DC708080 007D1800
	global_store_dword v[0:1], v23, off offset:256             // 000000020804: DC708100 007D1700
	global_store_dword v[0:1], v22, off offset:384             // 00000002080C: DC708180 007D1600
	s_endpgm                                                   // 000000020814: BF810000
	s_endpgm                                                   // 000000020818: BF810000
		...

