
P:/bc-250/scratch/m16-hip/step3/out3/dp4a/dp4a.1.o:	file format elf64-amdgpu

Disassembly of section .text:

0000000000001a00 <_Z14probe_dp4a_onePKiS0_Pi>:
	s_load_dwordx4 s[0:3], s[4:5], null                        // 000000001A00: F4080002 FA000000
	v_lshlrev_b32_e32 v0, 2, v0                                // 000000001A08: 34000082
	s_load_dwordx2 s[4:5], s[4:5], 0x10                        // 000000001A0C: F4040102 FA000010
	s_waitcnt lgkmcnt(0)                                       // 000000001A14: BF8CC07F
	s_clause 0x2                                               // 000000001A18: BFA10002
	global_load_dword v1, v0, s[0:1]                           // 000000001A1C: DC308000 01000000
	global_load_dword v2, v0, s[2:3]                           // 000000001A24: DC308000 02020000
	global_load_dword v3, v0, s[4:5]                           // 000000001A2C: DC308000 03040000
	s_waitcnt vmcnt(2)                                         // 000000001A34: BF8C3F72
	v_lshrrev_b16 v4, 8, v1                                    // 000000001A38: D7070004 00020288
	s_waitcnt vmcnt(1)                                         // 000000001A40: BF8C3F71
	v_lshrrev_b16 v5, 8, v2                                    // 000000001A44: D7070005 00020488
	v_bfe_i32 v6, v1, 0, 8                                     // 000000001A4C: D5490006 02210101
	v_bfe_i32 v7, v2, 0, 8                                     // 000000001A54: D5490007 02210102
	v_mul_i32_i24_sdwa v8, sext(v2), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001A5C: 121002F9 0A0A0602
	v_bfe_i32 v4, v4, 0, 8                                     // 000000001A64: D5490004 02210104
	v_bfe_i32 v5, v5, 0, 8                                     // 000000001A6C: D5490005 02210105
	v_mul_i32_i24_sdwa v1, sext(v2), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001A74: 120202F9 0B0B0602
	v_mad_i32_i24 v2, v7, v6, v8                               // 000000001A7C: D5420002 04220D07
	v_mad_i32_i24 v1, v5, v4, v1                               // 000000001A84: D5420001 04060905
	s_waitcnt vmcnt(0)                                         // 000000001A8C: BF8C3F70
	v_add3_u32 v1, v2, v1, v3                                  // 000000001A90: D76D0001 040E0302
	global_store_dword v0, v1, s[4:5]                          // 000000001A98: DC708000 00040100
	s_endpgm                                                   // 000000001AA0: BF810000
	s_nop 0                                                    // 000000001AA4: BF800000
	s_nop 0                                                    // 000000001AA8: BF800000
	s_nop 0                                                    // 000000001AAC: BF800000
	s_nop 0                                                    // 000000001AB0: BF800000
	s_nop 0                                                    // 000000001AB4: BF800000
	s_nop 0                                                    // 000000001AB8: BF800000
	s_nop 0                                                    // 000000001ABC: BF800000
	s_nop 0                                                    // 000000001AC0: BF800000
	s_nop 0                                                    // 000000001AC4: BF800000
	s_nop 0                                                    // 000000001AC8: BF800000
	s_nop 0                                                    // 000000001ACC: BF800000
	s_nop 0                                                    // 000000001AD0: BF800000
	s_nop 0                                                    // 000000001AD4: BF800000
	s_nop 0                                                    // 000000001AD8: BF800000
	s_nop 0                                                    // 000000001ADC: BF800000
	s_nop 0                                                    // 000000001AE0: BF800000
	s_nop 0                                                    // 000000001AE4: BF800000
	s_nop 0                                                    // 000000001AE8: BF800000
	s_nop 0                                                    // 000000001AEC: BF800000
	s_nop 0                                                    // 000000001AF0: BF800000
	s_nop 0                                                    // 000000001AF4: BF800000
	s_nop 0                                                    // 000000001AF8: BF800000
	s_nop 0                                                    // 000000001AFC: BF800000

0000000000001b00 <_Z16probe_dp4a_eightPKiS0_Pi>:
	s_load_dwordx4 s[0:3], s[4:5], null                        // 000000001B00: F4080002 FA000000
	v_lshlrev_b32_e32 v20, 5, v0                               // 000000001B08: 34280085
	s_load_dwordx2 s[4:5], s[4:5], 0x10                        // 000000001B0C: F4040102 FA000010
	s_waitcnt lgkmcnt(0)                                       // 000000001B14: BF8CC07F
	s_clause 0x4                                               // 000000001B18: BFA10004
	global_load_dwordx4 v[0:3], v20, s[0:1]                    // 000000001B1C: DC388000 00000014
	global_load_dwordx4 v[4:7], v20, s[2:3]                    // 000000001B24: DC388000 04020014
	global_load_dwordx4 v[8:11], v20, s[0:1] offset:16         // 000000001B2C: DC388010 08000014
	global_load_dwordx4 v[12:15], v20, s[2:3] offset:16        // 000000001B34: DC388010 0C020014
	global_load_dwordx4 v[16:19], v20, s[4:5]                  // 000000001B3C: DC388000 10040014
	s_waitcnt vmcnt(3)                                         // 000000001B44: BF8C3F73
	v_mul_i32_i24_sdwa v21, sext(v4), sext(v0) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001B48: 122A00F9 08080604
	v_mul_i32_i24_sdwa v22, sext(v4), sext(v0) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001B50: 122C00F9 09090604
	v_mul_i32_i24_sdwa v23, sext(v4), sext(v0) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001B58: 122E00F9 0A0A0604
	v_mul_i32_i24_sdwa v4, sext(v4), sext(v0) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001B60: 120800F9 0B0B0604
	v_mul_i32_i24_sdwa v24, sext(v5), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001B68: 123002F9 08080605
	v_mul_i32_i24_sdwa v25, sext(v5), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001B70: 123202F9 09090605
	v_mul_i32_i24_sdwa v26, sext(v5), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001B78: 123402F9 0A0A0605
	v_mul_i32_i24_sdwa v5, sext(v5), sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001B80: 120A02F9 0B0B0605
	v_mul_i32_i24_sdwa v27, sext(v6), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001B88: 123604F9 08080606
	v_mul_i32_i24_sdwa v28, sext(v6), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001B90: 123804F9 09090606
	v_mul_i32_i24_sdwa v29, sext(v6), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001B98: 123A04F9 0A0A0606
	v_mul_i32_i24_sdwa v6, sext(v6), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001BA0: 120C04F9 0B0B0606
	v_mul_i32_i24_sdwa v30, sext(v7), sext(v3) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001BA8: 123C06F9 08080607
	v_mul_i32_i24_sdwa v31, sext(v7), sext(v3) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001BB0: 123E06F9 09090607
	v_mul_i32_i24_sdwa v32, sext(v7), sext(v3) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001BB8: 124006F9 0A0A0607
	v_mul_i32_i24_sdwa v7, sext(v7), sext(v3) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001BC0: 120E06F9 0B0B0607
	global_load_dwordx4 v[0:3], v20, s[4:5] offset:16          // 000000001BC8: DC388010 00040014
	s_waitcnt vmcnt(2)                                         // 000000001BD0: BF8C3F72
	v_mul_i32_i24_sdwa v33, sext(v12), sext(v8) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001BD4: 124210F9 0808060C
	v_mul_i32_i24_sdwa v34, sext(v12), sext(v8) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001BDC: 124410F9 0909060C
	v_mul_i32_i24_sdwa v35, sext(v12), sext(v8) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001BE4: 124610F9 0A0A060C
	v_mul_i32_i24_sdwa v8, sext(v12), sext(v8) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001BEC: 121010F9 0B0B060C
	v_mul_i32_i24_sdwa v12, sext(v13), sext(v9) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001BF4: 121812F9 0808060D
	v_mul_i32_i24_sdwa v36, sext(v13), sext(v9) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001BFC: 124812F9 0909060D
	v_mul_i32_i24_sdwa v37, sext(v13), sext(v9) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C04: 124A12F9 0A0A060D
	v_mul_i32_i24_sdwa v9, sext(v13), sext(v9) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C0C: 121212F9 0B0B060D
	v_mul_i32_i24_sdwa v13, sext(v14), sext(v10) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001C14: 121A14F9 0808060E
	v_mul_i32_i24_sdwa v38, sext(v14), sext(v10) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001C1C: 124C14F9 0909060E
	v_mul_i32_i24_sdwa v39, sext(v14), sext(v10) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C24: 124E14F9 0A0A060E
	v_mul_i32_i24_sdwa v10, sext(v14), sext(v10) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C2C: 121414F9 0B0B060E
	v_mul_i32_i24_sdwa v14, sext(v15), sext(v11) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001C34: 121C16F9 0808060F
	v_mul_i32_i24_sdwa v40, sext(v15), sext(v11) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001C3C: 125016F9 0909060F
	v_mul_i32_i24_sdwa v41, sext(v15), sext(v11) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C44: 125216F9 0A0A060F
	v_mul_i32_i24_sdwa v11, sext(v15), sext(v11) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C4C: 121616F9 0B0B060F
	s_waitcnt vmcnt(1)                                         // 000000001C54: BF8C3F71
	v_add3_u32 v15, v22, v16, v21                              // 000000001C58: D76D000F 04562116
	v_add3_u32 v16, v25, v17, v24                              // 000000001C60: D76D0010 04622319
	v_add3_u32 v17, v28, v18, v27                              // 000000001C68: D76D0011 046E251C
	v_add3_u32 v18, v31, v19, v30                              // 000000001C70: D76D0012 047A271F
	s_waitcnt vmcnt(0)                                         // 000000001C78: BF8C3F70
	v_add3_u32 v19, v34, v0, v33                               // 000000001C7C: D76D0013 04860122
	v_add3_u32 v12, v36, v1, v12                               // 000000001C84: D76D000C 04320324
	v_add3_u32 v13, v38, v2, v13                               // 000000001C8C: D76D000D 04360526
	v_add3_u32 v14, v40, v3, v14                               // 000000001C94: D76D000E 043A0728
	v_add3_u32 v0, v15, v23, v4                                // 000000001C9C: D76D0000 04122F0F
	v_add3_u32 v1, v16, v26, v5                                // 000000001CA4: D76D0001 04163510
	v_add3_u32 v2, v17, v29, v6                                // 000000001CAC: D76D0002 041A3B11
	v_add3_u32 v3, v18, v32, v7                                // 000000001CB4: D76D0003 041E4112
	v_add3_u32 v4, v19, v35, v8                                // 000000001CBC: D76D0004 04224713
	v_add3_u32 v5, v12, v37, v9                                // 000000001CC4: D76D0005 04264B0C
	v_add3_u32 v6, v13, v39, v10                               // 000000001CCC: D76D0006 042A4F0D
	v_add3_u32 v7, v14, v41, v11                               // 000000001CD4: D76D0007 042E530E
	global_store_dwordx4 v20, v[0:3], s[4:5]                   // 000000001CDC: DC788000 00040014
	global_store_dwordx4 v20, v[4:7], s[4:5] offset:16         // 000000001CE4: DC788010 00040414
	s_endpgm                                                   // 000000001CEC: BF810000
	s_code_end                                                 // 000000001CF0: BF9F0000
	s_code_end                                                 // 000000001CF4: BF9F0000
	s_code_end                                                 // 000000001CF8: BF9F0000
	s_code_end                                                 // 000000001CFC: BF9F0000
	s_code_end                                                 // 000000001D00: BF9F0000
	s_code_end                                                 // 000000001D04: BF9F0000
	s_code_end                                                 // 000000001D08: BF9F0000
	s_code_end                                                 // 000000001D0C: BF9F0000
	s_code_end                                                 // 000000001D10: BF9F0000
	s_code_end                                                 // 000000001D14: BF9F0000
	s_code_end                                                 // 000000001D18: BF9F0000
	s_code_end                                                 // 000000001D1C: BF9F0000
	s_code_end                                                 // 000000001D20: BF9F0000
	s_code_end                                                 // 000000001D24: BF9F0000
	s_code_end                                                 // 000000001D28: BF9F0000
	s_code_end                                                 // 000000001D2C: BF9F0000
	s_code_end                                                 // 000000001D30: BF9F0000
	s_code_end                                                 // 000000001D34: BF9F0000
	s_code_end                                                 // 000000001D38: BF9F0000
	s_code_end                                                 // 000000001D3C: BF9F0000
	s_code_end                                                 // 000000001D40: BF9F0000
	s_code_end                                                 // 000000001D44: BF9F0000
	s_code_end                                                 // 000000001D48: BF9F0000
	s_code_end                                                 // 000000001D4C: BF9F0000
	s_code_end                                                 // 000000001D50: BF9F0000
	s_code_end                                                 // 000000001D54: BF9F0000
	s_code_end                                                 // 000000001D58: BF9F0000
	s_code_end                                                 // 000000001D5C: BF9F0000
	s_code_end                                                 // 000000001D60: BF9F0000
	s_code_end                                                 // 000000001D64: BF9F0000
	s_code_end                                                 // 000000001D68: BF9F0000
	s_code_end                                                 // 000000001D6C: BF9F0000
	s_code_end                                                 // 000000001D70: BF9F0000
	s_code_end                                                 // 000000001D74: BF9F0000
	s_code_end                                                 // 000000001D78: BF9F0000
	s_code_end                                                 // 000000001D7C: BF9F0000
	s_code_end                                                 // 000000001D80: BF9F0000
	s_code_end                                                 // 000000001D84: BF9F0000
	s_code_end                                                 // 000000001D88: BF9F0000
	s_code_end                                                 // 000000001D8C: BF9F0000
	s_code_end                                                 // 000000001D90: BF9F0000
	s_code_end                                                 // 000000001D94: BF9F0000
	s_code_end                                                 // 000000001D98: BF9F0000
	s_code_end                                                 // 000000001D9C: BF9F0000
	s_code_end                                                 // 000000001DA0: BF9F0000
	s_code_end                                                 // 000000001DA4: BF9F0000
	s_code_end                                                 // 000000001DA8: BF9F0000
	s_code_end                                                 // 000000001DAC: BF9F0000
	s_code_end                                                 // 000000001DB0: BF9F0000
	s_code_end                                                 // 000000001DB4: BF9F0000
	s_code_end                                                 // 000000001DB8: BF9F0000
	s_code_end                                                 // 000000001DBC: BF9F0000
