/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Deterministic unit test for the rados-nkvx off-reactor Exec executor (ADR-0009
 * / TB1). It exercises the executor in isolation — no librados, no OSD, no Ceph —
 * because the executor itself has no RADOS dependency: it consumes already-filled
 * object bytes and runs a built-in module off the SPDK reactor.
 *
 * It proves the three TB1 acceptance criteria deterministically:
 *   1. A dispatched Exec returns the correct trivial result computed from the
 *      object bytes (bytecount -> object length as LE u64; identity -> bytes back).
 *   2. The module body ran OFF the polled SPDK thread: the test captures the OS
 *      thread id of the SPDK thread driving completions and asserts the worker's
 *      run_tid (captured inside the module run) differs from it. This is a
 *      thread-id check, NOT a timing race.
 *   3. The dispatch is selected by the static built-in module name (the same name
 *      the "nkvx:<module>" op-ID binding resolves to in the live path).
 *
 * The executor's worker is a real pthread; it hands completions back to the SPDK
 * thread via spdk_thread_send_msg(), which ut_multithread.c's poll_threads()
 * harvests on the main OS thread. So the worker run_tid is genuinely a different
 * OS thread from the one delivering the completion.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"

#include "spdk/thread.h"

/* Pull the executor in as source: it depends only on spdk/thread + pthreads. */
#include "kvdev/rados/kvdev_rados_nkvx.c"
/* And the dlopen-backed wasm runtime, so the real-wasm path is exercised in the
 * same translation unit. With --without-wasm this is the NOT_SUPPORTED stub. */
#include "kvdev/rados/kvdev_rados_nkvx_wasm.c"

#include "common/lib/ut_multithread.c"

struct nkvx_result {
	bool		completed;
	int		kvstatus;
	uint32_t	out_len;
	pthread_t	complete_tid;	/* OS thread the completion fired on */
};

static void
nkvx_done(void *arg, int kvstatus, uint32_t out_len)
{
	struct nkvx_result *r = arg;

	r->kvstatus = kvstatus;
	r->out_len = out_len;
	r->complete_tid = pthread_self();
	r->completed = true;
}

/* Dispatch a job from the (single) SPDK thread and pump until it completes. */
static void
dispatch_and_wait(const char *module, const void *obj, size_t obj_len,
		  void *out, uint32_t out_cap, struct nkvx_result *r)
{
	int rc;

	memset(r, 0, sizeof(*r));

	set_thread(0);
	rc = kvdev_rados_nkvx_dispatch(module, obj, obj_len, out, out_cap,
				       nkvx_done, r);
	CU_ASSERT(rc == 0);
	set_thread(INVALID_THREAD);

	/* Pump the SPDK thread until the worker hands the completion back. A
	 * bounded spin avoids hanging the suite if the worker were broken. */
	for (int i = 0; i < 100000 && !r->completed; i++) {
		poll_threads();
		if (!r->completed) {
			usleep(100);
		}
	}
	CU_ASSERT(r->completed);
}

static void
test_nkvx_bytecount_offreactor(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj); /* includes NUL, matches host */
	uint8_t out[64];
	uint64_t got = 0;
	struct nkvx_result r;
	pthread_t main_tid = pthread_self();

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, obj, obj_len,
			  out, sizeof(out), &r);

	/* Criterion 1+3: correct trivial result from object bytes, via static name. */
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(r.out_len == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == obj_len);

	/* Criterion 2: completion fired on the SPDK (main) OS thread, but the module
	 * body ran on the executor worker — a DIFFERENT OS thread. Deterministic. */
	printf("\n    off-reactor proof: spdk/reactor_tid=0x%lx worker_tid=0x%lx differ=%s\n",
	       (unsigned long)main_tid, (unsigned long)g_nkvx.tid,
	       pthread_equal(g_nkvx.tid, main_tid) ? "NO" : "YES");
	CU_ASSERT(pthread_equal(r.complete_tid, main_tid));
	CU_ASSERT(g_nkvx.running);
	CU_ASSERT(!pthread_equal(g_nkvx.tid, main_tid));

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_identity(void)
{
	static const char obj[] = "near-data-compute payload";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_IDENTITY, obj, obj_len,
			  out, sizeof(out), &r);

	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(r.out_len == obj_len);
	CU_ASSERT(memcmp(out, obj, obj_len) == 0);

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_unknown_module(void)
{
	static const char obj[] = "x";
	uint8_t out[8];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* No static binding for this op-ID -> NOT_SUPPORTED, no result bytes. */
	dispatch_and_wait("does-not-exist", obj, sizeof(obj), out, sizeof(out), &r);
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
	CU_ASSERT(r.out_len == 0);

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_bytecount_buffer_too_small(void)
{
	static const char obj[] = "0123456789";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[4]; /* < sizeof(uint64_t): forces a truncated copy */
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, obj, obj_len,
			  out, sizeof(out), &r);

	/* True length still reported (8), but the small buffer is flagged. */
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL);
	CU_ASSERT(r.out_len == sizeof(uint64_t));

	kvdev_rados_nkvx_stop();
}

/*
 * Real-wasm path (TB-WIMP / ADR-0013). Dispatch "wasm:bytecount" off the reactor;
 * the dlopen-backed runtime instantiates the checked-in bytecount.wasm and
 * runs it against a PLAIN COPY of the object bytes in linear memory.
 *
 * Graceful-degradation aware: if SPDK was built --without-wasm, or libwasmtime.so
 * is not installed on this host, the runtime is unavailable and the dispatch must
 * fail SOFT with NOT_SUPPORTED (never crash). When the runtime IS available the
 * result must equal the object length (mirroring the bytecount built-in). Either
 * way the off-reactor dispatch/threading is exercised.
 */
static void
test_nkvx_wasm_bytecount_offreactor(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
#endif

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX
			  KVDEV_RADOS_NKVX_MODULE_BYTECOUNT,
			  obj, obj_len, out, sizeof(out), &r);

	/* The dispatch itself always completes (no crash) on the SPDK thread. */
	CU_ASSERT(r.completed);

	if (r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS) {
		uint64_t got = 0;

		/* Runtime available: a REAL .wasm ran off-reactor and returned the
		 * object length as an LE u64 read back from linear memory. */
		CU_ASSERT(r.out_len == sizeof(uint64_t));
		memcpy(&got, out, sizeof(got));
		CU_ASSERT(got == obj_len);
		printf("\n    real-wasm bytecount ran off-reactor: got=%llu expected=%u\n",
		       (unsigned long long)got, obj_len);
	} else {
		/* Runtime unavailable (built --without-wasm or libwasmtime.so absent):
		 * fail-soft with a DISTINCT status, no result bytes, no crash. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		printf("\n    wasm runtime unavailable -> fail-soft NOT_SUPPORTED (ok)\n");
	}

	kvdev_rados_nkvx_stop();
}

/*
 * TB2 per-invocation caps. These only run a REAL module when the wasm runtime is
 * available (built --with-wasm AND libwasmtime.so present AND the .wasm dir is
 * known); otherwise they assert the fail-soft NOT_SUPPORTED path, like the
 * bytecount wasm test. Caps are sourced per-invocation from env overrides
 * (SPDK_NKVX_WASM_FUEL / _EPOCH_TICKS / _MAX_MEMORY), exactly the TB2 source.
 */
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
#define NKVX_WASM_RUNTIME_TESTS 1
#endif

/* Fuel cap: a tight compute-runaway module is killed by fuel exhaustion and the
 * dispatch returns the distinct ABORTED status — never a crash/hang. */
static void
test_nkvx_wasm_fuel_runaway_aborted(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Small fuel ceiling so the tight loop exhausts it fast; epoch off so this
	 * test isolates the FUEL path; default memory. */
	setenv("SPDK_NKVX_WASM_FUEL", "1000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:fuel_runaway", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		/* Runtime available: fuel must have killed it -> ABORTED, no crash. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED);
		printf("\n    fuel-runaway contained: status=%d (ABORTED expected)\n", r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (fuel test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: fuel cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Epoch cap: a wall-clock-runaway module with FUEL DISABLED is stopped only by
 * the epoch deadline the background ticker advances -> ABORTED, never a hang. */
static void
test_nkvx_wasm_walltime_runaway_epoch_aborted(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Fuel OFF: ONLY epoch can stop it. Few ticks so the wall-clock budget is
	 * short (a few * 10ms). */
	setenv("SPDK_NKVX_WASM_FUEL", "0", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "3", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:walltime_runaway", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED);
		printf("\n    walltime-runaway contained by EPOCH: status=%d (ABORTED expected)\n",
		       r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (epoch test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: epoch cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Memory cap: an over-allocating module (memory.grow past the cap) is CONTAINED
 * by the store limiter -> the module's grow fails and it traps; the dispatch
 * returns a contained failure and the target survives (never an OOM). */
static void
test_nkvx_wasm_overalloc_contained(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Tight memory cap (1 MiB) so memory.grow fails quickly. Fuel/epoch high
	 * enough not to interfere — the grow loop is short. */
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "1048576", 1);
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:overalloc", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		/* Contained: a clean failure status (ABORTED if classified, else
		 * FAILED), NOT a crash. Either is acceptable containment. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED ||
			  r.kvstatus == SPDK_KVDEV_IO_STATUS_FAILED);
		printf("\n    over-alloc contained by MEMORY cap: status=%d (target survived)\n",
		       r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (memory test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: memory cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Caps configurable: the normal bytecount module STILL completes within generous
 * caps (proves caps don't break the happy path and are read per-invocation). */
static void
test_nkvx_wasm_normal_within_caps(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Explicit, modest caps -> bytecount still succeeds. */
	setenv("SPDK_NKVX_WASM_FUEL", "10000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "100", 1);
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "8388608", 1);

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:bytecount", obj, obj_len, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		uint64_t got = 0;

		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
		CU_ASSERT(r.out_len == sizeof(uint64_t));
		memcpy(&got, out, sizeof(got));
		CU_ASSERT(got == obj_len);
		printf("\n    normal bytecount within caps: got=%llu (SUCCESS)\n",
		       (unsigned long long)got);
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
#else
	(void)obj;
	(void)obj_len;
	(void)out;
	(void)r;
	printf("\n    built --without-wasm: caps happy-path test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_dispatch_without_thread_fails(void)
{
	uint8_t out[8];
	int rc;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* Off any SPDK thread there is no origin to complete on -> -EINVAL. */
	set_thread(INVALID_THREAD);
	rc = kvdev_rados_nkvx_dispatch(KVDEV_RADOS_NKVX_MODULE_IDENTITY, "x", 1,
				       out, sizeof(out), nkvx_done, NULL);
	CU_ASSERT(rc == -EINVAL);

	kvdev_rados_nkvx_stop();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("kvdev_rados_nkvx", NULL, NULL);

	CU_ADD_TEST(suite, test_nkvx_bytecount_offreactor);
	CU_ADD_TEST(suite, test_nkvx_identity);
	CU_ADD_TEST(suite, test_nkvx_unknown_module);
	CU_ADD_TEST(suite, test_nkvx_bytecount_buffer_too_small);
	CU_ADD_TEST(suite, test_nkvx_wasm_bytecount_offreactor);
	CU_ADD_TEST(suite, test_nkvx_wasm_fuel_runaway_aborted);
	CU_ADD_TEST(suite, test_nkvx_wasm_walltime_runaway_epoch_aborted);
	CU_ADD_TEST(suite, test_nkvx_wasm_overalloc_contained);
	CU_ADD_TEST(suite, test_nkvx_wasm_normal_within_caps);
	CU_ADD_TEST(suite, test_nkvx_dispatch_without_thread_fails);

	/* One SPDK thread stands in for the reactor; the executor worker is a real
	 * separate pthread, so the off-reactor thread-id check is meaningful. */
	allocate_threads(1);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	free_threads();
	CU_cleanup_registry();
	return num_failures;
}
