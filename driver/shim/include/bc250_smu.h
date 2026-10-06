/* Serialized-owner SMU transport. No mappings or locks are created here. */
#ifndef BC250_SMU_H
#define BC250_SMU_H
struct bc250_smu_io {
    void *context;
    /* Must identify the CALLING owner, not merely whether some thread owns it. */
    int (*owned)(void *context);
    int (*read)(void *context,unsigned int byte_offset,unsigned int *value);
    int (*write)(void *context,unsigned int byte_offset,unsigned int value);
    unsigned long long (*now_us)(void *context);
    void (*delay_us)(void *context,unsigned int usec);
};
struct bc250_smu {
    struct bc250_smu_io io;
    unsigned int timeout_us;
    int firmware_state;
    /* The three mailbox registers of the queue this instance drives, as BAR byte offsets. bc250_smu_init() sets
     * the firmware's queue 0 (C2PMSG_66/82/90), the one this driver has used since 0.7.174; the CPU surface of
     * 0.7.210 drives the firmware's queue 3 through bc250_smu_init_queue() with the offsets of
     * include/bc250_cpu.h. Each queue needs its own instance, because firmware_state above is per queue: the
     * "check and drain the previous response before the first write" path must run once for each. */
    unsigned int msg_reg, param_reg, resp_reg;
};
struct bc250_smu_report {
    unsigned int message,parameter,prior_response,response,value;
    unsigned int reads,writes,polls,delays,message_written;
    int status,io_status;
};
#define BC250_SMU_INVALID (-22)
#define BC250_SMU_NOT_OWNER (-1)
/* Initialize only after exclusive ownership has been established by the
 * lifecycle layer. already_active=1 polls the previous response before the
 * first write; =0 uses AMD's fresh SMU_FW_INIT path and requires separate
 * proof that no prior command is pending. This is not hardware detection. */
int bc250_smu_init(struct bc250_smu *smu,const struct bc250_smu_io *io,
                   unsigned int timeout_us,int already_active);
/* The same, for a queue other than the firmware's queue 0: the caller names the three mailbox registers. The
 * registers must still be ones the caller's read and write callbacks admit; this layer checks nothing about
 * them beyond "all three given and distinct". */
int bc250_smu_init_queue(struct bc250_smu *smu,const struct bc250_smu_io *io,
                         unsigned int timeout_us,int already_active,
                         unsigned int msg_reg,unsigned int param_reg,unsigned int resp_reg);
/* Caller holds the same owner for the entire clock/control transaction. Zero
 * means transport AND firmware response succeeded. Output value valid only then.
 * timeout_us bounds each poll's elapsed interval and number of iterations;
 * actual MMIO callback completion time cannot be bounded by this layer. */
int bc250_smu_message_locked(struct bc250_smu *smu,unsigned int message,
                             unsigned int parameter,struct bc250_smu_report *report);
#endif
