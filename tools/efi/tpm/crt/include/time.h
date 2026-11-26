/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <time.h> of the stage C runtime (crt.h). Every clock is the monotonic clock of the platform (crt_init): the stage
 * has no trusted wall time, and the TPM needs only time that does not go back. time() is that clock in seconds. */
#ifndef CRT_TIME_H
#define CRT_TIME_H
#include <stddef.h>

typedef long long time_t;
typedef long clock_t;
typedef int clockid_t;

#define CLOCKS_PER_SEC 1000
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_BOOTTIME 7

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

time_t time(time_t *out);
clock_t clock(void);
int clock_gettime(clockid_t clock_id, struct timespec *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
time_t mktime(struct tm *t);
double difftime(time_t end, time_t start);
int nanosleep(const struct timespec *request, struct timespec *remaining);

#endif
