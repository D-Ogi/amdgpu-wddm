/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * <time.h> and <sys/time.h> of the stage C runtime. Every clock is the monotonic clock of crt_init, in
 * milliseconds, counted from an origin that the platform chooses. The stage has no wall time that it can trust, and
 * the TPM clock needs only time that does not go back: libtpms keeps the TPM time continuous across a handoff with
 * its own offset in the volatile state.
 */
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "crt.h"

time_t time(time_t *out)
{
	time_t t = (time_t)(crt_monotonic_ms() / 1000);
	if (out) *out = t;
	return t;
}

clock_t clock(void) { return (clock_t)crt_monotonic_ms(); }

int clock_gettime(clockid_t clock_id, struct timespec *out)
{
	unsigned long long ms = crt_monotonic_ms();
	(void)clock_id;
	out->tv_sec = (time_t)(ms / 1000);
	out->tv_nsec = (long)(ms % 1000) * 1000000L;
	return 0;
}

int gettimeofday(struct timeval *out, void *zone)
{
	unsigned long long ms = crt_monotonic_ms();
	(void)zone;
	out->tv_sec = (time_t)(ms / 1000);
	out->tv_usec = (long)(ms % 1000) * 1000L;
	return 0;
}

/* Days since 1970-01-01 to a civil date (the proleptic Gregorian calendar), by eras of 400 years. */
static void civil_from_days(long long z, int *year, int *month, int *day)
{
	long long era, yoe, doy, mp, y;
	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	yoe = z - era * 146097;
	yoe = (yoe - yoe / 1460 + yoe / 36524 - yoe / 146096) / 365;
	y = yoe + era * 400;
	doy = z - era * 146097 - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*day = (int)(doy - (153 * mp + 2) / 5 + 1);
	*month = (int)(mp < 10 ? mp + 3 : mp - 9);
	*year = (int)(y + (*month <= 2));
}

static long long days_from_civil(long long y, int m, int d)
{
	long long era, yoe, doy, doe;
	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
	static const short before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	long long seconds = *t, days = seconds / 86400, rest = seconds % 86400;
	int year, month, day;

	if (rest < 0) {
		rest += 86400;
		days--;
	}
	civil_from_days(days, &year, &month, &day);
	out->tm_sec = (int)(rest % 60);
	out->tm_min = (int)(rest / 60 % 60);
	out->tm_hour = (int)(rest / 3600);
	out->tm_mday = day;
	out->tm_mon = month - 1;
	out->tm_year = year - 1900;
	out->tm_wday = (int)((days % 7 + 11) % 7);	/* 1970-01-01 was a Thursday */
	out->tm_yday = before[month - 1] + day - 1 + (month > 2 && is_leap(year));
	out->tm_isdst = 0;
	return out;
}

/* One static result: the runtime has no threads. */
struct tm *gmtime(const time_t *t)
{
	static struct tm result;
	return gmtime_r(t, &result);
}

/* The stage has no time zone. */
struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *t)
{
	long long year = 1900LL + t->tm_year + t->tm_mon / 12;
	int month = t->tm_mon % 12;
	time_t result;
	if (month < 0) {
		month += 12;
		year--;
	}
	result = (time_t)(days_from_civil(year, month + 1, 1) + t->tm_mday - 1) * 86400 + (time_t)t->tm_hour * 3600 +
		 (time_t)t->tm_min * 60 + t->tm_sec;
	gmtime_r(&result, t);
	return result;
}

double difftime(time_t end, time_t start) { return (double)(end - start); }

/* Busy waits: the stage has no scheduler. */
int nanosleep(const struct timespec *request, struct timespec *remaining)
{
	unsigned long long until = crt_monotonic_ms() + (unsigned long long)request->tv_sec * 1000 +
				   (unsigned long long)request->tv_nsec / 1000000;
	while (crt_monotonic_ms() < until) __asm__ volatile("pause");
	if (remaining) {
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

unsigned sleep(unsigned seconds)
{
	struct timespec t = {seconds, 0};
	nanosleep(&t, NULL);
	return 0;
}

int usleep(unsigned long long microseconds)
{
	struct timespec t = {(time_t)(microseconds / 1000000), (long)(microseconds % 1000000) * 1000};
	nanosleep(&t, NULL);
	return 0;
}
