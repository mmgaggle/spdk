/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkvx executor backend (Slice C5a.1). See nkvx_executor.h and
 * docs/design/slice-c-exec-rpc-mercury.md §2.
 */

#define _POSIX_C_SOURCE 200809L

#include "nkvx_executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <rados/librados.h>

#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, runtime enum, key max */

struct nkvx_executor {
	rados_t		cluster;
	rados_ioctx_t	ioctx;
};

/* Hex-encode a binary key into a NUL-terminated oid (mirrors
 * kvdev_rados_key_to_oid in kvdev_rados.h — the front and executor MUST encode
 * the oid identically, ADR-0014). oid must hold key_len*2 + 1 bytes. */
static void
nkvx_key_to_oid(const void *key, uint8_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = key;

	for (uint8_t i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}

int
nkvx_executor_open(const char *conf, const char *user, const char *pool,
		   const char *ns, struct nkvx_executor **out)
{
	struct nkvx_executor *ex;
	int rc;

	if (out == NULL || pool == NULL || pool[0] == '\0') {
		return -EINVAL;
	}
	*out = NULL;

	ex = calloc(1, sizeof(*ex));
	if (ex == NULL) {
		return -ENOMEM;
	}

	rc = rados_create(&ex->cluster, user ? user : "admin");
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_create failed: %s\n", strerror(-rc));
		free(ex);
		return rc;
	}

	/* Explicit conf must parse; default search is best-effort (matches the
	 * front's kvdev_rados register-cluster discipline). */
	rc = rados_conf_read_file(ex->cluster, conf);
	if (conf != NULL && rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_conf_read_file(%s) failed: %s\n",
			conf, strerror(-rc));
		goto err_shutdown;
	}

	rc = rados_connect(ex->cluster);
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_connect failed: %s\n", strerror(-rc));
		goto err_shutdown;
	}

	rc = rados_ioctx_create(ex->cluster, pool, &ex->ioctx);
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_ioctx_create(pool=%s) failed: %s\n",
			pool, strerror(-rc));
		goto err_shutdown;
	}
	if (ns != NULL && ns[0] != '\0') {
		rados_ioctx_set_namespace(ex->ioctx, ns);
	}

	*out = ex;
	return 0;

err_shutdown:
	/* No ioctx is created before any of the goto sites, so only the cluster
	 * handle (valid post-rados_create) needs teardown. */
	rados_shutdown(ex->cluster);
	free(ex);
	return rc;
}

void
nkvx_executor_close(struct nkvx_executor *ex)
{
	if (ex == NULL) {
		return;
	}
	if (ex->ioctx != NULL) {
		rados_ioctx_destroy(ex->ioctx);
	}
	rados_shutdown(ex->cluster);
	free(ex);
}

/* Map a kvdev status + delivered bytes into the response envelope. Allocates
 * out->result_inline (freed by nkvx_exec_out_free); deliver bytes are copied
 * from src. */
static int
nkvx_set_result(nkvx_exec_out_t *out, enum spdk_kvdev_io_status status,
		uint32_t result_len, const void *src, uint32_t deliver)
{
	out->status = nkvx_status_to_wire(status);
	out->result_len = result_len;
	out->result_inline = NULL;
	out->result_inline_len = 0;

	if (deliver == 0) {
		return 0;
	}
	out->result_inline = malloc(deliver);
	if (out->result_inline == NULL) {
		out->status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_NOMEM);
		out->result_len = 0;
		return 0;
	}
	memcpy(out->result_inline, src, deliver);
	out->result_inline_len = deliver;
	return 0;
}

/*
 * Run a built-in module over a cold-filled object. Mirrors the canonical built-in
 * semantics in kvdev_rados_nkvx.c:600-624 (kvdev_rados_nkvx_run_module):
 *   - bytecount: result is the object length as a little-endian uint64 (8 B).
 *   - identity:  result is the object bytes (truncated to the host cap, true
 *                length reported, matching Retrieve/ADR-0014 truncation).
 * The object is the value stored at oid=hex(key); osize is the tenant output cap.
 */
static int
nkvx_run_builtin(struct nkvx_executor *ex, const char *oid, const char *module,
		 uint32_t osize, nkvx_exec_out_t *out)
{
	uint64_t size = 0;
	time_t mtime = 0;
	int rc;

	rc = rados_stat(ex->ioctx, oid, &size, &mtime);
	if (rc == -ENOENT) {
		return nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_stat(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}

	if (strcmp(module, "bytecount") == 0) {
		uint64_t count = size;	/* object length, LE on this host's wire */
		uint32_t result_len = (uint32_t)sizeof(count);
		uint32_t deliver = (osize < result_len) ? osize : result_len;
		enum spdk_kvdev_io_status st = (result_len > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;

		/* 8 B never exceeds NKVX_INLINE_MAX; always inline-deliverable. */
		return nkvx_set_result(out, st, result_len, &count, deliver);
	}

	if (strcmp(module, "identity") == 0) {
		uint32_t result_len = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
		uint32_t deliver = (osize < result_len) ? osize : result_len;
		enum spdk_kvdev_io_status st = (size > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;
		char *buf;
		int n;

		/*
		 * Large results need the front-sink bulk PUSH (Slice C7); until then
		 * only inline-sized deliveries are supported. Report NOT_SUPPORTED so
		 * the front maps it cleanly rather than silently truncating.
		 */
		if (deliver > NKVX_INLINE_MAX) {
			fprintf(stderr, "nkvx_executor: identity result %u > inline max %u; "
				"large-result bulk is Slice C7 — NOT_SUPPORTED\n",
				deliver, NKVX_INLINE_MAX);
			return nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0, NULL, 0);
		}

		if (deliver == 0) {
			return nkvx_set_result(out, st, result_len, NULL, 0);
		}
		buf = malloc(deliver);
		if (buf == NULL) {
			return nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
		}
		n = rados_read(ex->ioctx, oid, buf, deliver, 0);
		if (n < 0) {
			fprintf(stderr, "nkvx_executor: rados_read(%s) failed: %s\n",
				oid, strerror(-n));
			free(buf);
			return nkvx_set_result(out,
				(n == -ENOENT) ? SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST
					       : SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		nkvx_set_result(out, st, result_len, buf, (uint32_t)n);
		free(buf);
		return 0;
	}

	/* Unknown built-in: no static binding for this op. */
	return nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0, NULL, 0);
}

int
nkvx_executor_run(struct nkvx_executor *ex, const nkvx_exec_in_t *in,
		  nkvx_exec_out_t *out)
{
	char oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1];

	memset(out, 0, sizeof(*out));

	if (ex == NULL || in == NULL) {
		nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		return -EINVAL;
	}
	if (in->key_len == 0) {
		nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
		return 0;
	}

	nkvx_key_to_oid(in->key, in->key_len, oid);

	/*
	 * Route exactly as the front did (design §2.2): the built-in route is
	 * runtime=CLS with module_ns "nkvx" and module_key the built-in name. The
	 * real-wasm route (runtime=WASM, fetch+verify+run_cached) is Slice C5a.2.
	 */
	if (in->runtime == (uint8_t)SPDK_KV_EXEC_RUNTIME_CLS &&
	    in->module_ns != NULL && strcmp(in->module_ns, "nkvx") == 0 &&
	    in->module_key != NULL) {
		return nkvx_run_builtin(ex, oid, in->module_key, in->osize, out);
	}

	if (in->runtime == (uint8_t)SPDK_KV_EXEC_RUNTIME_WASM) {
		/* C5a.2 lands the wasm pipeline; until then decline cleanly. */
		nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0, NULL, 0);
		return 0;
	}

	nkvx_set_result(out, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
	return 0;
}
