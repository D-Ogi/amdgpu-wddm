/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The seed source of OpenSSL in the stage. The UEFI target of OpenSSL is configured with --with-rand-seed=none, and
 * providers/implementations/rands/seeding/rand_unix.c then leaves out the five functions below for OPENSSL_SYS_UEFI.
 * The other platforms of OpenSSL define them in rand_win.c, rand_vms.c and rand_vxworks.c; this file is the same
 * contract (include/crypto/rand.h) for the entropy source of the platform (tpm_engine_platform.entropy).
 *
 * The entropy source gives full entropy (one bit of entropy for each bit), as RDSEED and BCryptGenRandom do: the
 * entropy factor is 1, as in rand_win.c. When the source fails, the pool gets nothing, OpenSSL cannot seed its DRBG,
 * RAND_bytes fails, and libtpms enters failure mode (see rand() in crt_stdlib.c): no key is made from weak data.
 */
#include <stdint.h>
#include <string.h>
#include "crypto/rand.h"
#include "crypto/rand_pool.h"
#include "crt.h"
#include "tpm_engine_internal.h"

size_t ossl_pool_acquire_entropy(RAND_POOL *pool)
{
	size_t needed = ossl_rand_pool_bytes_needed(pool, 1);
	unsigned char *buffer = ossl_rand_pool_add_begin(pool, needed);

	if (buffer && needed) {
		size_t got = tpm_engine_entropy(buffer, needed) ? needed : 0;
		if (!got) memset(buffer, 0, needed);
		ossl_rand_pool_add_end(pool, got, 8 * got);
	}
	return ossl_rand_pool_entropy_available(pool);
}

/* The nonce must be unique, not secret: the time and a count of calls. */
int ossl_pool_add_nonce_data(RAND_POOL *pool)
{
	static uint64_t count;
	struct {
		uint64_t time_ms;
		uint64_t count;
	} data;

	memset(&data, 0, sizeof(data));
	data.time_ms = crt_monotonic_ms();
	data.count = ++count;
	return ossl_rand_pool_add(pool, (unsigned char *)&data, sizeof(data), 0);
}

int ossl_rand_pool_init(void) { return 1; }

void ossl_rand_pool_cleanup(void) {}

void ossl_rand_pool_keep_random_devices_open(int keep) { (void)keep; }
