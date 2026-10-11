typedef struct bc250hipCounters {
  unsigned int struct_bytes;
  unsigned long long dispatches;          // dispatch packets built
  unsigned long long submissions;         // calls into the kernel driver that carried work
  unsigned long long batches;             // submissions that carried more than one dispatch
  unsigned long long dispatches_batched;  // dispatches that rode in such a submission
  unsigned long long waits;               // waits for the device
  unsigned long long waits_fast;          // waits the fence mapping answered with no sleep
} bc250hipCounters;
static_assert(sizeof(bc250hipCounters) == 56, "counter ABI");
