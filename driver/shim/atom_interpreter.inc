/*
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Stanislaw Skowronek
 */
// Generated checked extract; deviations are in tools/import/extract_atom.py.
static uint32_t atom_arg_mask[8] =
	{ 0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000, 0xFF, 0xFF00, 0xFF0000,
	  0xFF000000 };
static int atom_arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };

static int atom_dst_to_src[8][4] = {
	/* translate destination alignment field to the source alignment encoding */
	{0, 0, 0, 0},
	{1, 2, 3, 0},
	{1, 2, 3, 0},
	{1, 2, 3, 0},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
};
static int atom_def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

static uint32_t atom_get_src_int(atom_exec_context *ctx, uint8_t attr,
				 int *ptr, uint32_t *saved, int print)
{
	uint32_t idx, val = 0xCDCDCDCD, align, arg;
	struct atom_context *gctx = ctx->ctx;
	arg = attr & 7;
	align = (attr >> 3) & 7;
	switch (arg) {
	case ATOM_ARG_REG:
		idx = U16(*ptr);
		(*ptr) += 2;
		if (print)
			DEBUG("REG[0x%04X]", idx);
		idx += gctx->reg_block;
		switch (gctx->io_mode) {
		case ATOM_IO_MM:
			val = gctx->card->reg_read(gctx->card, idx);
			break;
		case ATOM_IO_PCI:
			pr_info("PCI registers are not implemented\n");
			return 0;
		case ATOM_IO_SYSIO:
			pr_info("SYSIO registers are not implemented\n");
			return 0;
		default:
			if (!(gctx->io_mode & 0x80)) {
				pr_info("Bad IO mode\n");
				return 0;
			}
			if (!gctx->iio[gctx->io_mode & 0x7F]) {
				pr_info("Undefined indirect IO read method %d\n",
					gctx->io_mode & 0x7F);
				return 0;
			}
			val =
			    atom_iio_execute(gctx,
					     gctx->iio[gctx->io_mode & 0x7F],
					     idx, 0);
		}
		break;
	case ATOM_ARG_PS:
		idx = U8(*ptr);
		(*ptr)++;
		/* get_unaligned_le32 avoids unaligned accesses from atombios
		 * tables, noticed on a DEC Alpha. */
		if (idx < ctx->ps_size)
			val = get_unaligned_le32((u32 *)&ctx->ps[idx]);
		else
			pr_info("PS index out of range: %i > %i\n", idx, ctx->ps_size);
		if (print)
			DEBUG("PS[0x%02X,0x%04X]", idx, val);
		break;
	case ATOM_ARG_WS:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("WS[0x%02X]", idx);
		switch (idx) {
		case ATOM_WS_QUOTIENT:
			val = gctx->divmul[0];
			break;
		case ATOM_WS_REMAINDER:
			val = gctx->divmul[1];
			break;
		case ATOM_WS_DATAPTR:
			val = gctx->data_block;
			break;
		case ATOM_WS_SHIFT:
			val = gctx->shift;
			break;
		case ATOM_WS_OR_MASK:
			val = UINT32_C(1) << gctx->shift;
			break;
		case ATOM_WS_AND_MASK:
			val = ~(UINT32_C(1) << gctx->shift);
			break;
		case ATOM_WS_FB_WINDOW:
			val = gctx->fb_base;
			break;
		case ATOM_WS_ATTRIBUTES:
			val = gctx->io_attr;
			break;
		case ATOM_WS_REGPTR:
			val = gctx->reg_block;
			break;
		default:
			if (idx < ctx->ws_size)
				val = ctx->ws[idx];
			else
				pr_info("WS index out of range: %i > %i\n", idx, ctx->ws_size);
		}
		break;
	case ATOM_ARG_ID:
		idx = U16(*ptr);
		(*ptr) += 2;
		if (print) {
			if (gctx->data_block)
				DEBUG("ID[0x%04X+%04X]", idx, gctx->data_block);
			else
				DEBUG("ID[0x%04X]", idx);
		}
		val = U32(idx + gctx->data_block);
		break;
	case ATOM_ARG_FB:
		idx = U8(*ptr);
		(*ptr)++;
		if ((gctx->fb_base + (idx * 4)) > gctx->scratch_size_bytes) {
			DRM_ERROR("ATOM: fb read beyond scratch region: %d vs. %d\n",
				  gctx->fb_base + (idx * 4), gctx->scratch_size_bytes);
			val = 0;
		} else
			val = gctx->scratch[(gctx->fb_base / 4) + idx];
		if (print)
			DEBUG("FB[0x%02X]", idx);
		break;
	case ATOM_ARG_IMM:
		switch (align) {
		case ATOM_SRC_DWORD:
			val = U32(*ptr);
			(*ptr) += 4;
			if (print)
				DEBUG("IMM 0x%08X\n", val);
			break;
		case ATOM_SRC_WORD0:
		case ATOM_SRC_WORD8:
		case ATOM_SRC_WORD16:
			val = U16(*ptr);
			(*ptr) += 2;
			if (print)
				DEBUG("IMM 0x%04X\n", val);
			break;
		case ATOM_SRC_BYTE0:
		case ATOM_SRC_BYTE8:
		case ATOM_SRC_BYTE16:
		case ATOM_SRC_BYTE24:
			val = U8(*ptr);
			(*ptr)++;
			if (print)
				DEBUG("IMM 0x%02X\n", val);
			break;
		}
		return val;
	case ATOM_ARG_PLL:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("PLL[0x%02X]", idx);
		val = gctx->card->pll_read(gctx->card, idx);
		break;
	case ATOM_ARG_MC:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("MC[0x%02X]", idx);
		val = gctx->card->mc_read(gctx->card, idx);
		break;
	}
	if (saved)
		*saved = val;
	val &= atom_arg_mask[align];
	val >>= atom_arg_shift[align];
	if (print)
		switch (align) {
		case ATOM_SRC_DWORD:
			DEBUG(".[31:0] -> 0x%08X\n", val);
			break;
		case ATOM_SRC_WORD0:
			DEBUG(".[15:0] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_WORD8:
			DEBUG(".[23:8] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_WORD16:
			DEBUG(".[31:16] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_BYTE0:
			DEBUG(".[7:0] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE8:
			DEBUG(".[15:8] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE16:
			DEBUG(".[23:16] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE24:
			DEBUG(".[31:24] -> 0x%02X\n", val);
			break;
		}
	return val;
}

static void atom_skip_src_int(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
	uint32_t align = (attr >> 3) & 7, arg = attr & 7;
	switch (arg) {
	case ATOM_ARG_REG:
	case ATOM_ARG_ID:
		(*ptr) += 2;
		break;
	case ATOM_ARG_PLL:
	case ATOM_ARG_MC:
	case ATOM_ARG_PS:
	case ATOM_ARG_WS:
	case ATOM_ARG_FB:
		(*ptr)++;
		break;
	case ATOM_ARG_IMM:
		switch (align) {
		case ATOM_SRC_DWORD:
			(*ptr) += 4;
			return;
		case ATOM_SRC_WORD0:
		case ATOM_SRC_WORD8:
		case ATOM_SRC_WORD16:
			(*ptr) += 2;
			return;
		case ATOM_SRC_BYTE0:
		case ATOM_SRC_BYTE8:
		case ATOM_SRC_BYTE16:
		case ATOM_SRC_BYTE24:
			(*ptr)++;
			return;
		}
	}
}

static uint32_t atom_get_src(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
	return atom_get_src_int(ctx, attr, ptr, NULL, 1);
}

static uint32_t atom_get_src_direct(atom_exec_context *ctx, uint8_t align, int *ptr)
{
	uint32_t val = 0xCDCDCDCD;

	switch (align) {
	case ATOM_SRC_DWORD:
		val = U32(*ptr);
		(*ptr) += 4;
		break;
	case ATOM_SRC_WORD0:
	case ATOM_SRC_WORD8:
	case ATOM_SRC_WORD16:
		val = U16(*ptr);
		(*ptr) += 2;
		break;
	case ATOM_SRC_BYTE0:
	case ATOM_SRC_BYTE8:
	case ATOM_SRC_BYTE16:
	case ATOM_SRC_BYTE24:
		val = U8(*ptr);
		(*ptr)++;
		break;
	}
	return val;
}

static uint32_t atom_get_dst(atom_exec_context *ctx, int arg, uint8_t attr,
			     int *ptr, uint32_t *saved, int print)
{
	return atom_get_src_int(ctx,
				arg | atom_dst_to_src[(attr >> 3) &
						      7][(attr >> 6) & 3] << 3,
				ptr, saved, print);
}

static void atom_skip_dst(atom_exec_context *ctx, int arg, uint8_t attr, int *ptr)
{
	atom_skip_src_int(ctx,
			  arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) &
								 3] << 3, ptr);
}

static void atom_put_dst(atom_exec_context *ctx, int arg, uint8_t attr,
			 int *ptr, uint32_t val, uint32_t saved)
{
	uint32_t align =
	    atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], old_val =
	    val, idx;
	struct atom_context *gctx = ctx->ctx;
	if (gctx->status != BC250_ATOM_OK) return;
	old_val &= atom_arg_mask[align] >> atom_arg_shift[align];
	val <<= atom_arg_shift[align];
	val &= atom_arg_mask[align];
	saved &= ~atom_arg_mask[align];
	val |= saved;
	switch (arg) {
	case ATOM_ARG_REG:
		idx = U16(*ptr);
		(*ptr) += 2;
		DEBUG("REG[0x%04X]", idx);
		idx += gctx->reg_block;
		switch (gctx->io_mode) {
		case ATOM_IO_MM:
			if (idx == 0 && val > UINT32_MAX / 4u) { atom_fail(gctx, BC250_ATOM_IO); return; }
			if (idx == 0)
				gctx->card->reg_write(gctx->card, idx,
						      val << 2);
			else
				gctx->card->reg_write(gctx->card, idx, val);
			break;
		case ATOM_IO_PCI:
			pr_info("PCI registers are not implemented\n");
			return;
		case ATOM_IO_SYSIO:
			pr_info("SYSIO registers are not implemented\n");
			return;
		default:
			if (!(gctx->io_mode & 0x80)) {
				pr_info("Bad IO mode\n");
				return;
			}
			if (!gctx->iio[gctx->io_mode & 0xFF]) {
				pr_info("Undefined indirect IO write method %d\n",
					gctx->io_mode & 0x7F);
				return;
			}
			atom_iio_execute(gctx, gctx->iio[gctx->io_mode & 0xFF],
					 idx, val);
		}
		break;
	case ATOM_ARG_PS:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("PS[0x%02X]", idx);
		if (idx >= ctx->ps_size) {
			pr_info("PS index out of range: %i > %i\n", idx, ctx->ps_size);
			return;
		}
		ctx->ps[idx] = cpu_to_le32(val);
		break;
	case ATOM_ARG_WS:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("WS[0x%02X]", idx);
		switch (idx) {
		case ATOM_WS_QUOTIENT:
			gctx->divmul[0] = val;
			break;
		case ATOM_WS_REMAINDER:
			gctx->divmul[1] = val;
			break;
		case ATOM_WS_DATAPTR:
			gctx->data_block = val;
			break;
		case ATOM_WS_SHIFT:
			if (val >= 32) { atom_fail(gctx, BC250_ATOM_PARAMETER); return; }
			gctx->shift = (uint8_t)val;
			break;
		case ATOM_WS_OR_MASK:
		case ATOM_WS_AND_MASK:
			break;
		case ATOM_WS_FB_WINDOW:
			gctx->fb_base = val;
			break;
		case ATOM_WS_ATTRIBUTES:
			gctx->io_attr = val;
			break;
		case ATOM_WS_REGPTR:
			gctx->reg_block = val;
			break;
		default:
			if (idx >= ctx->ws_size) {
				pr_info("WS index out of range: %i > %i\n", idx, ctx->ws_size);
				return;
			}
			ctx->ws[idx] = val;
		}
		break;
	case ATOM_ARG_FB:
		idx = U8(*ptr);
		(*ptr)++;
		if ((gctx->fb_base + (idx * 4)) > gctx->scratch_size_bytes) {
			DRM_ERROR("ATOM: fb write beyond scratch region: %d vs. %d\n",
				  gctx->fb_base + (idx * 4), gctx->scratch_size_bytes);
		} else
			gctx->scratch[(gctx->fb_base / 4) + idx] = val;
		DEBUG("FB[0x%02X]", idx);
		break;
	case ATOM_ARG_PLL:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("PLL[0x%02X]", idx);
		gctx->card->pll_write(gctx->card, idx, val);
		break;
	case ATOM_ARG_MC:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("MC[0x%02X]", idx);
		gctx->card->mc_write(gctx->card, idx, val);
		return;
	}
	switch (align) {
	case ATOM_SRC_DWORD:
		DEBUG(".[31:0] <- 0x%08X\n", old_val);
		break;
	case ATOM_SRC_WORD0:
		DEBUG(".[15:0] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_WORD8:
		DEBUG(".[23:8] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_WORD16:
		DEBUG(".[31:16] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_BYTE0:
		DEBUG(".[7:0] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE8:
		DEBUG(".[15:8] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE16:
		DEBUG(".[23:16] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE24:
		DEBUG(".[31:24] <- 0x%02X\n", old_val);
		break;
	}
}

static void atom_op_add(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst += src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_and(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst &= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_beep(atom_exec_context *ctx, int *ptr, int arg)
{
	printk("ATOM BIOS beeped!\n");
}

static void atom_op_calltable(atom_exec_context *ctx, int *ptr, int arg)
{
	int idx = U8((*ptr)++);
	int r = 0;

	if (idx < ATOM_TABLE_NAMES_CNT)
		SDEBUG("   table: %d (%s)\n", idx, atom_table_names[idx]);
	else
		SDEBUG("   table: %d\n", idx);
	if (U16(ctx->ctx->cmd_table + 4 + 2 * idx))
		r = amdgpu_atom_execute_table_locked(ctx->ctx, idx, ctx->ps + ctx->ps_shift, ctx->ps_size - ctx->ps_shift);
	if (r) {
		ctx->abort = true;
	}
}

static void atom_op_clear(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t saved;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, 0, saved);
}

static void atom_op_compare(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->cs_equal = (dst == src);
	ctx->ctx->cs_above = (dst > src);
	SDEBUG("   result: %s %s\n", ctx->ctx->cs_equal ? "EQ" : "NE",
	       ctx->ctx->cs_above ? "GT" : "LE");
}

static void atom_op_delay(atom_exec_context *ctx, int *ptr, int arg)
{
	unsigned count = U8((*ptr)++);
	SDEBUG("   count: %d\n", count);
	if (arg == ATOM_UNIT_MICROSEC)
		udelay(count);
	else if (!drm_can_sleep())
		mdelay(count);
	else
		msleep(count);
}

static void atom_op_div(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	if (src != 0) {
		ctx->ctx->divmul[0] = dst / src;
		ctx->ctx->divmul[1] = dst % src;
	} else {
		ctx->ctx->divmul[0] = 0;
		ctx->ctx->divmul[1] = 0;
	}
}

static void atom_op_div32(atom_exec_context *ctx, int *ptr, int arg)
{
	uint64_t val64;
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	if (src != 0) {
		val64 = dst;
		val64 |= ((uint64_t)ctx->ctx->divmul[1]) << 32;
		do_div(val64, src);
		ctx->ctx->divmul[0] = lower_32_bits(val64);
		ctx->ctx->divmul[1] = upper_32_bits(val64);
	} else {
		ctx->ctx->divmul[0] = 0;
		ctx->ctx->divmul[1] = 0;
	}
}

static void atom_op_eot(atom_exec_context *ctx, int *ptr, int arg)
{
	/* functionally, a nop */
}

static void atom_op_jump(atom_exec_context *ctx, int *ptr, int arg)
{
	int execute = 0, target = U16(*ptr);
	unsigned long cjiffies;

	(*ptr) += 2;
	switch (arg) {
	case ATOM_COND_ABOVE:
		execute = ctx->ctx->cs_above;
		break;
	case ATOM_COND_ABOVEOREQUAL:
		execute = ctx->ctx->cs_above || ctx->ctx->cs_equal;
		break;
	case ATOM_COND_ALWAYS:
		execute = 1;
		break;
	case ATOM_COND_BELOW:
		execute = !(ctx->ctx->cs_above || ctx->ctx->cs_equal);
		break;
	case ATOM_COND_BELOWOREQUAL:
		execute = !ctx->ctx->cs_above;
		break;
	case ATOM_COND_EQUAL:
		execute = ctx->ctx->cs_equal;
		break;
	case ATOM_COND_NOTEQUAL:
		execute = !ctx->ctx->cs_equal;
		break;
	}
	if (arg != ATOM_COND_ALWAYS)
		SDEBUG("   taken: %s\n", str_yes_no(execute));
	SDEBUG("   target: 0x%04X\n", target);
	if (execute) {
		if (ctx->last_jump == (ctx->start + target)) {
			cjiffies = jiffies;
			if (time_after(cjiffies, ctx->last_jump_jiffies)) {
				cjiffies -= ctx->last_jump_jiffies;
				if ((jiffies_to_msecs(cjiffies) > ATOM_CMD_TIMEOUT_SEC*1000)) {
					DRM_ERROR("atombios stuck in loop for more than %dsecs aborting\n",
						  ATOM_CMD_TIMEOUT_SEC);
					ctx->abort = true;
				}
			} else {
				/* jiffies wrap around we will just wait a little longer */
				ctx->last_jump_jiffies = jiffies;
			}
		} else {
			ctx->last_jump = ctx->start + target;
			ctx->last_jump_jiffies = jiffies;
		}
		*ptr = ctx->start + target;
	}
}

static void atom_op_mask(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, mask, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	mask = atom_get_src_direct(ctx, ((attr >> 3) & 7), ptr);
	SDEBUG("   mask: 0x%08x", mask);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst &= mask;
	dst |= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_move(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t src, saved;
	int dptr = *ptr;
	if (((attr >> 3) & 7) != ATOM_SRC_DWORD)
		atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
	else {
		atom_skip_dst(ctx, arg, attr, ptr);
		saved = 0xCDCDCDCD;
	}
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, src, saved);
}

static void atom_op_mul(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->divmul[0] = dst * src;
}

static void atom_op_mul32(atom_exec_context *ctx, int *ptr, int arg)
{
	uint64_t val64;
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	val64 = (uint64_t)dst * (uint64_t)src;
	ctx->ctx->divmul[0] = lower_32_bits(val64);
	ctx->ctx->divmul[1] = upper_32_bits(val64);
}

static void atom_op_nop(atom_exec_context *ctx, int *ptr, int arg)
{
	/* nothing */
}

static void atom_op_or(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst |= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_postcard(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t val = U8((*ptr)++);
	SDEBUG("POST card output: 0x%02X\n", val);
}

static void atom_op_repeat(atom_exec_context *ctx, int *ptr, int arg)
{
	pr_info("unimplemented!\n");
}

static void atom_op_restorereg(atom_exec_context *ctx, int *ptr, int arg)
{
	pr_info("unimplemented!\n");
}

static void atom_op_savereg(atom_exec_context *ctx, int *ptr, int arg)
{
	pr_info("unimplemented!\n");
}

static void atom_op_setdatablock(atom_exec_context *ctx, int *ptr, int arg)
{
	int idx = U8(*ptr);
	(*ptr)++;
	SDEBUG("   block: %d\n", idx);
	if (!idx)
		ctx->ctx->data_block = 0;
	else if (idx == 255)
		ctx->ctx->data_block = ctx->start;
	else
		ctx->ctx->data_block = U16(ctx->ctx->data_table + 4 + 2 * idx);
	SDEBUG("   base: 0x%04X\n", ctx->ctx->data_block);
}

static void atom_op_setfbbase(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	SDEBUG("   fb_base: ");
	ctx->ctx->fb_base = atom_get_src(ctx, attr, ptr);
}

static void atom_op_setport(atom_exec_context *ctx, int *ptr, int arg)
{
	int port;
	switch (arg) {
	case ATOM_PORT_ATI:
		port = U16(*ptr);
		if (port < ATOM_IO_NAMES_CNT)
			SDEBUG("   port: %d (%s)\n", port, atom_io_names[port]);
		else
			SDEBUG("   port: %d\n", port);
		if (!port)
			ctx->ctx->io_mode = ATOM_IO_MM;
		else
			ctx->ctx->io_mode = ATOM_IO_IIO | port;
		(*ptr) += 2;
		break;
	case ATOM_PORT_PCI:
		ctx->ctx->io_mode = ATOM_IO_PCI;
		(*ptr)++;
		break;
	case ATOM_PORT_SYSIO:
		ctx->ctx->io_mode = ATOM_IO_SYSIO;
		(*ptr)++;
		break;
	}
}

static void atom_op_setregblock(atom_exec_context *ctx, int *ptr, int arg)
{
	ctx->ctx->reg_block = U16(*ptr);
	(*ptr) += 2;
	SDEBUG("   base: 0x%04X\n", ctx->ctx->reg_block);
}

static void atom_op_shift_left(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	shift = atom_shift(ctx->ctx, atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr));
	SDEBUG("   shift: %d\n", shift);
	dst <<= shift;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shift_right(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	shift = atom_shift(ctx->ctx, atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr));
	SDEBUG("   shift: %d\n", shift);
	dst >>= shift;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shl(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	/* op needs to full dst value */
	dst = saved;
	shift = atom_shift(ctx->ctx, atom_get_src(ctx, attr, ptr));
	SDEBUG("   shift: %d\n", shift);
	dst <<= shift;
	dst &= atom_arg_mask[dst_align];
	dst >>= atom_arg_shift[dst_align];
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shr(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	/* op needs to full dst value */
	dst = saved;
	shift = atom_shift(ctx->ctx, atom_get_src(ctx, attr, ptr));
	SDEBUG("   shift: %d\n", shift);
	dst >>= shift;
	dst &= atom_arg_mask[dst_align];
	dst >>= atom_arg_shift[dst_align];
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_sub(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst -= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_switch(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t src, val, target;
	SDEBUG("   switch: ");
	src = atom_get_src(ctx, attr, ptr);
	while (U16(*ptr) != ATOM_CASE_END)
		if (U8(*ptr) == ATOM_CASE_MAGIC) {
			(*ptr)++;
			SDEBUG("   case: ");
			val =
			    atom_get_src(ctx, (attr & 0x38) | ATOM_ARG_IMM,
					 ptr);
			target = U16(*ptr);
			if (val == src) {
				SDEBUG("   target: %04X\n", target);
				*ptr = ctx->start + target;
				return;
			}
			(*ptr) += 2;
		} else {
			pr_info("Bad case\n");
			return;
		}
	(*ptr) += 2;
}

static void atom_op_test(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->cs_equal = ((dst & src) == 0);
	SDEBUG("   result: %s\n", ctx->ctx->cs_equal ? "EQ" : "NE");
}

static void atom_op_xor(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst ^= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_debug(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t val = U8((*ptr)++);
	SDEBUG("DEBUG output: 0x%02X\n", val);
}

static void atom_op_processds(atom_exec_context *ctx, int *ptr, int arg)
{
	uint16_t val = U16(*ptr);
	(*ptr) += val + 2;
	SDEBUG("PROCESSDS output: 0x%02X\n", val);
}

static struct {
	void (*func) (atom_exec_context *, int *, int);
	int arg;
} opcode_table[ATOM_OP_CNT] = {
	{
	NULL, 0}, {
	atom_op_move, ATOM_ARG_REG}, {
	atom_op_move, ATOM_ARG_PS}, {
	atom_op_move, ATOM_ARG_WS}, {
	atom_op_move, ATOM_ARG_FB}, {
	atom_op_move, ATOM_ARG_PLL}, {
	atom_op_move, ATOM_ARG_MC}, {
	atom_op_and, ATOM_ARG_REG}, {
	atom_op_and, ATOM_ARG_PS}, {
	atom_op_and, ATOM_ARG_WS}, {
	atom_op_and, ATOM_ARG_FB}, {
	atom_op_and, ATOM_ARG_PLL}, {
	atom_op_and, ATOM_ARG_MC}, {
	atom_op_or, ATOM_ARG_REG}, {
	atom_op_or, ATOM_ARG_PS}, {
	atom_op_or, ATOM_ARG_WS}, {
	atom_op_or, ATOM_ARG_FB}, {
	atom_op_or, ATOM_ARG_PLL}, {
	atom_op_or, ATOM_ARG_MC}, {
	atom_op_shift_left, ATOM_ARG_REG}, {
	atom_op_shift_left, ATOM_ARG_PS}, {
	atom_op_shift_left, ATOM_ARG_WS}, {
	atom_op_shift_left, ATOM_ARG_FB}, {
	atom_op_shift_left, ATOM_ARG_PLL}, {
	atom_op_shift_left, ATOM_ARG_MC}, {
	atom_op_shift_right, ATOM_ARG_REG}, {
	atom_op_shift_right, ATOM_ARG_PS}, {
	atom_op_shift_right, ATOM_ARG_WS}, {
	atom_op_shift_right, ATOM_ARG_FB}, {
	atom_op_shift_right, ATOM_ARG_PLL}, {
	atom_op_shift_right, ATOM_ARG_MC}, {
	atom_op_mul, ATOM_ARG_REG}, {
	atom_op_mul, ATOM_ARG_PS}, {
	atom_op_mul, ATOM_ARG_WS}, {
	atom_op_mul, ATOM_ARG_FB}, {
	atom_op_mul, ATOM_ARG_PLL}, {
	atom_op_mul, ATOM_ARG_MC}, {
	atom_op_div, ATOM_ARG_REG}, {
	atom_op_div, ATOM_ARG_PS}, {
	atom_op_div, ATOM_ARG_WS}, {
	atom_op_div, ATOM_ARG_FB}, {
	atom_op_div, ATOM_ARG_PLL}, {
	atom_op_div, ATOM_ARG_MC}, {
	atom_op_add, ATOM_ARG_REG}, {
	atom_op_add, ATOM_ARG_PS}, {
	atom_op_add, ATOM_ARG_WS}, {
	atom_op_add, ATOM_ARG_FB}, {
	atom_op_add, ATOM_ARG_PLL}, {
	atom_op_add, ATOM_ARG_MC}, {
	atom_op_sub, ATOM_ARG_REG}, {
	atom_op_sub, ATOM_ARG_PS}, {
	atom_op_sub, ATOM_ARG_WS}, {
	atom_op_sub, ATOM_ARG_FB}, {
	atom_op_sub, ATOM_ARG_PLL}, {
	atom_op_sub, ATOM_ARG_MC}, {
	atom_op_setport, ATOM_PORT_ATI}, {
	atom_op_setport, ATOM_PORT_PCI}, {
	atom_op_setport, ATOM_PORT_SYSIO}, {
	atom_op_setregblock, 0}, {
	atom_op_setfbbase, 0}, {
	atom_op_compare, ATOM_ARG_REG}, {
	atom_op_compare, ATOM_ARG_PS}, {
	atom_op_compare, ATOM_ARG_WS}, {
	atom_op_compare, ATOM_ARG_FB}, {
	atom_op_compare, ATOM_ARG_PLL}, {
	atom_op_compare, ATOM_ARG_MC}, {
	atom_op_switch, 0}, {
	atom_op_jump, ATOM_COND_ALWAYS}, {
	atom_op_jump, ATOM_COND_EQUAL}, {
	atom_op_jump, ATOM_COND_BELOW}, {
	atom_op_jump, ATOM_COND_ABOVE}, {
	atom_op_jump, ATOM_COND_BELOWOREQUAL}, {
	atom_op_jump, ATOM_COND_ABOVEOREQUAL}, {
	atom_op_jump, ATOM_COND_NOTEQUAL}, {
	atom_op_test, ATOM_ARG_REG}, {
	atom_op_test, ATOM_ARG_PS}, {
	atom_op_test, ATOM_ARG_WS}, {
	atom_op_test, ATOM_ARG_FB}, {
	atom_op_test, ATOM_ARG_PLL}, {
	atom_op_test, ATOM_ARG_MC}, {
	atom_op_delay, ATOM_UNIT_MILLISEC}, {
	atom_op_delay, ATOM_UNIT_MICROSEC}, {
	atom_op_calltable, 0}, {
	atom_op_repeat, 0}, {
	atom_op_clear, ATOM_ARG_REG}, {
	atom_op_clear, ATOM_ARG_PS}, {
	atom_op_clear, ATOM_ARG_WS}, {
	atom_op_clear, ATOM_ARG_FB}, {
	atom_op_clear, ATOM_ARG_PLL}, {
	atom_op_clear, ATOM_ARG_MC}, {
	atom_op_nop, 0}, {
	atom_op_eot, 0}, {
	atom_op_mask, ATOM_ARG_REG}, {
	atom_op_mask, ATOM_ARG_PS}, {
	atom_op_mask, ATOM_ARG_WS}, {
	atom_op_mask, ATOM_ARG_FB}, {
	atom_op_mask, ATOM_ARG_PLL}, {
	atom_op_mask, ATOM_ARG_MC}, {
	atom_op_postcard, 0}, {
	atom_op_beep, 0}, {
	atom_op_savereg, 0}, {
	atom_op_restorereg, 0}, {
	atom_op_setdatablock, 0}, {
	atom_op_xor, ATOM_ARG_REG}, {
	atom_op_xor, ATOM_ARG_PS}, {
	atom_op_xor, ATOM_ARG_WS}, {
	atom_op_xor, ATOM_ARG_FB}, {
	atom_op_xor, ATOM_ARG_PLL}, {
	atom_op_xor, ATOM_ARG_MC}, {
	atom_op_shl, ATOM_ARG_REG}, {
	atom_op_shl, ATOM_ARG_PS}, {
	atom_op_shl, ATOM_ARG_WS}, {
	atom_op_shl, ATOM_ARG_FB}, {
	atom_op_shl, ATOM_ARG_PLL}, {
	atom_op_shl, ATOM_ARG_MC}, {
	atom_op_shr, ATOM_ARG_REG}, {
	atom_op_shr, ATOM_ARG_PS}, {
	atom_op_shr, ATOM_ARG_WS}, {
	atom_op_shr, ATOM_ARG_FB}, {
	atom_op_shr, ATOM_ARG_PLL}, {
	atom_op_shr, ATOM_ARG_MC}, {
	atom_op_debug, 0}, {
	atom_op_processds, 0}, {
	atom_op_mul32, ATOM_ARG_PS}, {
	atom_op_mul32, ATOM_ARG_WS}, {
	atom_op_div32, ATOM_ARG_PS}, {
	atom_op_div32, ATOM_ARG_WS},
};

