/* POSIX stand-in for the libtpms build on Windows: RuntimeProfile.c parses the optional runtime profile files with POSIX regular
   expressions. A Windows C library has none. This stand-in compiles a pattern and never matches, which is the
   behaviour of a core with no runtime profile loaded. */
#ifndef BC250_POSIX_REGEX_H
#define BC250_POSIX_REGEX_H
#include <stddef.h>

typedef struct { int unused; } regex_t;
typedef long regoff_t;
typedef struct { regoff_t rm_so; regoff_t rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NOMATCH 1
#define REG_NOERROR 0

static inline int regcomp(regex_t *preg, const char *pattern, int flags)
{
	(void)preg; (void)pattern; (void)flags;
	return 0;
}
static inline int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int flags)
{
	(void)preg; (void)string; (void)nmatch; (void)pmatch; (void)flags;
	return REG_NOMATCH;
}
static inline void regfree(regex_t *preg) { (void)preg; }
static inline size_t regerror(int code, const regex_t *preg, char *buffer, size_t size)
{
	(void)code; (void)preg; (void)buffer; (void)size;
	return 0;
}
#endif
