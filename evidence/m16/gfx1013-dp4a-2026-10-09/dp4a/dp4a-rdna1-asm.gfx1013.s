
P:/bc-250/scratch/m16-hip/step3/out3/dp4a/dp4a.2.o:	file format elf64-amdgpu

Disassembly of section .text:

0000000000001a00 <_Z14probe_dp4a_onePKiS0_Pi>:
	s_clause 0x1                                               // 000000001A00: BFA10001
	s_load_dwordx4 s[0:3], s[4:5], null                        // 000000001A04: F4080002 FA000000
	s_load_dwordx2 s[6:7], s[4:5], 0x10                        // 000000001A0C: F4040182 FA000010
	v_lshlrev_b32_e32 v0, 2, v0                                // 000000001A14: 34000082
	s_waitcnt lgkmcnt(0)                                       // 000000001A18: BF8CC07F
	s_clause 0x2                                               // 000000001A1C: BFA10002
	global_load_dword v1, v0, s[0:1]                           // 000000001A20: DC308000 01000000
	global_load_dword v2, v0, s[2:3]                           // 000000001A28: DC308000 02020000
	global_load_dword v3, v0, s[6:7]                           // 000000001A30: DC308000 03060000
	s_waitcnt vmcnt(0)                                         // 000000001A38: BF8C3F70
	v_mul_i32_i24_sdwa v4, sext(v1), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001A3C: 120804F9 08080601
	v_mul_i32_i24_sdwa v5, sext(v1), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001A44: 120A04F9 09090601
	v_add3_u32 v3, v4, v5, v3                                  // 000000001A4C: D76D0003 040E0B04
	v_mul_i32_i24_sdwa v4, sext(v1), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001A54: 120804F9 0A0A0601
	v_mul_i32_i24_sdwa v5, sext(v1), sext(v2) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001A5C: 120A04F9 0B0B0601
	v_add3_u32 v3, v4, v5, v3                                  // 000000001A64: D76D0003 040E0B04
	global_store_dword v0, v3, s[6:7]                          // 000000001A6C: DC708000 00060300
	s_endpgm                                                   // 000000001A74: BF810000
	s_nop 0                                                    // 000000001A78: BF800000
	s_nop 0                                                    // 000000001A7C: BF800000
	s_nop 0                                                    // 000000001A80: BF800000
	s_nop 0                                                    // 000000001A84: BF800000
	s_nop 0                                                    // 000000001A88: BF800000
	s_nop 0                                                    // 000000001A8C: BF800000
	s_nop 0                                                    // 000000001A90: BF800000
	s_nop 0                                                    // 000000001A94: BF800000
	s_nop 0                                                    // 000000001A98: BF800000
	s_nop 0                                                    // 000000001A9C: BF800000
	s_nop 0                                                    // 000000001AA0: BF800000
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
	s_clause 0x1                                               // 000000001B00: BFA10001
	s_load_dwordx4 s[0:3], s[4:5], null                        // 000000001B04: F4080002 FA000000
	s_load_dwordx2 s[6:7], s[4:5], 0x10                        // 000000001B0C: F4040182 FA000010
	v_lshlrev_b32_e32 v24, 5, v0                               // 000000001B14: 34300085
	s_waitcnt lgkmcnt(0)                                       // 000000001B18: BF8CC07F
	s_clause 0x5                                               // 000000001B1C: BFA10005
	global_load_dwordx4 v[0:3], v24, s[0:1]                    // 000000001B20: DC388000 00000018
	global_load_dwordx4 v[4:7], v24, s[2:3]                    // 000000001B28: DC388000 04020018
	global_load_dwordx4 v[8:11], v24, s[6:7]                   // 000000001B30: DC388000 08060018
	global_load_dwordx4 v[12:15], v24, s[6:7] offset:16        // 000000001B38: DC388010 0C060018
	global_load_dwordx4 v[16:19], v24, s[0:1] offset:16        // 000000001B40: DC388010 10000018
	global_load_dwordx4 v[20:23], v24, s[2:3] offset:16        // 000000001B48: DC388010 14020018
	s_waitcnt vmcnt(3)                                         // 000000001B50: BF8C3F73
	v_mul_i32_i24_sdwa v25, sext(v0), sext(v4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001B54: 123208F9 08080600
	v_mul_i32_i24_sdwa v26, sext(v0), sext(v4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001B5C: 123408F9 09090600
	v_add3_u32 v8, v25, v26, v8                                // 000000001B64: D76D0008 04223519
	v_mul_i32_i24_sdwa v25, sext(v0), sext(v4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001B6C: 123208F9 0A0A0600
	v_mul_i32_i24_sdwa v26, sext(v0), sext(v4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001B74: 123408F9 0B0B0600
	v_add3_u32 v8, v25, v26, v8                                // 000000001B7C: D76D0008 04223519
	v_mul_i32_i24_sdwa v0, sext(v1), sext(v5) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001B84: 12000AF9 08080601
	v_mul_i32_i24_sdwa v4, sext(v1), sext(v5) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001B8C: 12080AF9 09090601
	v_add3_u32 v9, v0, v4, v9                                  // 000000001B94: D76D0009 04260900
	v_mul_i32_i24_sdwa v0, sext(v1), sext(v5) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001B9C: 12000AF9 0A0A0601
	v_mul_i32_i24_sdwa v4, sext(v1), sext(v5) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001BA4: 12080AF9 0B0B0601
	v_add3_u32 v9, v0, v4, v9                                  // 000000001BAC: D76D0009 04260900
	v_mul_i32_i24_sdwa v0, sext(v2), sext(v6) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001BB4: 12000CF9 08080602
	v_mul_i32_i24_sdwa v1, sext(v2), sext(v6) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001BBC: 12020CF9 09090602
	v_add3_u32 v10, v0, v1, v10                                // 000000001BC4: D76D000A 042A0300
	v_mul_i32_i24_sdwa v0, sext(v2), sext(v6) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001BCC: 12000CF9 0A0A0602
	v_mul_i32_i24_sdwa v1, sext(v2), sext(v6) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001BD4: 12020CF9 0B0B0602
	v_add3_u32 v10, v0, v1, v10                                // 000000001BDC: D76D000A 042A0300
	v_mul_i32_i24_sdwa v0, sext(v3), sext(v7) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001BE4: 12000EF9 08080603
	v_mul_i32_i24_sdwa v1, sext(v3), sext(v7) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001BEC: 12020EF9 09090603
	v_add3_u32 v11, v0, v1, v11                                // 000000001BF4: D76D000B 042E0300
	v_mul_i32_i24_sdwa v0, sext(v3), sext(v7) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001BFC: 12000EF9 0A0A0603
	v_mul_i32_i24_sdwa v1, sext(v3), sext(v7) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C04: 12020EF9 0B0B0603
	v_add3_u32 v11, v0, v1, v11                                // 000000001C0C: D76D000B 042E0300
	s_waitcnt vmcnt(0)                                         // 000000001C14: BF8C3F70
	v_mul_i32_i24_sdwa v0, sext(v16), sext(v20) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001C18: 120028F9 08080610
	v_mul_i32_i24_sdwa v1, sext(v16), sext(v20) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001C20: 120228F9 09090610
	v_add3_u32 v12, v0, v1, v12                                // 000000001C28: D76D000C 04320300
	v_mul_i32_i24_sdwa v0, sext(v16), sext(v20) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C30: 120028F9 0A0A0610
	v_mul_i32_i24_sdwa v1, sext(v16), sext(v20) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C38: 120228F9 0B0B0610
	v_add3_u32 v12, v0, v1, v12                                // 000000001C40: D76D000C 04320300
	v_mul_i32_i24_sdwa v0, sext(v17), sext(v21) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001C48: 12002AF9 08080611
	v_mul_i32_i24_sdwa v1, sext(v17), sext(v21) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001C50: 12022AF9 09090611
	v_add3_u32 v13, v0, v1, v13                                // 000000001C58: D76D000D 04360300
	v_mul_i32_i24_sdwa v0, sext(v17), sext(v21) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C60: 12002AF9 0A0A0611
	v_mul_i32_i24_sdwa v1, sext(v17), sext(v21) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C68: 12022AF9 0B0B0611
	v_add3_u32 v13, v0, v1, v13                                // 000000001C70: D76D000D 04360300
	v_mul_i32_i24_sdwa v0, sext(v18), sext(v22) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001C78: 12002CF9 08080612
	v_mul_i32_i24_sdwa v1, sext(v18), sext(v22) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001C80: 12022CF9 09090612
	v_add3_u32 v14, v0, v1, v14                                // 000000001C88: D76D000E 043A0300
	v_mul_i32_i24_sdwa v0, sext(v18), sext(v22) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001C90: 12002CF9 0A0A0612
	v_mul_i32_i24_sdwa v1, sext(v18), sext(v22) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001C98: 12022CF9 0B0B0612
	v_add3_u32 v14, v0, v1, v14                                // 000000001CA0: D76D000E 043A0300
	v_mul_i32_i24_sdwa v0, sext(v19), sext(v23) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0// 000000001CA8: 12002EF9 08080613
	v_mul_i32_i24_sdwa v1, sext(v19), sext(v23) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1// 000000001CB0: 12022EF9 09090613
	v_add3_u32 v15, v0, v1, v15                                // 000000001CB8: D76D000F 043E0300
	v_mul_i32_i24_sdwa v0, sext(v19), sext(v23) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2// 000000001CC0: 12002EF9 0A0A0613
	v_mul_i32_i24_sdwa v1, sext(v19), sext(v23) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3// 000000001CC8: 12022EF9 0B0B0613
	v_add3_u32 v15, v0, v1, v15                                // 000000001CD0: D76D000F 043E0300
	global_store_dwordx4 v24, v[8:11], s[6:7]                  // 000000001CD8: DC788000 00060818
	global_store_dwordx4 v24, v[12:15], s[6:7] offset:16       // 000000001CE0: DC788010 00060C18
	s_endpgm                                                   // 000000001CE8: BF810000
	s_code_end                                                 // 000000001CEC: BF9F0000
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
