/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host replay backend for the shim: see backend_trace.c. */
#ifndef BC250_BACKEND_TRACE_H
#define BC250_BACKEND_TRACE_H

#include "amdgpu.h"

struct bc250_reg_write {
	u32 byte_offset;
	u32 value;
};

/* Load one `NAME 0xOFFSET VALUE` sweep log into the register state. First occurrence of an offset
 * wins, across calls as well, so load the logs in the order they were taken. Returns the number of
 * offsets this call contributed, or -1 if the file could not be read. */
int backend_load_sweep(const char *path);

/* The same, but a later file wins over an earlier one. For the Windows-state pass, which overlays
 * unit A's post-PSP state under Windows on top of everything loaded so far. Handles the UTF-16LE
 * the Windows sweeps are written in; backend_load_sweep() does too. */
int backend_load_sweep_replace(const char *path);

/* The recorded writes, oldest first. */
const struct bc250_reg_write *backend_writes(void);
unsigned int backend_write_count(void);
void backend_reset_writes(void);

/* Reads that hit an offset no sweep had a value for. Should stay 0: a non-zero count means the
 * replay was answering with an invented value. */
unsigned int backend_unknown_reads(void);
u32 backend_first_unknown_read(void);

/* Drop every written value, so the next run reads the firmware state again. */
void backend_reset_state(void);

/* Give an offset a value without recording a write.
 *
 * Two callers only, and both are the test speaking for hardware the replay does not have:
 * backend_seed(), which puts unit A's read values in place before a run, and the CP stub in
 * backend_mem.c, which executes the one PM4 packet a ring test submits. Nothing in driver/shim can
 * reach it, so it can never launder a write into the comparison. */
void backend_poke(u32 byte_offset, u32 value);

/* Declare that a write to `alias_offset` also lands on `target_offset`, without being recorded as a
 * second write. One caller, one pair: the GRBM CAM remapping that unit A's firmware has already set
 * up, which gfx_v10_0_check_grbm_cam_remapping() probes for. See replay_gfx.c for the trace lines
 * that show it. Returns 0, or -1 if the table is full. */
int backend_add_alias(u32 alias_offset, u32 target_offset);

/*
 * Declare that writing `trigger_offset` with any of `trigger_mask` set makes the HARDWARE put
 * `target_value` into `target_offset`, but only while `cond_offset` under `cond_mask` reads
 * `cond_value` (cond_mask 0 = unconditional). Applied to the register state, never recorded as a
 * write, so it cannot enter the comparison.
 *
 * One pair is declared, by replay_gfx.c, and it is the same kind of thing as the CP stub: a piece of
 * the GPU the replay does not have.
 *
 *     CP_HQD_DEQUEUE_REQUEST = 1  ->  CP_HQD_ACTIVE = 0,  while CP_MEC_CNTL has no halt bit
 *
 * gfx_v10_0_kiq_init_register() checks whether the HQD is already active and, if it is, asks it to
 * dequeue and polls CP_HQD_ACTIVE until the CP clears it. On a cold boot the register reads 0 and
 * the branch is never taken, which is why the first bring-up in this test never needs it. On a
 * second bring-up it reads 1, because the first one set it.
 *
 * The condition is the whole point and it was missing at first. A dequeue request is serviced by the
 * MEC microengine; teardown halts it (CP_MEC_CNTL = MEC_ME1_HALT | MEC_ME2_HALT) and nothing in the
 * bring-up clears that until gfx_v10_0_kcq_resume(), which runs after the KIQ HQD is programmed
 * (gfx_v10_0.c:7208 called from 7243, one step after kiq_resume at 7239). So on the re-run the
 * driver is polling a halted engine. An unconditional reaction here would answer that poll and hide
 * a failure the hardware will have; with the condition, the replay reproduces it.
 *
 * Returns 0, or -1 if the table is full.
 */
int backend_add_reaction(u32 trigger_offset, u32 trigger_mask,
			 u32 cond_offset, u32 cond_mask, u32 cond_value,
			 u32 target_offset, u32 target_value);

/*
 * Declare that the hardware clears `mask` in `byte_offset` as the write lands, so that the value a
 * later read returns is not the value that was written.
 *
 * This is a third kind of thing from an alias and a reaction and needs its own call, because it
 * must not disturb the comparison: the write is recorded exactly as the driver issued it, and only
 * the state behind subsequent reads is corrected.
 *
 * One bit is declared, by replay_ih.c, and the trace states it outright - the value goes in with
 * bit 31 set and comes back out without it:
 *
 *     0.252838  W  OSSSYS.IH_RB_CNTL  0x04480  C03101A0
 *     0.252845  R  OSSSYS.IH_RB_CNTL  0x04480  403101A0
 *
 * That bit is WPTR_OVERFLOW_CLEAR, and it is a pulse, not a setting (navi10_ih_get_wptr() sets it
 * and clears it again on every overflow). Without this the final read-modify-write of
 * navi10_ih_irq_init() would carry the stale bit forward and produce C03301A1 where unit A produced
 * 403301A1 - a mismatch caused entirely by the model, in a register the driver got right.
 *
 * Returns 0, or -1 if the table is full.
 */
int backend_add_selfclear(u32 byte_offset, u32 mask);

/* Forget every alias, every reaction and every self-clearing bit. */
void backend_clear_aliases(void);

/*
 * Be told about every register write, after the three declarations above have been applied.
 *
 * For a model that cannot be expressed as a register changing, which is what the MEC's own fetch
 * state is: an engine remembering a queue's base and read pointer across a halt. backend_mem.c
 * installs the hook from backend_add_mec_fetch_state() and takes it off again on reset, so this file
 * neither knows nor needs that file - replay.c links it without backend_mem.c at all.
 *
 * NULL is the default and means nobody is listening.
 */
typedef void (*backend_write_hook)(u32 byte_offset, u32 value);
void backend_set_write_hook(backend_write_hook hook);

/*
 * The same, for reads. The hook is asked before the register file and answers by returning 1 and
 * filling in *value; returning 0 leaves the read exactly as it would have been.
 *
 * It exists for one thing this file structurally cannot hold: a register whose value depends on
 * which queue GRBM_GFX_CNTL has selected. There is one value per offset here, so the eight compute
 * queues' CP_HQD_PQ_RPTR would be one register, and a test could not tell "the teardown zeroed all
 * of them" from "the teardown zeroed one of them". backend_mem.c's MEC model already follows the
 * selection, so it answers instead - and only for the queues it has actually seen a ring test run
 * on, so a cold boot reads the seeded sweep value exactly as before.
 */
typedef int (*backend_read_hook)(u32 byte_offset, u32 *value);
void backend_set_read_hook(backend_read_hook hook);

/* Load the read values of a trace extract taken with `--reads`, so that the first read of a register
 * in the replayed window returns what unit A's hardware returned at that moment rather than what the
 * pre-driver sweep saw. Later reads come from the run's own writes, as they do on hardware.
 *
 * Only the FIRST read of each offset is taken, and writes in the extract are ignored: the run under
 * test has to produce those itself. Returns the number of offsets seeded, or -1. */
int backend_seed_reads(const char *trace_path);

/*
 * Every distinct register offset touched between start and stop, reads included.
 *
 * This exists for one question the write log cannot answer: the miniport only lets through registers
 * that are on a generated allow-list, and the list is generated from the trace windows amdgpu's own
 * init produced. The teardown path has no counterpart in any window - unit A was never torn down
 * while the tracer ran - so anything it touches has to be found by running it and looking.
 */
void backend_touched_start(void);
void backend_touched_stop(void);
unsigned int backend_touched_count(void);
unsigned int backend_touched_overflow(void);   /* must be 0, or the table above is too small */
u32 backend_touched_offset(unsigned int i);
int backend_touched_written(unsigned int i);   /* 0 = read only */

/* 0 silences the imports' dev_err/dev_info; 1 prints them. */
void backend_set_verbose(int on);

#endif /* BC250_BACKEND_TRACE_H */
