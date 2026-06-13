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
 *   - test_reconnect_stale_completion_interleave: the regression for the
 *                                  vfio-user orphaned-tracker race -- op A times
 *                                  out (its hardware tracker is left outstanding
 *                                  and is NOT aborted by reconnect on this
 *                                  transport), op B reconnects and arms, and A's
 *                                  orphan callback fires at a LATER poll -- past
 *                                  the bounded drain, inside B's armed window.
 *                                  B must still return its OWN result, not A's
 *                                  ABORTED (0x08) status. FAILS on the old
 *                                  boolean-`expecting` gate (returns 8), PASSES
 *                                  with the per-op identity-token gate.
 *   - test_reconnect_stale_completion_two_deep: same, two orphans deep -- B's
 *                                  orphan lands inside C's armed window; the
 *                                  distinct per-op tokens keep C clean.
 *   - test_sustained_failure_ring_wrap: the case that RETURNED-TO-GO the fixed
 *                                  64-slot ring. Time out 64 ops (so the old ring
 *                                  maps op #1 and op #65 to the SAME slot), then
 *                                  fire op #1's long-outstanding orphan inside op
 *                                  #65's armed window. The old ring's slot reuse
 *                                  overwrites op #1's stored token with #65's ->
 *                                  false match -> #65 corrupted with 0x08 (rc 8).
 *                                  The per-op HEAP ctx has no slot reuse, so op
 *                                  #1's distinct immutable token != #65's -> the
 *                                  orphan is discarded and #65 returns 0. FAILS on
 *                                  ec1d9e5c7, PASSES with the heap fix. Also
 *                                  asserts every ctx is freed (incl. the 63
 *                                  remaining orphans flushed at qpair free).
 *   - test_no_ctx_leak:            per-op ctx ownership end to end -- success,
 *                                  submit-failure, timeout and transport-failure
 *                                  each free their ctx exactly once (allocs ==
 *                                  frees), the timeout/transport orphans freed
 *                                  when flushed at qpair free (close).
 *
 * A malloc/free interpose (counters) confirms zero ctx leaks; the whole suite is
 * additionally valgrind-clean (0 errors, 0 bytes in use at exit).
 *
 * The mocked clock (spdk_get_ticks / spdk_get_ticks_hz from test_env.c) lets
 * the timeout test jump past the deadline instantly, so the suite never waits
 * the real 20s budget.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/test_env.c"

/*
 * Per-op ctx leak accounting. The shim heap-allocates ONE struct kv_op_ctx per
 * submitted op (in arm_op) and frees it exactly once -- in io_complete() when
 * the op's single callback fires (real completion or orphan abort), or on the
 * submit-failure path. To prove that holds even across many timed-out ops plus
 * close (where free_io_qpair aborts all outstanding orphans), we interpose
 * malloc/free with counters BEFORE including the shim, so the shim's bare
 * malloc()/free() route through them. Tests assert g_ctx_allocs == g_ctx_frees.
 *
 * The shim uses bare malloc/free in exactly two places: the per-op ctx
 * (arm_op/io_complete/submit-fail) and the shim struct itself in
 * open()/close(). The op-path tests drive a static g_sh (never open/close it),
 * so every counted alloc/free here is a per-op ctx -- giving an exact ctx leak
 * check. (The dedicated close test below frees orphans WITHOUT freeing g_sh, so
 * it likewise only counts ctxs.)
 */
static long g_ctx_allocs;
static long g_ctx_frees;

static inline void *
ut_counting_malloc(size_t n)
{
	void *p = malloc(n);

	if (p != NULL) {
		g_ctx_allocs++;
	}
	return p;
}

static inline void
ut_counting_free(void *p)
{
	if (p != NULL) {
		g_ctx_frees++;
	}
	free(p);
}

/* Route the shim's bare malloc/free (used only for the per-op ctx in the op
 * paths under test) through the counting wrappers. Scoped to the shim include. */
#define malloc(n) ut_counting_malloc(n)
#define free(p)   ut_counting_free(p)

/* Pull in the shim under test so its static poll_to_completion()/
 * ensure_connected() are exercised directly. */
#include "nvmf/kv_shim/kv_host_shim.c"

#undef malloc
#undef free

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

/* Captured submit callback for the currently/last submitted op (see
 * ut_capture_submit). Declared here so the disconnect stub can snapshot it as
 * the stale outstanding tracker. */
static spdk_nvme_cmd_cb g_saved_cb;
static void *g_saved_cb_arg;

/*
 * Faithful model of the vfio-user abort semantics (transport facts verified in
 * lib/nvme/: nvme_qpair.c:797 gates the CONNECTED->ENABLING tracker abort on
 * trtype == PCIE, so for vfio-user that abort is SKIPPED on reconnect).
 *
 * Consequences this mock reproduces:
 *   - disconnect does NOT abort an in-flight op's HARDWARE tracker (it only
 *     flushes software-queued reqs, which the shim has none of). The tracker is
 *     left OUTSTANDING.
 *   - reconnect does NOT abort it either. The orphan callback therefore fires at
 *     an UNBOUNDED later poll, NOT deterministically on the first poll after
 *     reconnect, and NOT during the bounded reconnect drain.
 *
 * We model the worst case the reviewer demanded: the orphan fires on the Nth
 * process_completions AFTER reconnect, chosen to land PAST the shim's bounded
 * 8-pass drain AND while the NEXT op is armed (mid poll_to_completion()). With
 * this faithful timing, the OLD boolean-`expecting` fix FAILS (it records the
 * orphan's 0x08 into the armed op's slot) and only the identity-token fix PASSES.
 */
static spdk_nvme_cmd_cb g_stale_cb;	/* orphaned tracker pending to fire */
static void *g_stale_cb_arg;
static bool g_op_in_flight;		/* a submitted op has not yet completed */

/*
 * When >= 0, an orphan is pending and will fire after this many MORE
 * process_completions calls have been observed (counts down; fires at 0). This
 * lets a test schedule the orphan at an arbitrary later poll rather than on the
 * first one. -1 means no orphan pending.
 */
static int g_stale_fire_after = -1;

/*
 * How many polls after the disconnect the orphan should fire. Default 0 = the
 * legacy "fires on the first poll" timing (used by tests that don't care). The
 * interleave test sets this large enough to land PAST the shim's bounded 8-pass
 * reconnect drain and INSIDE the next op's armed poll window, which is the
 * faithful vfio-user worst case.
 */
static int g_stale_default_delay;

/*
 * When true, the single-orphan auto-firing timing (g_stale_cb / g_stale_fire_after)
 * is DISABLED, so disconnect only appends to the multi-orphan registry and the
 * test drives orphan firing explicitly via ut_fire_orphan(). Used by the
 * sustained-failure/wrap test, which needs precise control over which orphan
 * fires when, with no spurious auto-fire double-freeing a ctx.
 */
static bool g_stale_manual_only;

/*
 * Manual orphan-fire schedule for the sustained-failure/wrap test. When
 * g_manual_fire_idx >= 0, fire registry orphan g_manual_fire_idx after
 * g_manual_fire_after MORE process_completions calls, and hold off the armed
 * op's own success until then -- so the chosen orphan lands squarely inside the
 * armed op's poll window. One-shot.
 */
static int g_manual_fire_idx = -1;
static int g_manual_fire_after;

/*
 * Registry of ALL outstanding orphan trackers (vfio-user leaves an arbitrary
 * number of them outstanding under sustained failure -- they are aborted only
 * when the io qpair is freed/deleted). Every disconnect of an in-flight op
 * appends that op's (cb, cb_arg) here, in ADDITION to the single-orphan
 * g_stale_cb timing used by the interleave tests. The sustained-failure/wrap
 * test uses this registry to (a) hold 65+ simultaneous orphans -- which the OLD
 * 64-slot ring could not disambiguate -- and (b) fire a CHOSEN orphan (the very
 * first) while a much-later op is armed. spdk_nvme_ctrlr_free_io_qpair() flushes
 * the whole registry, modeling abort_trackers at close so the ctx leak check can
 * confirm every orphan's ctx is freed.
 */
#define UT_MAX_ORPHANS 256
static spdk_nvme_cmd_cb g_orphan_cb[UT_MAX_ORPHANS];
static void *g_orphan_cb_arg[UT_MAX_ORPHANS];
static int g_orphan_count;

/* Fire orphan slot `idx` with an ABORTED (0x08) status and a bogus cdw0, exactly
 * as the transport would when the abort eventually surfaces. One-shot: the slot
 * is cleared so it cannot fire (or be flushed) twice. */
static void
ut_fire_orphan(int idx)
{
	struct spdk_nvme_cpl acpl = {};

	if (idx < 0 || idx >= g_orphan_count || g_orphan_cb[idx] == NULL) {
		return;
	}
	acpl.status.sct = SPDK_NVME_SCT_GENERIC;
	acpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;	/* 0x08 */
	acpl.cdw0 = 0xdeadbeef;	/* wrong cdw0, to catch slot corruption */
	g_orphan_cb[idx](g_orphan_cb_arg[idx], &acpl);
	g_orphan_cb[idx] = NULL;
	g_orphan_cb_arg[idx] = NULL;
}

/* Remove an orphan from the registry by cb_arg WITHOUT firing it -- used when the
 * single-orphan auto-timing path (g_stale_cb) has already fired and freed it, so
 * the close-time flush does not fire/free that same (now dangling) ctx twice. */
static void
ut_forget_orphan(void *cb_arg)
{
	int i;

	for (i = 0; i < g_orphan_count; i++) {
		if (g_orphan_cb_arg[i] == cb_arg) {
			g_orphan_cb[i] = NULL;
			g_orphan_cb_arg[i] = NULL;
			return;
		}
	}
}

void
spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair)
{
	g_disconnect_calls++;
	/*
	 * If an op was in flight when we disconnected, its hardware tracker is
	 * left OUTSTANDING (NOT aborted here, and NOT aborted on reconnect for
	 * vfio-user). It becomes an orphan that the transport fires at some later
	 * poll. By default schedule it g_stale_default_delay polls out, which the
	 * interleave test sets to land past the drain and inside op B's armed
	 * window. Also append it to the multi-orphan registry so the sustained-
	 * failure test can hold many at once and fire/flush a chosen one.
	 */
	if (g_op_in_flight) {
		if (g_orphan_count < UT_MAX_ORPHANS) {
			g_orphan_cb[g_orphan_count] = g_saved_cb;
			g_orphan_cb_arg[g_orphan_count] = g_saved_cb_arg;
			g_orphan_count++;
		}
		if (!g_stale_manual_only) {
			g_stale_cb = g_saved_cb;
			g_stale_cb_arg = g_saved_cb_arg;
			g_stale_fire_after = g_stale_default_delay;
		}
		/* Move the tracker from "in flight" to "orphan, pending". Clear the
		 * saved cb so it fires exactly once, as the orphan, not again as a
		 * fresh success. */
		g_saved_cb = NULL;
		g_saved_cb_arg = NULL;
		g_op_in_flight = false;
	}
}

/*
 * Model spdk_nvme_ctrlr_free_io_qpair -> nvme_pcie_qpair_abort_trackers: on
 * qpair free/delete, EVERY still-outstanding orphan's callback fires exactly
 * once (with an aborted status). The shim's io_complete() frees each one's ctx.
 * Returns 0 like the real call.
 */
int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	int i;

	for (i = 0; i < g_orphan_count; i++) {
		ut_fire_orphan(i);
	}
	g_orphan_count = 0;
	/* Also drop any single-orphan timing state so it can't double-fire. */
	g_stale_cb = NULL;
	g_stale_cb_arg = NULL;
	g_stale_fire_after = -1;
	return 0;
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
static int g_submit_rc;

static int
ut_capture_submit(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	g_saved_cb = cb_fn;
	g_saved_cb_arg = cb_arg;
	if (g_submit_rc == 0) {
		g_op_in_flight = true;	/* a tracker is now outstanding */
	}
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

	/*
	 * Manual orphan schedule (sustained-failure/wrap test). Fire a CHOSEN
	 * registry orphan after g_manual_fire_after polls, holding off the armed
	 * op's success so the orphan lands inside the armed op's poll window.
	 */
	if (g_manual_fire_idx >= 0) {
		if (g_manual_fire_after == 0) {
			int idx = g_manual_fire_idx;

			g_manual_fire_idx = -1;
			ut_fire_orphan(idx);
			return 1;
		}
		g_manual_fire_after--;
		if (g_poll_mode == POLL_COMPLETE_SUCCESS) {
			return 0;	/* armed op stays in flight; orphan fires soon */
		}
	}

	/*
	 * Faithful orphan timing: an outstanding tracker left by a disconnected op
	 * fires its aborted callback at an ARBITRARY later poll (g_stale_fire_after
	 * polls after the disconnect), NOT necessarily on the first poll. The
	 * transport fires it unconditionally -- it has no knowledge of the shim's
	 * arming, so it can (and in the interleave test, does) land while the NEXT
	 * op is armed and mid-poll. The shim's identity token, not this timing, is
	 * what must reject it.
	 */
	if (g_stale_cb != NULL && g_stale_fire_after >= 0) {
		if (g_stale_fire_after == 0) {
			struct spdk_nvme_cpl acpl = {};

			acpl.status.sct = SPDK_NVME_SCT_GENERIC;
			acpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;	/* 0x08 */
			acpl.cdw0 = 0xdeadbeef;	/* wrong cdw0, to catch slot corruption */
			g_stale_fire_after = -1;
			/* This same orphan is also tracked in the registry; drop it
			 * there first so the close-time flush won't fire/free the
			 * (about-to-be-freed) ctx a second time. */
			ut_forget_orphan(g_stale_cb_arg);
			g_stale_cb(g_stale_cb_arg, &acpl);
			g_stale_cb = NULL;
			g_stale_cb_arg = NULL;
			return 1;
		}
		/*
		 * Orphan pending but not ready this poll. Hold off the armed op's
		 * own completion so the orphan is GUARANTEED to land while that op
		 * is still armed and polling (the faithful worst case). For the
		 * POLL_HANG/POLL_TRANSPORT_FAIL modes the op was going to make
		 * progress toward timeout/failure anyway, so fall through.
		 */
		g_stale_fire_after--;
		if (g_poll_mode == POLL_COMPLETE_SUCCESS) {
			return 0;	/* armed op stays in flight; orphan fires soon */
		}
	}

	switch (g_poll_mode) {
	case POLL_COMPLETE_SUCCESS:
		cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		cpl.status.sc = SPDK_NVME_SC_SUCCESS;
		cpl.cdw0 = 0;
		if (g_saved_cb != NULL) {
			g_saved_cb(g_saved_cb_arg, &cpl);
			/* Op completed; its tracker is consumed. Clear the saved cb
			 * so a subsequent poll (e.g. the reconnect drain loop) finds
			 * nothing pending and returns 0, matching a real qpair. */
			g_saved_cb = NULL;
			g_saved_cb_arg = NULL;
			g_op_in_flight = false;
			return 1;
		}
		return 0;
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
	/*
	 * Flush any orphans the PREVIOUS test left outstanding, modeling the
	 * qpair free at kv_host_shim_close(): each fires io_complete() once, which
	 * frees its per-op ctx. Tests that deliberately time out an op (and do not
	 * themselves close) would otherwise leak that op's ctx; flushing here keeps
	 * the whole suite valgrind-clean and proves the close-time abort path frees
	 * every ctx. Safe on the first call (registry empty). Must run BEFORE the
	 * memset below, while g_sh.armed_op_id still reflects the prior test (so the
	 * flushed orphans are correctly discarded, not recorded). */
	spdk_nvme_ctrlr_free_io_qpair(g_sh.qpair);

	memset(&g_sh, 0, sizeof(g_sh));
	g_sh.ctrlr = (struct spdk_nvme_ctrlr *)0x1;
	g_sh.ns = (struct spdk_nvme_ns *)0x2;
	g_sh.qpair = (struct spdk_nvme_qpair *)0x3;
	g_disconnect_calls = 0;
	g_reconnect_calls = 0;
	g_reconnect_rc = 0;
	g_saved_cb = NULL;
	g_saved_cb_arg = NULL;
	g_stale_cb = NULL;
	g_stale_cb_arg = NULL;
	g_stale_fire_after = -1;
	g_stale_default_delay = 0;
	g_stale_manual_only = false;
	g_manual_fire_idx = -1;
	g_manual_fire_after = 0;
	g_op_in_flight = false;
	g_submit_rc = 0;
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	memset(g_orphan_cb, 0, sizeof(g_orphan_cb));
	memset(g_orphan_cb_arg, 0, sizeof(g_orphan_cb_arg));
	g_orphan_count = 0;
	g_ctx_allocs = 0;
	g_ctx_frees = 0;
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

/*
 * Regression for the vfio-user orphaned-tracker race -- with FAITHFUL timing.
 *
 * Transport facts (lib/nvme/): vfio-user does NOT abort an outstanding tracker
 * on disconnect or reconnect, so a timed-out op's orphan callback fires at an
 * UNBOUNDED later poll -- past any bounded reconnect drain, and possibly while
 * the NEXT op is armed and mid-poll. This test models exactly that worst case:
 *
 *   1. Op A is submitted and HANGS -> times out (-ETIMEDOUT). The shim
 *      disconnects the qpair; A's TRACKER is left OUTSTANDING (g_stale_cb),
 *      scheduled to fire g_stale_default_delay polls later -- chosen to land
 *      PAST the shim's bounded 8-pass reconnect drain and INSIDE op B's armed
 *      poll window (NOT on the first poll after reconnect).
 *   2. Op B runs: ensure_connected() reconnects and best-effort drains (the
 *      orphan is NOT ready during the drain). B arms a NEW identity token and
 *      submits.
 *   3. During B's poll_to_completion(), A's orphan fires
 *      (SC_ABORTED_SQ_DELETION == 0x08, bogus cdw0) carrying A's OLD token. The
 *      next poll completes B with SUCCESS.
 *
 * Before the identity-token fix (single boolean `expecting`, which is TRUE while
 * B is armed), A's orphan landing in B's armed window is recorded as B's result:
 * done set with sc=0x08, loop exits, B returns 8 (the exact rc=8-instead-of-0
 * the reviewer saw). The bounded drain does NOT help because the orphan fires
 * after it. After the fix, A's orphan carries A's token != B's armed token, so
 * io_complete() discards it and B returns its own correct result.
 */
static void
test_reconnect_stale_completion_interleave(void)
{
	int rc;
	uint32_t vlen = 0;

	/* 1. Op A times out, leaving its tracker outstanding, scheduled to fire
	 * PAST the drain and inside B's armed poll window (delay 3: drain does one
	 * pass -> 2 left; B poll#1 -> 1; B poll#2 -> fires orphan; B poll#3 ->
	 * completes B). */
	reset_state();
	g_stale_default_delay = 3;
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_store(&g_sh, "A", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_sh.qpair_failed == true);
	CU_ASSERT(g_disconnect_calls == 1);
	/* A's tracker is now a pending orphan (not yet fired). */
	CU_ASSERT(g_stale_cb != NULL);
	CU_ASSERT(g_stale_fire_after == 3);

	/* 2+3. Op B reconnects, arms a fresh token, and completes with SUCCESS.
	 * A's orphan fires mid-B-poll but carries A's OLD token, so it is dropped.
	 * B must return ITS OWN result, never A's 0x08. */
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	g_reconnect_rc = 0;
	MOCK_SET(spdk_get_ticks, 0);
	rc = kv_host_shim_retrieve(&g_sh, "B", 1, (void *)0x4, 4, &vlen);

	CU_ASSERT(g_reconnect_calls == 1);
	CU_ASSERT(rc == 0);			/* FAILS (rc==8) on the old boolean-gate code */
	CU_ASSERT(g_sh.last_sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(g_sh.qpair_failed == false);
	/* B's own (zero) value length, not A's bogus 0xdeadbeef cdw0. */
	CU_ASSERT(vlen == 0);
	/* A's orphan was consumed exactly once. */
	CU_ASSERT(g_stale_cb == NULL);
	/* B's own tracker is not left dangling: it completed and was consumed. */
	CU_ASSERT(g_op_in_flight == false);
	CU_ASSERT(g_sh.armed_op_id == 0);

	/* 4. A follow-on op C runs cleanly with no leftover stale state -- proves
	 * B did not leave its tracker dangling to corrupt C. */
	rc = kv_host_shim_exist(&g_sh, "C", 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_reconnect_calls == 1);	/* qpair stayed healthy, no reconnect */
	CU_ASSERT(g_sh.last_sc == SPDK_NVME_SC_SUCCESS);
}

/*
 * Two-deep faithful interleave: A times out, then B times out, then C runs --
 * and B's orphan lands inside C's armed poll window. Proves the identity token
 * protects across MORE than one generation of orphan: C must complete clean even
 * though B's orphan (B's token) fires while C (a third, distinct token) is armed.
 *
 * (A's orphan is fired during B's run so that exactly one orphan -- B's -- is
 * pending when C runs, which is the case this test isolates.)
 */
static void
test_reconnect_stale_completion_two_deep(void)
{
	int rc;

	/* 1. Op A times out; its orphan is scheduled to fire on the first poll of
	 * B's run (delay 0) so it is consumed during B and does not co-mingle with
	 * B's own orphan below. */
	reset_state();
	g_stale_default_delay = 0;
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_store(&g_sh, "A", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_stale_cb != NULL);

	/* 2. Op B also times out. ensure_connected() reconnects (consuming A's
	 * orphan during the drain), B arms+submits, hangs, times out. B's tracker
	 * is now the outstanding orphan, scheduled to fire PAST C's drain and
	 * inside C's armed poll window. */
	g_stale_default_delay = 3;
	g_poll_mode = POLL_HANG;
	MOCK_SET(spdk_get_ticks, 0);
	rc = kv_host_shim_store(&g_sh, "B", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_reconnect_calls == 1);	/* reconnected once, before B */
	CU_ASSERT(g_sh.qpair_failed == true);
	/* B's tracker is the pending orphan (A's was consumed during B's drain). */
	CU_ASSERT(g_stale_cb != NULL);
	CU_ASSERT(g_stale_fire_after == 3);

	/* 3. Op C reconnects, arms a THIRD distinct token, and completes SUCCESS.
	 * B's orphan fires mid-C-poll carrying B's OLD token and is dropped. */
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	MOCK_SET(spdk_get_ticks, 0);
	rc = kv_host_shim_exist(&g_sh, "C", 1);
	CU_ASSERT(rc == 0);			/* clean, not B's 0x08 */
	CU_ASSERT(g_reconnect_calls == 2);	/* reconnected again, before C */
	CU_ASSERT(g_sh.last_sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(g_sh.qpair_failed == false);
	CU_ASSERT(g_stale_cb == NULL);
	CU_ASSERT(g_sh.armed_op_id == 0);
}

/*
 * SUSTAINED-FAILURE / RING-WRAP regression (the case the reviewer used to
 * RETURN-TO-GO the fixed 64-slot ring).
 *
 * The old fix stored each op's identity token in a slot of a fixed
 * KV_HOST_SHIM_OP_RING (64) ring, indexed by op_id & 63. Under sustained
 * failure, MANY ops time out and leave their trackers outstanding
 * simultaneously (vfio-user aborts them only at qpair free). After 64 ops the
 * ring WRAPS: op #65 reuses op #1's slot and OVERWRITES its stored op_id. When
 * op #1's long-outstanding orphan finally fires, it reads back the slot's op_id
 * -- now #65's (== armed_op_id) -- a FALSE identity match, and the orphan's
 * aborted 0x08 status corrupts the armed op #65.
 *
 * This test reproduces exactly that: time out 64 ops (ids 1..64; the old ring
 * maps op #1 and op #65 to the SAME slot 1), then arm op #65 (a fresh SUCCESS
 * op) and fire op #1's orphan inside op #65's armed poll window. With the
 * per-op heap ctx (this fix) op #1's ctx is a DISTINCT allocation whose op_id is
 * immutably 1 != 65, so the orphan is discarded and op #65 returns its own
 * SUCCESS. With the old ring fix (ec1d9e5c7) the slot aliasing makes op #65
 * return 8. Also asserts every per-op ctx is freed (no leak) including after the
 * remaining 63 orphans are flushed at qpair free (close).
 */
static void
test_sustained_failure_ring_wrap(void)
{
	int rc;
	int i;
	const int N_TIMEOUTS = 64;	/* ids 1..64; op #65 wraps onto op #1's slot */

	reset_state();
	g_stale_manual_only = true;	/* we drive orphan firing explicitly */

	/* 1. Time out N_TIMEOUTS ops. Each leaves its tracker outstanding (appended
	 * to the orphan registry) and flags qpair_failed so the next op reconnects.
	 * The OLD ring would, by op #65, have wrapped op #1's slot. */
	for (i = 0; i < N_TIMEOUTS; i++) {
		g_poll_mode = POLL_HANG;
		g_reconnect_rc = 0;
		MOCK_SET(spdk_get_ticks, 0);
		rc = kv_host_shim_store(&g_sh, "x", 1, "v", 1);
		CU_ASSERT(rc == -ETIMEDOUT);
	}
	CU_ASSERT(g_orphan_count == N_TIMEOUTS);
	CU_ASSERT(g_sh.next_op_id == (uint64_t)N_TIMEOUTS);	/* next op is #65 */
	CU_ASSERT(g_orphan_cb[0] != NULL);	/* op #1's orphan still outstanding */

	/* 2. Op #65 reconnects, arms a fresh token (id 65; old ring slot 65&63 == 1,
	 * the SAME slot op #1 used), and submits a SUCCESS op. During its armed poll
	 * we fire op #1's orphan (registry idx 0) carrying 0x08 / 0xdeadbeef. */
	g_poll_mode = POLL_COMPLETE_SUCCESS;
	g_reconnect_rc = 0;
	g_manual_fire_idx = 0;		/* op #1's orphan */
	g_manual_fire_after = 3;	/* lands past the drain, inside #65's poll */
	MOCK_SET(spdk_get_ticks, 0);
	rc = kv_host_shim_store(&g_sh, "Z", 1, "v", 1);

	/* The armed op #65 must return ITS OWN result (0), NOT op #1's orphan 0x08.
	 * On the OLD ring (ec1d9e5c7) the slot aliasing records 0x08 -> rc == 8. */
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_sh.last_sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(g_sh.last_cdw0 != 0xdeadbeef);	/* not op #1's bogus cdw0 */
	CU_ASSERT(g_sh.armed_op_id == 0);
	/* op #1's orphan was fired+freed; one fewer outstanding. */
	CU_ASSERT(g_orphan_cb[0] == NULL);

	/* 3. Close-time flush: free the io qpair, which aborts ALL remaining
	 * outstanding orphans (the 63 still-pending timed-out ops). Each fires
	 * io_complete() once -> frees its ctx. Models kv_host_shim_close()'s
	 * free_io_qpair-before-free(sh) ordering without freeing the static g_sh. */
	spdk_nvme_ctrlr_free_io_qpair(g_sh.qpair);

	/* LEAK CHECK: every per-op ctx ever allocated has now been freed exactly
	 * once -- the 64 timed-out ops' orphans (1 fired manually + 63 at flush) and
	 * op #65's own completion. No ctx leaks across unbounded orphans + close. */
	CU_ASSERT(g_ctx_allocs == N_TIMEOUTS + 1);	/* 64 timeouts + op #65 */
	CU_ASSERT(g_ctx_frees == g_ctx_allocs);
}

/*
 * Dedicated ctx-leak check for the routine paths: success, timeout, transport
 * failure, and submit-failure must each free their per-op ctx exactly once
 * (allocs == frees), with the timeout/transport orphans freed when flushed at
 * qpair free. Guards the per-op heap ownership model end to end.
 */
static void
test_no_ctx_leak(void)
{
	int rc;

	/* Success: ctx freed in io_complete on the real completion. */
	reset_state();
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_ctx_allocs == 1);
	CU_ASSERT(g_ctx_frees == 1);

	/* Submit failure: request never queued -> submit path frees the ctx. */
	reset_state();
	g_submit_rc = -EIO;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -EIO);
	CU_ASSERT(g_ctx_allocs == 1);
	CU_ASSERT(g_ctx_frees == 1);	/* freed on the submit-fail path */

	/* Timeout: ctx stays alive as an orphan until flushed at qpair free. */
	reset_state();
	g_stale_manual_only = true;
	g_poll_mode = POLL_HANG;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ETIMEDOUT);
	CU_ASSERT(g_ctx_allocs == 1);
	CU_ASSERT(g_ctx_frees == 0);	/* orphan ctx not yet freed */
	spdk_nvme_ctrlr_free_io_qpair(g_sh.qpair);	/* abort_trackers -> free */
	CU_ASSERT(g_ctx_frees == 1);

	/* Transport failure: same -- orphan ctx freed at qpair free. */
	reset_state();
	g_stale_manual_only = true;
	g_poll_mode = POLL_TRANSPORT_FAIL;
	rc = kv_host_shim_store(&g_sh, "k", 1, "v", 1);
	CU_ASSERT(rc == -ENXIO);
	CU_ASSERT(g_ctx_allocs == 1);
	CU_ASSERT(g_ctx_frees == 0);
	spdk_nvme_ctrlr_free_io_qpair(g_sh.qpair);
	CU_ASSERT(g_ctx_frees == 1);
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
	CU_ADD_TEST(suite, test_reconnect_stale_completion_interleave);
	CU_ADD_TEST(suite, test_reconnect_stale_completion_two_deep);
	CU_ADD_TEST(suite, test_sustained_failure_ring_wrap);
	CU_ADD_TEST(suite, test_no_ctx_leak);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
