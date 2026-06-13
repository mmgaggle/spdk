/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Unit tests for the KV host shim's bounded completion wait (spdk-blf).
 *
 * The bug: poll_to_completion() used to busy-loop FOREVER. If the NVMe
 * controller/target died mid-op the call hung forever, stalling the in-process
 * NIXL data path. These tests pin the fixed behaviour WITHOUT needing a live
 * target by including kv_host_shim.c directly (so the static poll loop is
 * reachable) and stubbing the SPDK NVMe submit/poll calls:
 *
 *   - test_op_completes_success:   the op completes normally -> rc 0, no delay,
 *                                  qpair NOT disconnected.
 *   - test_op_timeout:             the op never completes -> rc -ETIMEDOUT
 *                                  within the bound, qpair disconnected (the
 *                                  outstanding request is aborted).
 *   - test_op_transport_failure:   process_completions returns -ENXIO (dead
 *                                  target) -> rc -ENXIO immediately, qpair
 *                                  disconnected.
 *   - test_reconnect_after_failure: a subsequent op transparently reconnects
 *                                  the disconnected qpair, then succeeds.
 *
 * The mocked clock (spdk_get_ticks / spdk_get_ticks_hz from test_env.c) lets
 * the timeout test jump past the deadline instantly, so the suite never waits
 * the real 20s budget.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/test_env.c"

/* Pull in the shim under test so its static poll_to_completion()/
 * ensure_connected() are exercised directly. */
#include "nvmf/kv_shim/kv_host_shim.c"

/* ---- Stubs for the SPDK NVMe calls the shim references (only the ones the
 * op paths under test touch need real behaviour; the rest are no-op stubs). */

DEFINE_STUB(spdk_nvme_kv_ns_get_data, const struct spdk_nvme_kv_ns_data *,
	    (const struct spdk_nvme_ns *ns), NULL);
DEFINE_STUB(spdk_nvme_ns_get_csi, enum spdk_nvme_csi,
	    (const struct spdk_nvme_ns *ns), SPDK_NVME_CSI_KV);
DEFINE_STUB(spdk_nvme_probe, int,
	    (const struct spdk_nvme_transport_id *trid, void *cb_ctx,
	     spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
	     spdk_nvme_remove_cb remove_cb), 0);
DEFINE_STUB(spdk_nvme_detach, int, (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB(spdk_nvme_ctrlr_alloc_io_qpair, struct spdk_nvme_qpair *,
	    (struct spdk_nvme_ctrlr *ctrlr,
	     const struct spdk_nvme_io_qpair_opts *opts, size_t opts_size), NULL);
DEFINE_STUB(spdk_nvme_ctrlr_free_io_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_first_active_ns, uint32_t,
	    (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_next_active_ns, uint32_t,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_ns, struct spdk_nvme_ns *,
	    (struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid), NULL);

/* Disconnect / reconnect: count invocations so tests can assert the abort and
 * reconnect semantics on the timeout / failure paths. */
static int g_disconnect_calls;
static int g_reconnect_calls;
static int g_reconnect_rc;

void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	g_disconnect_calls++;
}

int
spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	g_reconnect_calls++;
	return g_reconnect_rc;
}

/* ---- Programmable submit + completion behaviour. ----
 *
 * The kv submit stubs capture the completion callback. process_completions then
 * drives one of three behaviours selected by g_poll_mode. */
enum poll_mode {
	POLL_COMPLETE_SUCCESS,	/* invoke cb with SUCCESS, return 1 */
	POLL_HANG,		/* never complete, return 0 (op times out) */
	POLL_TRANSPORT_FAIL,	/* return -ENXIO (qpair failed) */
};

static enum poll_mode g_poll_mode;
static spdk_nvme_cmd_cb g_saved_cb;
static void *g_saved_cb_arg;
static int g_submit_rc;

static int
ut_capture_submit(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	g_saved_cb = cb_fn;
	g_saved_cb_arg = cb_arg;
	return g_submit_rc;
}

int
spdk_nvme_kv_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len, const void *value,
		   uint32_t value_len, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		   uint8_t options)
{
	return ut_capture_submit(cb_fn, cb_arg);
}

int
spdk_nvme_kv_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const void *key, uint8_t key_len, void *value,
		      uint32_t value_len, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint8_t options)
{
	return ut_capture_submit(cb_fn, cb_arg);
}

int
spdk_nvme_kv_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len, spdk_nvme_cmd_cb cb_fn,
		   void *cb_arg)
{
	return ut_capture_submit(cb_fn, cb_arg);
}

int
spdk_nvme_kv_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		    const void *key, uint8_t key_len, spdk_nvme_cmd_cb cb_fn,
		    void *cb_arg)
{
	return ut_capture_submit(cb_fn, cb_arg);
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct spdk_nvme_cpl cpl = {};

	switch (g_poll_mode) {
	case POLL_COMPLETE_SUCCESS:
		cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		cpl.status.sc = SPDK_NVME_SC_SUCCESS;
		cpl.cdw0 = 0;
		if (g_saved_cb != NULL) {
			g_saved_cb(g_saved_cb_arg, &cpl);
		}
		return 1;
	case POLL_HANG:
		/* Simulate a target that accepted the command but will never
		 * complete it (hung, not yet transport-failed). Advance the
		 * mocked clock so the bounded wait's deadline trips on the next
		 * check -- the test never waits the real 20s budget. */
		spdk_delay_us((uint64_t)KV_HOST_SHIM_OP_TIMEOUT_S * 1000000ULL + 1);
		return 0;
	case POLL_TRANSPORT_FAIL:
	default:
		return -ENXIO;
	}
}

/* ---- Test fixture helpers ---- */

static struct kv_host_shim g_sh;

static void
reset_state(void)
{
	memset(&g_sh, 0, sizeof(g_sh));
	g_sh.ctrlr = (struct spdk_nvme_ctrlr *)0x1;
	g_sh.ns = (struct spdk_nvme_ns *)0x2;
	g_sh.qpair = (struct spdk_nvme_qpair *)0x3;
	g_disconnect_calls = 0;
	g_reconnect_calls = 0;
	g_reconnect_rc = 0;
	g_saved_cb = NULL;
	g_saved_cb_arg = NULL;
	g_submit_rc = 0;
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	MOCK_SET(spdk_get_ticks, 0);
}

/* The normal completing path: rc 0, qpair untouched (no spurious abort). This
 * also guards against the fix slowing down the happy path. */
static void
test_op_completes_success(void)
{
	int rc;

	reset_state();
	g_poll_mode = POLL_COMPLETE_SUCCESS;

	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_disconnect_calls == 0);
	CU_ASSERT(g_sh.qpair_failed == false);

	/* Same for the no-data ops. */
	rc = kv_host_shim_exist(&g_sh, "k", 1);
	CU_ASSERT(rc == 0);
	rc = kv_host_shim_delete(&g_sh, "k", 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_disconnect_calls == 0);
}

/* The core fix: an op that never completes must return -ETIMEDOUT (NOT hang)
 * and abort the outstanding request by disconnecting the qpair. Plumbed through
 * every op. */
static void
test_op_timeout(void)
{
	int rc;

	/* store */
	reset_state();
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_disconnect_calls == 1);
	CU_ASSERT(g_sh.qpair_failed == true);

	/* retrieve */
	reset_state();
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_retrieve(&g_sh, "k", 1, (void *)0x4, 4, NULL);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_disconnect_calls == 1);

	/* exist */
	reset_state();
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_exist(&g_sh, "k", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_disconnect_calls == 1);

	/* delete */
	reset_state();
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_delete(&g_sh, "k", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_disconnect_calls == 1);
}

/* A transport-layer qpair failure (the usual signal of a hard-killed target)
 * is detected immediately (-ENXIO), without waiting for the full deadline, and
 * the qpair is disconnected. */
static void
test_op_transport_failure(void)
{
	int rc;

	reset_state();
	g_poll_mode = POLL_TRANSPORT_FAIL;
	MOCK_SET(spdk_get_ticks, 0);

	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_disconnect_calls == 1);
	CU_ASSERT(g_sh.qpair_failed == true);
	/* Detected immediately: the mocked clock was never advanced. */
	CU_ASSERT(spdk_get_ticks() == 0);
}

/* After a timeout/failure disconnected the qpair, the next op must reconnect it
 * (ensure_connected) and then succeed -- a late completion from the aborted op
 * cannot corrupt this one. If reconnect fails, the op fails fast. */
static void
test_reconnect_after_failure(void)
{
	int rc;

	/* First op times out, flagging qpair_failed. */
	reset_state();
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_sh.qpair_failed == true);

	/* Next op: reconnect succeeds, then the op completes normally. */
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	g_reconnect_rc = 0;
	MOCK_SET(spdk_get_ticks, 0);
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_reconnect_calls == 1);
	CU_ASSERT(g_sh.qpair_failed == false);

	/* If reconnect fails (target still down), the op fails fast with the
	 * reconnect error and does NOT submit onto a dead qpair. */
	g_sh.qpair_failed = true;
	g_reconnect_rc = -ENODEV;
	g_saved_cb = NULL;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ENODEV);
	CU_ASSERT(g_saved_cb == NULL);	/* never submitted */
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("kv_host_shim", NULL, NULL);

	CU_ADD_TEST(suite, test_op_completes_success);
	CU_ADD_TEST(suite, test_op_timeout);
	CU_ADD_TEST(suite, test_op_transport_failure);
	CU_ADD_TEST(suite, test_reconnect_after_failure);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
