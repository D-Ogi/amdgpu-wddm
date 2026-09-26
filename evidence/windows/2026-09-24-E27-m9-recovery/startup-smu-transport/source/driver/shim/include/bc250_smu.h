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
};
struct bc250_smu_report {
    unsigned int message,parameter,prior_response,response,value;
    unsigned int reads,writes,polls,delays,message_written;
    int status,io_status;
};
#define BC250_SMU_INVALID (-22)
#define BC250_SMU_NOT_OWNER (-1)
/* Initialize only after exclusive ownership and previous-command drain have
 * been established by the lifecycle layer. already_active=1 checks the prior
 * response even on the first call; =0 uses AMD's fresh SMU_FW_INIT path.
 * This argument is a lifecycle assertion, not hardware detection. */
int bc250_smu_init(struct bc250_smu *smu,const struct bc250_smu_io *io,
                   unsigned int timeout_us,int already_active);
/* Caller holds the same owner for the entire clock/control transaction. Zero
 * means transport AND firmware response succeeded. Output value valid only then.
 * timeout_us bounds each poll's elapsed interval and number of iterations;
 * actual MMIO callback completion time cannot be bounded by this layer. */
int bc250_smu_message_locked(struct bc250_smu *smu,unsigned int message,
                             unsigned int parameter,struct bc250_smu_report *report);
#endif
