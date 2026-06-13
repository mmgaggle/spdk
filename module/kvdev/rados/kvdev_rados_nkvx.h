/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * rados-nkvx: the sandboxed near-data Exec executor (TB1 walking skeleton).
 *
 * This is the NEW local-execution path mandated by ADR-0009: KV Exec runs in an
 * in-process sandboxed executor, NOT as a Ceph object-class in the OSD. The
 * existing rados_read_op_exec/cls path in kvdev_rados.c is deliberately left
 * untouched and is being phased out.
 *
 * TB1 scope (intentionally thin):
 *   - A CPU-issued KV Exec selects a hardcoded built-in module by op-ID (via a
 *     "nkvx:" binding prefix the NVMf allowlist forwards verbatim).
 *   - The object is cold-filled from RADOS via plain librados aio (the same
 *     harvest-on-the-SPDK-thread poller pattern the rest of the kvdev uses).
 *   - The module runs OFF the SPDK reactor thread on a dedicated worker, with
 *     async completion handed back to the originating SPDK thread. This
 *     off-reactor hand-off is the load-bearing piece (rados-nkvx-plan §3.2:
 *     "Off-reactor execution ... harder than the sandbox").
 *
 * Explicitly OUT of TB1 scope (later tracer bullets): allowlist/sidecar, caps
 * (fuel/epoch/memory), content-addressed raw-bdev cache, hash-verify,
 * warm-instance reuse, two-tier CRUSH routing.
 *
 * wasmtime SEAM: see kvdev_rados_nkvx_run_module(). TB1 runs a trivial built-in
 * in C because wasmtime is not yet linked into the SPDK build; the seam is
 * marked so TB2 can swap in a real (tiny) wasmtime invocation without disturbing
 * the off-reactor dispatch proven here.
 */

#ifndef SPDK_KVDEV_RADOS_NKVX_H
#define SPDK_KVDEV_RADOS_NKVX_H

#include "spdk/stdinc.h"

/* Binding-string prefix that routes a KV Exec to the nkvx local executor instead
 * of the legacy cls path. The text after the prefix names the built-in module. */
#define KVDEV_RADOS_NKVX_BINDING_PREFIX "nkvx:"

/* Built-in module names (TB1: statically bound, selected by the binding). */
#define KVDEV_RADOS_NKVX_MODULE_BYTECOUNT "bytecount"
#define KVDEV_RADOS_NKVX_MODULE_IDENTITY  "identity"

/*
 * Real-wasm module prefix (TB-WIMP / ADR-0013). A module name of the form
 * "wasm:<name>" routes the Exec to the dlopen-backed wasmtime runtime instead of
 * a C built-in. The full binding is therefore "nkvx:wasm:<name>"; <name> selects
 * a precompiled <name>.wasm from the nkvx wasm module directory (see
 * KVDEV_RADOS_NKVX_WASM_DIR_ENV).
 *
 * Graceful degradation (ADR-0013): when SPDK was built --without-wasm, or
 * libwasmtime.so cannot be dlopen'd at runtime, a wasm module fails with a
 * DISTINCT "runtime unavailable" status (SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) —
 * never a crash. The C built-ins above always work regardless.
 */
#define KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX "wasm:"

/* Env var naming the directory that holds precompiled <name>.wasm modules. The
 * e2e and unit tests set this; if unset the executor falls back to a path
 * relative to the SPDK build tree. */
#define KVDEV_RADOS_NKVX_WASM_DIR_ENV "SPDK_NKVX_WASM_DIR"

struct kvdev_rados_nkvx_worker;

/*
 * Start/stop the executor worker pool (one dedicated pthread in TB1). Called
 * from module_init/module_fini. Idempotent.
 */
int kvdev_rados_nkvx_start(void);
void kvdev_rados_nkvx_stop(void);

/*
 * Dispatch a built-in module run OFF the SPDK reactor.
 *
 * \param module      built-in module name (after the "nkvx:" prefix).
 * \param obj_key     stable identity key for the object (the oid). Keys the
 *                    executor's identity-addressed object cache and, with the
 *                    module, the warm-instance cache (TB4 / spdk-ii0). May be NULL
 *                    or "" to bypass the cache (plain-copy run).
 * \param obj_pin     OPTIONAL pinned cache handle from
 *                    kvdev_rados_nkvx_wasm_cache_pin (probe-hit on the datapath,
 *                    spdk-ii0 D2). When non-NULL the worker serves exactly this
 *                    pinned object version (no librados refetch, race-safe against
 *                    a concurrent Store/Delete invalidation) and releases the pin
 *                    when the run finishes. NULL on the cold-read path.
 * \param object      object bytes cold-filled from RADOS (owned by caller; must
 *                    remain valid until done_fn fires). Ignored when obj_pin set.
 * \param object_len  length of \c object.
 * \param out         host output buffer to receive the module result.
 * \param out_len     capacity of \c out.
 * \param done_fn     completion, invoked ON the originating SPDK thread with the
 *                    kvdev status and the true result length.
 * \param done_arg    opaque context for \c done_fn.
 *
 * Returns 0 if the job was queued (done_fn will fire later), or negative errno
 * if it could not be queued (done_fn will NOT fire).
 */
typedef void (*kvdev_rados_nkvx_done_fn)(void *done_arg, int kvstatus, uint32_t out_len);

int kvdev_rados_nkvx_dispatch(const char *module, const char *obj_key, void *obj_pin,
			      const void *object, size_t object_len,
			      void *out, uint32_t out_len,
			      kvdev_rados_nkvx_done_fn done_fn, void *done_arg);

#endif /* SPDK_KVDEV_RADOS_NKVX_H */
