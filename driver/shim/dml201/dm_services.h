/* Platform boundary for AMD DML20. No hardware access or global mutable state. */
#ifndef BC250_DML_SERVICES_H
#define BC250_DML_SERVICES_H
#include <string.h>
#include <stddef.h>
#ifndef __cplusplus
#ifndef bool
#define bool unsigned char
#define true 1
#define false 0
#endif
#endif
struct dal_logger { unsigned int Assertions; unsigned int FirstLine; };
#define ASSERT(x) do { if (!(x)) { ++mode_lib->logger->Assertions; \
    if (!mode_lib->logger->FirstLine) mode_lib->logger->FirstLine = __LINE__; } } while (0)
/* Keep arguments type-checked and marked used, without printing in the KMD. */
static inline void bc250_dml_log(const char *format, ...) { (void)format; }
#define DC_LOG_DML(...) bc250_dml_log(__VA_ARGS__)
#endif
