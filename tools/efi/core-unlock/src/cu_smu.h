/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The eight-core message before the operating system starts: queue 3 message BC250_CPU_MSG_STORE_FF with the
 * argument BC250_CPU_SMN_CORE_ENABLE_MASK, through the shim's own SMU transport (driver/shim/bc250_smu.c) and the
 * shim's own policy (driver/shim/bc250_cpu.c), in the order the Windows driver sends it (driver/kmd/smu.c,
 * driver/kmd/cpu.c). The caller gives register access to the GPU function's register BAR and nothing else, so
 * test/host_test.c runs this file against a model of the firmware.
 */
#ifndef CU_SMU_H
#define CU_SMU_H

/* Access to the register BAR (BC250_BAR5_INDEX) by byte offset. 0 is success. */
struct cu_regs {
	void *context;
	int (*read32)(void *context, unsigned offset, unsigned *value);
	int (*write32)(void *context, unsigned offset, unsigned value);
	void (*stall_us)(void *context, unsigned usec);
};

enum cu_unlock_result {
	CU_UNLOCK_SENT = 0,		/* the firmware answered the store with OK */
	CU_UNLOCK_POLICY = 1,		/* the shim's allowlist or argument check refused: nothing was sent */
	CU_UNLOCK_QUEUE = 2,		/* the transport refused the queue's three registers */
	CU_UNLOCK_HOT = 3,		/* at or above BC250_CLOCK_HOT_MC, or no temperature: the store was not sent */
	CU_UNLOCK_READ_STAGE = 4,	/* the voltage getter did not answer OK: the store was not sent */
	CU_UNLOCK_VOLTAGE = 5,		/* the getter answered a voltage outside the plausible band: not sent */
	CU_UNLOCK_REFUSED = 6,		/* the store was sent and the firmware did not answer OK */
	CU_UNLOCK_COUNT
};

struct cu_unlock_report {
	enum cu_unlock_result result;
	int status;			/* bc250_smu_message_locked's return of the last message, 0 for none */
	unsigned response;		/* the firmware's response word of the last message */
	unsigned millivolts;		/* the getter's answer */
	int temperature_mc;		/* the last temperature read */
	int temperature_valid;
	unsigned messages;		/* how many messages reached the command register */
};

/* One attempt. Sends at most two messages: the getter BC250_CPU_MSG_READ_CPU_MV, which proves that queue 3
 * answers, and then the store. Never sends the store with any argument but BC250_CPU_SMN_CORE_ENABLE_MASK. */
enum cu_unlock_result cu_unlock_send(const struct cu_regs *regs, struct cu_unlock_report *report);

const char *cu_unlock_result_text(enum cu_unlock_result result);

#endif
