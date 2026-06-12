/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/kvdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/queue.h"

#include "kvdev_rados_nkvx.h"

/*
 * rados-nkvx executor worker (TB1). See kvdev_rados_nkvx.h for scope.
 *
 * Off-reactor dispatch, the load-bearing piece:
 *   - One dedicated pthread (a pool of one in TB1) consumes a mutex+cond work
 *     queue. KV Exec module runs happen there, NEVER on the polled SPDK reactor.
 *   - Each job records the SPDK thread that submitted it (spdk_get_thread()).
 *     When the worker finishes the module, it hands the result back with
 *     spdk_thread_send_msg(origin, ...), so the kvdev completion fires on the
 *     ORIGINATING SPDK thread — never from the worker thread, mirroring how the
 *     librados poller keeps completion delivery on the SPDK thread.
 *
 * This is exactly the impedance match the plan calls the hardest part of slice
 * #1: a multi-hundred-µs module run must not head-of-line-block co-resident
 * Retrieve traffic on the reactor.
 */

struct kvdev_rados_nkvx_job {
	char				module[32];
	const void			*object;
	size_t				object_len;
	void				*out;
	uint32_t			out_len;
	kvdev_rados_nkvx_done_fn	done_fn;
	void				*done_arg;
	struct spdk_thread		*origin;	/* SPDK thread to complete on */

	/* Filled in by the worker, read back on the SPDK thread. */
	int				kvstatus;
	uint32_t			result_len;

	STAILQ_ENTRY(kvdev_rados_nkvx_job) link;
};

static struct {
	pthread_t			tid;
	pthread_mutex_t			mutex;
	pthread_cond_t			cond;
	STAILQ_HEAD(, kvdev_rados_nkvx_job) queue;
	bool				running;
	bool				stop;
	/*
	 * TEST-ONLY: artificial per-job compute delay (µs) read from the env var
	 * SPDK_NKVX_TEST_DELAY_US at start. Defaults to 0 (no effect on production).
	 * It lets a functional test make the off-reactor compute observably long so
	 * a concurrently issued Retrieve can demonstrate the polled reactor is NOT
	 * blocked by module execution. Production never sets it.
	 */
	uint64_t			test_delay_us;
} g_nkvx = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

/*
 * wasmtime SEAM (TB1).
 *
 * Run one built-in module against the cold-filled object bytes and write the
 * result into the host output buffer. This runs ON THE WORKER THREAD.
 *
 * TB1 implements the two trivial built-ins directly in C:
 *   - "bytecount": result is the object length as a little-endian uint64_t (8B).
 *   - "identity":  result is the object bytes copied through, truncated to the
 *                  host output buffer (true length reported so the host can
 *                  detect truncation, matching Retrieve semantics).
 *
 * TB2 replaces the body below with a real (tiny) wasmtime invocation:
 *   wasm_engine_new -> wasmtime_module_new(precompiled .wasm) ->
 *   wasmtime_instance_new (no WASI imports) -> call the module export with the
 *   object in linear memory -> copy the Arrow/raw result out. The object/out
 *   contract and the off-reactor threading proven here do not change — only this
 *   function's body does. Marked clearly so the swap is local.
 */
static int
kvdev_rados_nkvx_run_module(const char *module, const void *object, size_t object_len,
			    void *out, uint32_t out_len, uint32_t *result_len)
{
	if (strcmp(module, KVDEV_RADOS_NKVX_MODULE_BYTECOUNT) == 0) {
		uint64_t count = (uint64_t)object_len;
		uint32_t copy = (uint32_t)spdk_min(sizeof(count), (size_t)out_len);

		/* Always report the true result length (8 bytes); copy what fits. */
		if (copy > 0 && out != NULL) {
			memcpy(out, &count, copy);
		}
		*result_len = (uint32_t)sizeof(count);
		return sizeof(count) > out_len ?
		       SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	if (strcmp(module, KVDEV_RADOS_NKVX_MODULE_IDENTITY) == 0) {
		uint32_t copy = (uint32_t)spdk_min(object_len, (size_t)out_len);

		if (copy > 0 && out != NULL && object != NULL) {
			memcpy(out, object, copy);
		}
		*result_len = (uint32_t)spdk_min(object_len, (size_t)UINT32_MAX);
		return object_len > out_len ?
		       SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	/* Unknown built-in: no static binding for this op-ID. */
	*result_len = 0;
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

/* Runs ON THE ORIGINATING SPDK THREAD: deliver the result and free the job. */
static void
kvdev_rados_nkvx_complete_on_spdk(void *ctx)
{
	struct kvdev_rados_nkvx_job *job = ctx;

	job->done_fn(job->done_arg, job->kvstatus, job->result_len);
	free(job);
}

static void *
kvdev_rados_nkvx_worker_main(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&g_nkvx.mutex);
	while (true) {
		struct kvdev_rados_nkvx_job *job;

		while (STAILQ_EMPTY(&g_nkvx.queue) && !g_nkvx.stop) {
			pthread_cond_wait(&g_nkvx.cond, &g_nkvx.mutex);
		}
		if (g_nkvx.stop && STAILQ_EMPTY(&g_nkvx.queue)) {
			break;
		}
		job = STAILQ_FIRST(&g_nkvx.queue);
		STAILQ_REMOVE_HEAD(&g_nkvx.queue, link);
		pthread_mutex_unlock(&g_nkvx.mutex);

		/* TEST-ONLY artificial compute delay (see test_delay_us). Stands in for
		 * a realistic 100s-of-µs–ms module run so a concurrent Retrieve can show
		 * the reactor is not blocked. No-op in production (delay == 0). */
		if (g_nkvx.test_delay_us > 0) {
			usleep(g_nkvx.test_delay_us);
		}

		/* The actual off-reactor compute. */
		job->kvstatus = kvdev_rados_nkvx_run_module(job->module, job->object,
				job->object_len, job->out, job->out_len,
				&job->result_len);

		/* Hand the result back to the SPDK thread that submitted it; the
		 * kvdev completion fires there, never on this worker thread. */
		spdk_thread_send_msg(job->origin, kvdev_rados_nkvx_complete_on_spdk, job);

		pthread_mutex_lock(&g_nkvx.mutex);
	}
	pthread_mutex_unlock(&g_nkvx.mutex);
	return NULL;
}

int
kvdev_rados_nkvx_start(void)
{
	int rc;

	pthread_mutex_lock(&g_nkvx.mutex);
	if (g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		return 0;
	}
	STAILQ_INIT(&g_nkvx.queue);
	g_nkvx.stop = false;

	/* TEST-ONLY off-reactor delay knob (see test_delay_us). */
	{
		const char *env = getenv("SPDK_NKVX_TEST_DELAY_US");

		g_nkvx.test_delay_us = env ? strtoull(env, NULL, 10) : 0;
		SPDK_NOTICELOG("nkvx: SPDK_NKVX_TEST_DELAY_US=%s -> test_delay_us=%" PRIu64 "\n",
			       env ? env : "(unset)", g_nkvx.test_delay_us);
	}

	rc = pthread_create(&g_nkvx.tid, NULL, kvdev_rados_nkvx_worker_main, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		SPDK_ERRLOG("nkvx: failed to start executor worker: %s\n", spdk_strerror(rc));
		return -rc;
	}
	g_nkvx.running = true;
	pthread_mutex_unlock(&g_nkvx.mutex);
	SPDK_NOTICELOG("nkvx: executor worker started (TB1: 1 thread, built-in modules)\n");
	return 0;
}

void
kvdev_rados_nkvx_stop(void)
{
	pthread_mutex_lock(&g_nkvx.mutex);
	if (!g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		return;
	}
	g_nkvx.stop = true;
	pthread_cond_broadcast(&g_nkvx.cond);
	pthread_mutex_unlock(&g_nkvx.mutex);

	pthread_join(g_nkvx.tid, NULL);

	pthread_mutex_lock(&g_nkvx.mutex);
	g_nkvx.running = false;
	pthread_mutex_unlock(&g_nkvx.mutex);
	SPDK_NOTICELOG("nkvx: executor worker stopped\n");
}

int
kvdev_rados_nkvx_dispatch(const char *module,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len,
			  kvdev_rados_nkvx_done_fn done_fn, void *done_arg)
{
	struct kvdev_rados_nkvx_job *job;
	struct spdk_thread *origin = spdk_get_thread();

	if (origin == NULL) {
		/* Must be dispatched from an SPDK thread so we can hand the
		 * completion back to it. */
		return -EINVAL;
	}

	if (module == NULL) {
		return -EINVAL;
	}

	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return -ENOMEM;
	}
	snprintf(job->module, sizeof(job->module), "%s", module);
	job->object = object;
	job->object_len = object_len;
	job->out = out;
	job->out_len = out_len;
	job->done_fn = done_fn;
	job->done_arg = done_arg;
	job->origin = origin;

	pthread_mutex_lock(&g_nkvx.mutex);
	if (!g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		free(job);
		return -ENODEV;
	}
	STAILQ_INSERT_TAIL(&g_nkvx.queue, job, link);
	pthread_cond_signal(&g_nkvx.cond);
	pthread_mutex_unlock(&g_nkvx.mutex);
	return 0;
}
