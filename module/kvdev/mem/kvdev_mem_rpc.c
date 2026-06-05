/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"

#include "kvdev_mem.h"

struct rpc_kvdev_mem_create {
	char			*name;
	struct spdk_uuid	uuid;
	uint32_t		max_value_len;
	uint32_t		max_num_keys;
};

static void
free_rpc_kvdev_mem_create(struct rpc_kvdev_mem_create *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_kvdev_mem_create_decoders[] = {
	{"name", offsetof(struct rpc_kvdev_mem_create, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_kvdev_mem_create, uuid), spdk_json_decode_uuid, true},
	{"max_value_len", offsetof(struct rpc_kvdev_mem_create, max_value_len), spdk_json_decode_uint32, true},
	{"max_num_keys", offsetof(struct rpc_kvdev_mem_create, max_num_keys), spdk_json_decode_uint32, true},
};

static void
rpc_kvdev_mem_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_create req = {};
	struct kvdev_mem_opts opts = {};
	struct spdk_kvdev *kvdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_create_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(kvdev_mem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name;
	opts.uuid = req.uuid;
	opts.max_value_len = req.max_value_len;
	opts.max_num_keys = req.max_num_keys;

	rc = kvdev_mem_create(&opts, &kvdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_kvdev_get_name(kvdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_kvdev_mem_create(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_create", rpc_kvdev_mem_create, SPDK_RPC_RUNTIME)

struct rpc_kvdev_mem_delete {
	char	*name;
};

static void
free_rpc_kvdev_mem_delete(struct rpc_kvdev_mem_delete *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_kvdev_mem_delete_decoders[] = {
	{"name", offsetof(struct rpc_kvdev_mem_delete, name), spdk_json_decode_string},
};

static void
rpc_kvdev_mem_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_delete req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_delete_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(kvdev_mem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = kvdev_mem_delete(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_kvdev_mem_delete(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_delete", rpc_kvdev_mem_delete, SPDK_RPC_RUNTIME)

/*
 * Debug/diagnostic RPC: inspect a stored entry, notably the store-only vendor
 * TTL (ADR-0003), so end-to-end tests can assert the TTL round-trips into the
 * backend. The key is passed as a UTF-8/ASCII string (1-16 bytes).
 */
struct rpc_kvdev_mem_get_entry {
	char	*name;
	char	*key;
};

static void
free_rpc_kvdev_mem_get_entry(struct rpc_kvdev_mem_get_entry *r)
{
	free(r->name);
	free(r->key);
}

static const struct spdk_json_object_decoder rpc_kvdev_mem_get_entry_decoders[] = {
	{"name", offsetof(struct rpc_kvdev_mem_get_entry, name), spdk_json_decode_string},
	{"key", offsetof(struct rpc_kvdev_mem_get_entry, key), spdk_json_decode_string},
};

static void
rpc_kvdev_mem_get_entry(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_kvdev_mem_get_entry req = {};
	struct kvdev_mem_entry_info info = {};
	struct spdk_json_write_ctx *w;
	size_t key_len;
	int rc;

	if (spdk_json_decode_object(params, rpc_kvdev_mem_get_entry_decoders,
				    SPDK_COUNTOF(rpc_kvdev_mem_get_entry_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	key_len = strlen(req.key);
	if (key_len < 1 || key_len > 16) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "key must be 1-16 bytes");
		goto cleanup;
	}

	rc = kvdev_mem_get_entry(req.name, req.key, (uint8_t)key_len, &info);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "value_len", info.value_len);
	spdk_json_write_named_bool(w, "ttl_valid", info.ttl_valid);
	spdk_json_write_named_uint32(w, "ttl", info.ttl);
	spdk_json_write_named_uint64(w, "deadline", info.deadline);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_kvdev_mem_get_entry(&req);
}
SPDK_RPC_REGISTER("kvdev_mem_get_entry", rpc_kvdev_mem_get_entry, SPDK_RPC_RUNTIME)
