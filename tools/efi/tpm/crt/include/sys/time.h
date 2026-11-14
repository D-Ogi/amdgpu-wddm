/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <sys/time.h> of the stage C runtime (crt.h): gettimeofday from the monotonic clock (time.h). */
#ifndef CRT_SYS_TIME_H
#define CRT_SYS_TIME_H
#include <time.h>

struct timeval {
	time_t tv_sec;
	long tv_usec;
};

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

int gettimeofday(struct timeval *out, void *zone);

#endif
