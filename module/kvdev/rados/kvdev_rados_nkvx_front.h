/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Two-tier front bridge (Slice C4): the thin glue between kvdev_rados.c (the
 * SPDK datapath) and the Mercury front client core (nkvx_front_client.{h,c}).
 *
 * This header deliberately pulls in NO Mercury headers — kvdev_rados.c includes
 * it and must stay free of <mercury.h>. `struct nkvx_front` is opaque here; its
 * definition lives in nkvx_front_client.c. The whole bridge (and these symbols)
 * compile only under SPDK_CONFIG_MERCURY; kvdev_rados.c guards every call site
 * with the same macro so a stock build is byte-identical (design §C4 / §1).
 *
 * Object-flow model (design §2): the front sends KEY-ONLY — key + binding +
 * input. The executor cold-fills the object from RADOS and owns the TB4 cache.
 */

#ifndef KVDEV_RADOS_NKVX_FRONT_H
#define KVDEV_RADOS_NKVX_FRONT_H

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, spdk_kvdev_io_completion_cb */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_front;	/* opaque; defined in nkvx_front_client.c */

/**
 * Bring up a front client bound to the executor self-address \p target_addr (as
 * published by the Slice C2 nkvx_service, design OQ-8). The NA init/provider
 * string is derived from the address prefix (e.g. "ofi+tcp://127.0.0.1:7" ->
 * origin "ofi+tcp://"). Synchronous (bootstrap, before the poller is hot).
 *
 * \return 0 and *out set on success; negative errno-style otherwise.
 */
int kvdev_rados_nkvx_front_create(const char *target_addr, struct nkvx_front **out);

/** Tear down a front client. */
void kvdev_rados_nkvx_front_destroy(struct nkvx_front *front);

/**
 * Non-blocking Mercury progress tick for the per-channel SPDK poller (design
 * §4.2): fires any ready completion callbacks on the calling (reactor) thread.
 *
 * \return number of completions fired (>= 0), or negative on a fatal error.
 */
int kvdev_rados_nkvx_front_progress(struct nkvx_front *front);

/**
 * Forward ONE nkvx Exec to the remote executor (design §2, key-only). The wire
 * fields mirror what kvdev_rados_exec() resolved from the binding:
 *   - WASM route: runtime=WASM, (module_key, module_ns) locator, sha256 (+valid),
 *     caps tier.
 *   - built-in route: runtime=CLS, module_ns="nkvx", module_key=<built-in name>,
 *     sha256_valid=false.
 *
 * On success (return 0) the RPC is in flight and \p cb_fn fires exactly once from
 * a later progress tick (on the reactor thread): up to \p output_buf_len bytes of
 * the inline result are copied into \p output_buf, then
 * cb_fn(cb_arg, status, TRUE_result_len) runs. The request fields/buffers are
 * consumed synchronously, so the caller may return immediately.
 *
 * On a synchronous submission failure returns negative and \p cb_fn is NOT
 * called (the caller completes the io). Large input (> NKVX_INLINE_MAX) needs the
 * front-origin bulk PULL path (Slice C7); until then it is reported NOT_SUPPORTED
 * via \p cb_fn (return 0).
 *
 * \return 0 if submitted (or terminally completed via cb_fn); negative on a
 *         synchronous failure where cb_fn was not invoked.
 */
int kvdev_rados_nkvx_front_forward(struct nkvx_front *front,
				   const void *key, uint8_t key_len,
				   uint32_t op_id, bool read_only,
				   uint8_t runtime,
				   const char *module_key, const char *module_ns,
				   const uint8_t *sha256, bool sha256_valid,
				   uint64_t caps,
				   const void *input, uint32_t input_len,
				   void *output_buf, uint32_t output_buf_len,
				   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* KVDEV_RADOS_NKVX_FRONT_H */
