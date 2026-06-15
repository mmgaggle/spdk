/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * rados-nkvx executor backend (Slice C5a.1): the librados cold-fill + module run
 * that the standalone executor service (nkvx_service.c) invokes from its Mercury
 * handler. This is the "object/cache path on the executor" the design relocates
 * here (docs/design/slice-c-exec-rpc-mercury.md §2): the executor owns its own
 * librados (cold path only, ADR-0009), resolves oid=hex(key), cold-fills the
 * object, and runs the module — the front never ships object bytes.
 *
 * C5a.1 scope: the BUILT-IN modules (bytecount/identity) over a synchronous
 * librados cold-fill, replacing the C2 NOT_SUPPORTED stub for the built-in
 * route. Real-wasm (module fetch + sha256 verify + run_cached cold-fill-once +
 * the TB4 cache) is C5a.2; cross-hop invalidation is C5b.
 */

#ifndef NKVX_EXECUTOR_H
#define NKVX_EXECUTOR_H

#include "nkvx_exec_rpc.h"	/* nkvx_exec_in_t / nkvx_exec_out_t */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_executor;

/**
 * Open the executor's librados handle (cold path only, ADR-0009) and an ioctx on
 * \p pool (== the tenant subsystem's pool) with \p ns set (== KV namespace).
 *
 * \param conf  ceph.conf path (NULL -> librados default search).
 * \param user  ceph client id, e.g. "admin" (NULL -> "admin").
 * \param pool  rados pool holding the KV objects (required).
 * \param ns    rados namespace (NULL/"" -> default namespace).
 * \return 0 and *out set on success; negative errno-style otherwise.
 */
int nkvx_executor_open(const char *conf, const char *user, const char *pool,
		       const char *ns, struct nkvx_executor **out);

/** Tear down the executor's librados handle. */
void nkvx_executor_close(struct nkvx_executor *ex);

/**
 * Run ONE decoded Exec request: resolve oid=hex(in->key), cold-fill the object
 * from RADOS, run the requested module, and populate \p out (status as a wire
 * int32, the TRUE result_len, and a freshly-allocated result_inline of the
 * delivered bytes). The caller responds with \p out then releases it via
 * nkvx_exec_out_free().
 *
 * Always returns 0 with the outcome carried in out->status (mapped to a tenant
 * CQE on the front); a negative return is reserved for an internal contract
 * violation (out left zeroed/FAILED).
 */
int nkvx_executor_run(struct nkvx_executor *ex, const nkvx_exec_in_t *in,
		      nkvx_exec_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_EXECUTOR_H */
