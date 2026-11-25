/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The eight-core message before the operating system starts (cu_smu.h). This file holds no register offset, no
 * message number and no status code of its own: the three mailbox registers come from bc250_cpu_queue3(), the
 * temperature register from the generated register header, the message and its argument from bc250_cpu.h, and
 * the response codes and the send order from the imported AMD mailbox body behind bc250_smu_message_locked().
 *
 * What it adds is the order of driver/kmd: the shim's allowlist and argument check before any traffic, a
 * temperature read before each message with the KMD's hot gate (a getter passes, a setter needs a reading under
 * BC250_CLOCK_HOT_MC), the getter that proves the queue answers before the first setter (driver/kmd/cpu.c, "the
 * read stage first, always"), and the slow gap of the first queue 3 traffic of a start.
 */
#include <stddef.h>
#include <string.h>
#include "cu_smu.h"
#include "bc250_cpu.h"
#include "bc250_smu.h"
#include "generated/smu_registers.h"
#include "thm_10_0_sh_mask.h"

/* The values driver/kmd/smu.c gives bc250_smu_init_queue for queue 3: a 20 ms poll bound, and "the BIOS owns the
 * image", so the previous response is checked and drained before the first write. */
#define CU_SMU_TIMEOUT_US	20000u
#define CU_SMU_ALREADY_ACTIVE	1

struct cu_smu_context {
	const struct cu_regs *regs;
	struct bc250_cpu_queue queue;
	unsigned long long now_us;
	unsigned messages;
};

/* The three registers of queue 3 and nothing else, the command register for writes only: the rule of
 * MailboxOffsetAllowed in driver/kmd/smu.c, without queue 0, which this application never uses. */
static int offset_allowed(const struct cu_smu_context *c, unsigned offset, int write)
{
	if (offset == c->queue.resp_reg || offset == c->queue.param_reg) return 1;
	return write && offset == c->queue.msg_reg;
}

static int io_owned(void *context)
{
	/* Boot services run one thread, and this application is the only caller of queue 3 in it. */
	(void)context;
	return 1;
}

static int io_read(void *context, unsigned offset, unsigned *value)
{
	struct cu_smu_context *c = context;
	if (!offset_allowed(c, offset, 0)) return -22;
	return c->regs->read32(c->regs->context, offset, value) ? -5 : 0;
}

static int io_write(void *context, unsigned offset, unsigned value)
{
	struct cu_smu_context *c = context;
	if (!offset_allowed(c, offset, 1)) return -22;
	if (c->regs->write32(c->regs->context, offset, value)) return -5;
	if (offset == c->queue.msg_reg) c->messages++;
	return 0;
}

/* Boot services give a stall and no monotonic clock that every firmware has. The clock of the transport is
 * therefore the sum of the stalls it asked for. The transport also bounds each poll by its iteration count
 * (timeout_us iterations of at least a 1 us stall), so a poll lasts at least CU_SMU_TIMEOUT_US and ends. */
static unsigned long long io_now(void *context)
{
	return ((struct cu_smu_context *)context)->now_us;
}

static void io_delay(void *context, unsigned usec)
{
	struct cu_smu_context *c = context;
	c->regs->stall_us(c->regs->context, usec);
	c->now_us += usec;
}

/* The decode of Temperature() in driver/kmd/smu.c, over the same register and the same AMD field names. */
static int read_temperature(const struct cu_smu_context *c, int *mc)
{
	unsigned raw = 0;
	if (c->regs->read32(c->regs->context, BC250_SMU_mmTHM_TCON_CUR_TMP, &raw)) return -1;
	if (raw == 0xFFFFFFFFu) return -5;
	*mc = (int)((raw & (unsigned)THM_TCON_CUR_TMP__CUR_TEMP_MASK) >> THM_TCON_CUR_TMP__CUR_TEMP__SHIFT) * 125;
	if (raw & (unsigned)THM_TCON_CUR_TMP__CUR_TEMP_RANGE_SEL_MASK) *mc -= 49000;
	return 0;
}

static int policy_admits(unsigned message, unsigned parameter, int write)
{
	return bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, message, write) &&
	       bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, message, parameter);
}

/* One message, as CpuMessageGated and CpuDomainMessage in driver/kmd/smu.c send it. Returns 0, the transport's
 * status, or 1 for the hot gate. */
static int cpu_message(struct cu_smu_context *c, struct bc250_smu *smu, unsigned message, unsigned parameter,
		       int write, unsigned *value, struct cu_unlock_report *report)
{
	struct bc250_smu_report transport;
	int degrees = 0, result;
	result = read_temperature(c, &degrees);
	report->temperature_valid = result == 0;
	if (!result) report->temperature_mc = degrees;
	/* A getter changes nothing and passes without a reading. A setter needs one, under the hot limit. */
	if (write && (result || degrees >= BC250_CLOCK_HOT_MC)) return 1;
	if (!policy_admits(message, parameter, write)) return -22;
	result = bc250_smu_message_locked(smu, message, parameter, &transport);
	report->status = result;
	report->response = transport.response;
	if (!result && value) *value = transport.value;
	return result;
}

enum cu_unlock_result cu_unlock_send(const struct cu_regs *regs, struct cu_unlock_report *report)
{
	struct cu_smu_context c;
	struct bc250_smu smu;
	struct bc250_smu_io io;
	unsigned mv = 0;
	int result;

	memset(report, 0, sizeof(*report));
	memset(&c, 0, sizeof(c));
	c.regs = regs;
	/* The policy first, for both messages: a refusal sends nothing at all. bc250_cpu_argument_allowed admits one
	 * argument for the store, the named register, and never 0, which hangs the firmware. */
	if (!policy_admits(BC250_CPU_MSG_READ_CPU_MV, 0u, 0) ||
	    !policy_admits(BC250_CPU_MSG_STORE_FF, BC250_CPU_SMN_CORE_ENABLE_MASK, 1))
		return report->result = CU_UNLOCK_POLICY;

	bc250_cpu_queue3(&c.queue);
	io.context = &c;
	io.owned = io_owned;
	io.read = io_read;
	io.write = io_write;
	io.now_us = io_now;
	io.delay_us = io_delay;
	if (bc250_smu_init_queue(&smu, &io, CU_SMU_TIMEOUT_US, CU_SMU_ALREADY_ACTIVE,
				 c.queue.msg_reg, c.queue.param_reg, c.queue.resp_reg))
		return report->result = CU_UNLOCK_QUEUE;

	/* The read stage: the queue must answer a getter before this start sends a setter. */
	result = cpu_message(&c, &smu, BC250_CPU_MSG_READ_CPU_MV, 0u, 0, &mv, report);
	report->messages = c.messages;
	if (result) return report->result = CU_UNLOCK_READ_STAGE;
	report->millivolts = mv;
	if (mv < BC250_CPU_PLAUSIBLE_MIN_MV || mv > BC250_CPU_PLAUSIBLE_MAX_MV)
		return report->result = CU_UNLOCK_VOLTAGE;

	/* The first queue 3 traffic of a driver start keeps the slow rate (CpuReadStage with BC250_CPU_MESSAGE_GAP_MS). */
	regs->stall_us(regs->context, BC250_CPU_MESSAGE_GAP_MS * 1000u);

	result = cpu_message(&c, &smu, BC250_CPU_MSG_STORE_FF, BC250_CPU_SMN_CORE_ENABLE_MASK, 1, NULL, report);
	report->messages = c.messages;
	if (result == 1) return report->result = CU_UNLOCK_HOT;
	if (result) return report->result = CU_UNLOCK_REFUSED;
	return report->result = CU_UNLOCK_SENT;
}

const char *cu_unlock_result_text(enum cu_unlock_result result)
{
	static const char *const text[CU_UNLOCK_COUNT] = {
		"the firmware accepted the message",
		"the policy refused the message, nothing was sent",
		"the mailbox registers were refused, nothing was sent",
		"the part is too hot or the temperature cannot be read, the message was not sent",
		"queue 3 did not answer the voltage read, the message was not sent",
		"the voltage read is not plausible, the message was not sent",
		"the firmware did not accept the message",
	};
	return (unsigned)result < CU_UNLOCK_COUNT ? text[result] : "unknown result";
}
