/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"

#include <rados/librados.h>

#include "spdk/kvdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/queue.h"

#include "kvdev_rados.h"
#include "kvdev_rados_nkvx.h"
#include "kvdev_rados_nkvx_wasm.h"

#include "spdk/config.h"
#ifdef SPDK_CONFIG_MERCURY
/*
 * Slice C4 two-tier front: when a kvdev_rados is configured with a remote
 * executor address, nkvx Exec ops are forwarded over Mercury to the standalone
 * rados-nkvx executor (Slice C2) instead of running in-process. All Mercury
 * coupling lives behind this header (no <mercury.h> in this datapath file); the
 * whole path compiles out when --with-mercury is off, keeping a stock build
 * byte-identical. See docs/design/slice-c-exec-rpc-mercury.md §2/§4.
 */
#include "kvdev_rados_nkvx_front.h"
#endif

/*
 * librados-backed kvdev. See kvdev_rados.h / ADR-0002 / ADR-0004 for the model.
 *
 * IO path is fully async and never blocks: each op builds a librados aio
 * completion and is queued on the submitting channel's in-flight list. A
 * per-channel poller harvests finished librados completions (rados_aio_is_complete)
 * and fires the kvdev completion callback on the SPDK thread — librados invokes
 * its own callbacks from internal threads, so we deliberately do NOT complete
 * the kvdev request from inside a librados callback.
 */

/* ADR-0002: cap value size at 64 MB; advertised as kvvml in KV Identify. */
#define KVDEV_RADOS_MAX_VALUE_LEN (64ull * 1024 * 1024)

/*
 * rados-nkvx (ADR-0009) cold-fill cap: the executor reads the object into a
 * per-IO buffer (malloc'd to this cap) before running the module off-reactor.
 * Raised to match KVDEV_RADOS_MAX_VALUE_LEN (64 MiB) so GPU-initiated KV Exec
 * works on bulk values up to the full advertised store size. The buffer is
 * allocated per exec op and freed when the worker finishes, so the cap doubles
 * as the buffer size — a larger value is rejected by the stat-size check below.
 */
#define KVDEV_RADOS_NKVX_COLDFILL_CAP KVDEV_RADOS_MAX_VALUE_LEN

/*
 * TB3 (spdk-fbm) module-fetch cap: the largest .wasm artifact the executor will
 * cold-read from (module_namespace, module_key). A compiled module is small; 16
 * MiB is a generous ceiling. A module object larger than this is rejected (never
 * partially read + run) — the hash check would fail on a truncated read anyway.
 */
#define KVDEV_RADOS_NKVX_MODULE_FETCH_CAP (16ull * 1024 * 1024)

/* xattr name for the store-only vendor TTL (ADR-0003 spirit; not enforced). */
#define KVDEV_RADOS_TTL_XATTR "kv_ttl"

/* KVDEV_RADOS_OID_MAX / KVDEV_RADOS_EXEC_OID_MAX / KVDEV_RADOS_NKVX_OID_BUFSZ and
 * kvdev_rados_key_to_oid() live in kvdev_rados.h so the unit test shares the exact
 * oid sizing + encoding used by this datapath (see test_nkvx_long_key_oid). */
SPDK_STATIC_ASSERT(KVDEV_RADOS_EXEC_OID_MAX >= KVDEV_RADOS_OID_MAX,
		   "Exec oid buffer must also cover spec Store/Retrieve keys");
SPDK_STATIC_ASSERT(KVDEV_RADOS_NKVX_OID_BUFSZ == KVDEV_RADOS_EXEC_OID_MAX,
		   "nkvx_oid must hold the full hex of a 255-byte KV Exec key");

/* ---- shared, named cluster registry (mirrors bdev_rbd) ------------------- */

struct kvdev_rados_cluster {
	char			*name;
	char			*user_id;
	char			**config_param;	/* NULL-terminated k,v,... */
	char			*config_file;
	char			*key_file;
	rados_t			cluster;
	uint32_t		ref;
	STAILQ_ENTRY(kvdev_rados_cluster) link;
};

static STAILQ_HEAD(, kvdev_rados_cluster) g_clusters =
	STAILQ_HEAD_INITIALIZER(g_clusters);
static pthread_mutex_t g_clusters_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Set during module_fini so the last put_cluster also shuts the cluster down
 * (kvdev destruct is async and may outlive the module_fini cluster sweep). */
static bool g_shutting_down = false;

/* ---- kvdev instance ------------------------------------------------------ */

struct kvdev_rados {
	struct spdk_kvdev		kvdev;
	char				*cluster_name;
	char				*pool_name;
	char				*namespace_name;
	rados_t				*cluster_p;	/* into the registry entry */
	rados_ioctx_t			io_ctx;		/* pool ioctx, namespace set */
#ifdef SPDK_CONFIG_MERCURY
	/*
	 * Slice C4: when non-NULL, nkvx Exec is two-tier — forwarded to the remote
	 * executor at this self-address (design §2) instead of run in-process. NULL
	 * keeps the single-tier (in-process) path. Per-channel front clients are
	 * created from this address (one per reactor thread; the Mercury context is
	 * not shared across threads).
	 */
	char				*remote_executor;
#endif
	TAILQ_ENTRY(kvdev_rados)	tailq;
};

static TAILQ_HEAD(, kvdev_rados) g_kvdevs = TAILQ_HEAD_INITIALIZER(g_kvdevs);

struct kvdev_rados_io_channel {
	struct kvdev_rados	*rdev;
	struct spdk_poller	*poller;
#ifdef SPDK_CONFIG_MERCURY
	/*
	 * Slice C4 two-tier front (present iff rdev->remote_executor). Each channel
	 * owns its own Mercury front client + progress poller — per-reactor-thread,
	 * so the Mercury context is never progressed concurrently (no locking). NULL
	 * on the single-tier path.
	 */
	struct nkvx_front	*front;
	struct spdk_poller	*front_poller;
	/*
	 * Slice C6c (bead spdk-v3w): per-command cancel-token retention for in-flight
	 * two-tier Exec forwards. A two-tier Exec does NOT allocate a kvdev_rados_io
	 * (it forwards the tenant cb_fn/cb_arg straight to the front); to let a live
	 * NVMe ABORT cancel the ONE forward it targets, each forward is tracked here as
	 * { tenant cb_arg (the abort key), front cancel token }. Inserted at submission,
	 * removed when the forward completes (normal OR aborted) via the wrapping done
	 * trampoline kvdev_rados_nkvx_fwd_done. The list is per-reactor-thread (the
	 * channel is single-threaded), so no locking; the tenant cb_arg uniquely
	 * identifies one in-flight command, so it is the abort lookup key.
	 */
	TAILQ_HEAD(, kvdev_rados_nkvx_pending) nkvx_pending;
#endif
	TAILQ_HEAD(, kvdev_rados_io) inflight;
};

#ifdef SPDK_CONFIG_MERCURY
/*
 * Slice C6c per-command cancel-token retention entry (one in-flight two-tier
 * Exec forward). Allocated at forward submission, freed when the forward's done
 * trampoline fires. Keyed for abort lookup by the tenant's opaque cb_arg.
 */
struct kvdev_rados_nkvx_pending {
	void					*tenant_cb_arg;	/* abort lookup key */
	spdk_kvdev_io_completion_cb		tenant_cb_fn;	/* real tenant completion */
	struct kvdev_rados_io_channel		*ch;		/* owning channel */
	uint64_t				token;		/* front cancel token */
	TAILQ_ENTRY(kvdev_rados_nkvx_pending)	link;
};
#endif

enum kvdev_rados_op {
	KVDEV_RADOS_OP_STORE,
	KVDEV_RADOS_OP_RETRIEVE,
	KVDEV_RADOS_OP_DELETE,
	KVDEV_RADOS_OP_EXIST,
	KVDEV_RADOS_OP_EXEC,
	/*
	 * rados-nkvx local executor (ADR-0009): a KV Exec routed to the new
	 * in-process sandboxed executor instead of the legacy cls path. The io
	 * first cold-fills the object via a librados aio read (harvested by the
	 * same poller as RETRIEVE); io_finish then dispatches the built-in module
	 * OFF the reactor (see kvdev_rados_nkvx.c) rather than completing inline.
	 */
	KVDEV_RADOS_OP_NKVX_EXEC,
};

/* One in-flight librados aio. Lives on the channel inflight list until the
 * poller observes its completion. */
struct kvdev_rados_io {
	enum kvdev_rados_op		op;
	rados_completion_t		comp;
	rados_write_op_t		write_op;	/* STORE only */
	rados_read_op_t			read_op;	/* RETRIEVE only */
	uint64_t			stat_size;	/* RETRIEVE/EXIST scratch */
	time_t				stat_mtime;	/* EXIST scratch */
	size_t				bytes_read;	/* RETRIEVE: read_op_read out */
	int				read_rval;	/* RETRIEVE: read_op_read rc */
	uint32_t			buf_len;	/* RETRIEVE/EXEC: caller buf size */
	/* EXEC only: the host's output buffer plus the librados-ALLOCATED output
	 * buffer and its length. rados_read_op_exec allocates exec_out to the cls's
	 * true output length (exec_out_len) — there is no caller buffer to over-run;
	 * io_finish copies min(true_len, buf_len) of it into host_out and frees
	 * exec_out with rados_buffer_free. */
	void				*host_out;	/* EXEC/NKVX_EXEC: caller's output buffer */
	char				*exec_out;	/* EXEC: librados-allocated output buf */
	size_t				exec_out_len;	/* EXEC: librados-allocated out length */
	int				exec_rval;	/* EXEC: read_op_exec sub-op rc */
	/* NKVX_EXEC only: cold-fill state. nkvx_obj is a malloc'd buffer sized to
	 * the object (stat); the librados read_op fills it, then io_finish hands it
	 * to the off-reactor worker, which frees it after the module runs. */
	char				nkvx_module[32];
	char				nkvx_oid[KVDEV_RADOS_NKVX_OID_BUFSZ];	/* TB4 cache key (KV Exec keys up to 255 B, ADR-0014) */
	void				*nkvx_obj;
	uint32_t			nkvx_obj_cap;	/* allocated size of nkvx_obj */
	/*
	 * TB3 (spdk-fbm) verified-module state. nkvx_mod carries the bound sha256 +
	 * caps (by value) and, on the fetch-miss path, a pointer to nkvx_mod_buf (the
	 * librados-fetched .wasm). nkvx_need_module_fetch is set when the sha256 cache
	 * missed and the module object must be cold-read from RADOS BEFORE the data
	 * object. nkvx_mod_* hold that module read's state; the module object may live
	 * in a DIFFERENT pool/namespace (nkvx_mod_ioctx), distinct from the data oid.
	 */
	struct kvdev_rados_io_channel	*ch;		/* owning channel (for async continue) */
	/*
	 * spdk-5wi: while an io is handed to the off-reactor compile worker it is OFF
	 * the channel inflight list, so destroy_channel_cb's inflight-drain cannot see
	 * it. To stop a runtime kvdev delete from freeing the channel ctx_buf (and its
	 * rdev/ioctx) out from under the compile completion — which re-arms channel I/O
	 * in start_data_phase — the dispatch takes a REAL spdk_io_channel reference here
	 * and holds it until the completion (success OR failure) releases it. The held
	 * ref defers the channel destroy past the in-flight compile, exactly the
	 * guarantee module_fini already gets by joining the worker first.
	 */
	struct spdk_io_channel		*nkvx_compile_ch;
	struct kvdev_rados_nkvx_module	nkvx_mod;
	bool				nkvx_has_mod;	/* a verified binding is present */
	/*
	 * spdk-k3z RESULT-cache state. The per-request input bytes are copied at Exec
	 * entry (the caller's input buffer is not guaranteed to outlive the async
	 * cold-fill) so the result-cache key — built on the reactor once the object
	 * content is in hand, right before the off-reactor dispatch — can fold them in.
	 * nkvx_result_key_valid marks that a key was built and a MISS occurred, so the
	 * run completion knows to memoize the freshly-computed result under it.
	 */
	void				*nkvx_input;
	uint32_t			nkvx_input_len;
	struct kvdev_rados_nkvx_result_key nkvx_result_key;
	bool				nkvx_result_key_valid;
	bool				nkvx_in_module_fetch;	/* this aio IS the module read */
	char				nkvx_mod_key[SPDK_KVDEV_EXEC_KEY_MAX_LEN + 1];
	char				nkvx_mod_ns[256];	/* module pool[/namespace] locator */
	rados_ioctx_t			nkvx_mod_ioctx;	/* module-object ioctx (own pool/ns) */
	void				*nkvx_mod_buf;	/* malloc'd module .wasm buffer */
	uint32_t			nkvx_mod_cap;	/* allocated size of nkvx_mod_buf */
	size_t				nkvx_mod_bytes_read;
	int				nkvx_mod_read_rval;
	uint64_t			nkvx_mod_stat_size;
	time_t				nkvx_mod_stat_mtime;
	spdk_kvdev_io_completion_cb	cb_fn;
	void				*cb_arg;
	TAILQ_ENTRY(kvdev_rados_io)	link;
};

static int kvdev_rados_module_init(void);
static void kvdev_rados_module_fini(void);

/* Forward declarations for the TB3 (spdk-fbm) two-phase nkvx Exec: the module-fetch
 * harvest continues into the data phase, both defined further down. */
static void kvdev_rados_aio_cb(rados_completion_t comp, void *arg);
static const struct kvdev_rados_nkvx_module *kvdev_rados_nkvx_mod_arg(struct kvdev_rados_io *io);
static int kvdev_rados_nkvx_start_data_phase(struct kvdev_rados_io *io);

static struct spdk_kvdev_module g_kvdev_rados_module = {
	.name = "kvdev_rados",
	.module_init = kvdev_rados_module_init,
	.module_fini = kvdev_rados_module_fini,
};

SPDK_KVDEV_MODULE_REGISTER(kvdev_rados, &g_kvdev_rados_module)

/* ---- config helpers (dup/free a NULL-terminated k,v list) ---------------- */

static void
kvdev_rados_free_config(char **config)
{
	char **entry;

	if (config) {
		for (entry = config; *entry; entry++) {
			free(*entry);
		}
		free(config);
	}
}

static char **
kvdev_rados_dup_config(const char *const *config)
{
	size_t count;
	char **copy;

	if (!config) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {}
	copy = calloc(count + 1, sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	for (count = 0; config[count]; count++) {
		if (!(copy[count] = strdup(config[count]))) {
			kvdev_rados_free_config(copy);
			return NULL;
		}
	}
	return copy;
}

/* ---- key -> oid hex encoding --------------------------------------------- */
/* kvdev_rados_key_to_oid() is a static-inline in kvdev_rados.h (shared with the
 * unit test). The CALLER must size oid to the key it passes: KVDEV_RADOS_OID_MAX
 * for the spec's <=16-byte Store/Retrieve keys, but KVDEV_RADOS_EXEC_OID_MAX for
 * KV Exec keys (up to SPDK_KVDEV_EXEC_KEY_MAX_LEN = 255 bytes, ADR-0014). */

/* ---- cluster registry ---------------------------------------------------- */

static void
kvdev_rados_cluster_free(struct kvdev_rados_cluster *entry)
{
	if (entry == NULL) {
		return;
	}
	kvdev_rados_free_config(entry->config_param);
	free(entry->config_file);
	free(entry->key_file);
	free(entry->user_id);
	free(entry->name);
	free(entry);
}

/* Look up a cluster by name and take a reference. Caller holds no lock. */
static int
kvdev_rados_get_cluster(const char *cluster_name, rados_t **cluster)
{
	struct kvdev_rados_cluster *entry;

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(cluster_name, entry->name) == 0) {
			entry->ref++;
			*cluster = &entry->cluster;
			pthread_mutex_unlock(&g_clusters_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	return -ENODEV;
}

static void
kvdev_rados_put_cluster(rados_t **cluster)
{
	struct kvdev_rados_cluster *entry;

	if (cluster == NULL || *cluster == NULL) {
		return;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (*cluster == &entry->cluster) {
			assert(entry->ref > 0);
			entry->ref--;
			*cluster = NULL;
			/* On shutdown the registry is being torn down: when the last
			 * referencing kvdev releases the cluster, shut it down here so
			 * module_fini's sweep (which already ran) need not chase the
			 * async kvdev frees. */
			if (g_shutting_down && entry->ref == 0) {
				STAILQ_REMOVE(&g_clusters, entry, kvdev_rados_cluster, link);
				rados_shutdown(entry->cluster);
				pthread_mutex_unlock(&g_clusters_mutex);
				kvdev_rados_cluster_free(entry);
				return;
			}
			pthread_mutex_unlock(&g_clusters_mutex);
			return;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	SPDK_ERRLOG("Cannot find registry entry for cluster=%p\n", (void *)cluster);
}

/*
 * Create + connect a rados handle: rados_create(user_id) -> conf_read_file /
 * conf_set -> connect. Runs on a non-SPDK thread (via spdk_call_unaffinitized)
 * so it does not contend with reactors. Connect CAN fail; on any failure the
 * partially-built entry is fully torn down (no leak).
 */
static int
kvdev_rados_cluster_connect(struct kvdev_rados_cluster *entry)
{
	int rc;

	rc = rados_create(&entry->cluster, entry->user_id);
	if (rc < 0) {
		SPDK_ERRLOG("rados_create failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	/* Read ceph.conf: explicit path must succeed; default path is best-effort. */
	rc = rados_conf_read_file(entry->cluster, entry->config_file);
	if (entry->config_file && rc < 0) {
		SPDK_ERRLOG("Failed to read conf file %s\n", entry->config_file);
		goto err;
	}

	if (entry->config_param) {
		char **e = entry->config_param;
		while (*e) {
			rc = rados_conf_set(entry->cluster, e[0], e[1]);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to set %s = %s\n", e[0], e[1]);
				goto err;
			}
			e += 2;
		}
	}

	if (entry->key_file) {
		rc = rados_conf_set(entry->cluster, "keyring", entry->key_file);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to set keyring = %s\n", entry->key_file);
			goto err;
		}
	}

	rc = rados_connect(entry->cluster);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect rados cluster '%s': %s\n",
			    entry->name, spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	rados_shutdown(entry->cluster);
	entry->cluster = NULL;
	return rc;
}

static int
kvdev_rados_do_register_cluster(const struct kvdev_rados_cluster_info *info)
{
	struct kvdev_rados_cluster *entry;
	int rc;

	if (info == NULL || info->name == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(info->name, entry->name) == 0) {
			pthread_mutex_unlock(&g_clusters_mutex);
			SPDK_ERRLOG("Cluster name '%s' already exists\n", info->name);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return -ENOMEM;
	}

	entry->name = strdup(info->name);
	if (entry->name == NULL) {
		rc = -ENOMEM;
		goto err;
	}
	if (info->user_id) {
		entry->user_id = strdup(info->user_id);
		if (entry->user_id == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->config_param) {
		entry->config_param = kvdev_rados_dup_config(info->config_param);
		if (entry->config_param == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->config_file) {
		entry->config_file = strdup(info->config_file);
		if (entry->config_file == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (info->key_file) {
		entry->key_file = strdup(info->key_file);
		if (entry->key_file == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}

	rc = kvdev_rados_cluster_connect(entry);
	if (rc < 0) {
		goto err;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_INSERT_TAIL(&g_clusters, entry, link);
	pthread_mutex_unlock(&g_clusters_mutex);
	SPDK_NOTICELOG("Registered rados cluster '%s'\n", entry->name);
	return 0;

err:
	kvdev_rados_cluster_free(entry);
	return rc;
}

struct kvdev_rados_register_ctx {
	const struct kvdev_rados_cluster_info	*info;
	int					rc;
};

static void *
kvdev_rados_register_cluster_unaff(void *arg)
{
	struct kvdev_rados_register_ctx *ctx = arg;

	ctx->rc = kvdev_rados_do_register_cluster(ctx->info);
	return arg;
}

int
kvdev_rados_register_cluster(const struct kvdev_rados_cluster_info *info)
{
	struct kvdev_rados_register_ctx ctx = { .info = info, .rc = -1 };

	/* rados_connect spins up threads; run it off the SPDK reactor. */
	spdk_call_unaffinitized(kvdev_rados_register_cluster_unaff, &ctx);
	return ctx.rc;
}

int
kvdev_rados_unregister_cluster(const char *name)
{
	struct kvdev_rados_cluster *entry;

	if (name == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		if (strcmp(name, entry->name) == 0) {
			if (entry->ref != 0) {
				pthread_mutex_unlock(&g_clusters_mutex);
				SPDK_ERRLOG("Cluster '%s' still in use (ref=%u)\n",
					    name, entry->ref);
				return -EBUSY;
			}
			STAILQ_REMOVE(&g_clusters, entry, kvdev_rados_cluster, link);
			rados_shutdown(entry->cluster);
			pthread_mutex_unlock(&g_clusters_mutex);
			kvdev_rados_cluster_free(entry);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_clusters_mutex);
	return -ENODEV;
}

static void
kvdev_rados_dump_cluster(struct kvdev_rados_cluster *entry, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "cluster_name", entry->name);
	if (entry->user_id) {
		spdk_json_write_named_string(w, "user_id", entry->user_id);
	}
	if (entry->config_param) {
		char **e = entry->config_param;
		spdk_json_write_named_object_begin(w, "config_param");
		while (*e) {
			spdk_json_write_named_string(w, e[0], e[1]);
			e += 2;
		}
		spdk_json_write_object_end(w);
	}
	if (entry->config_file) {
		spdk_json_write_named_string(w, "config_file", entry->config_file);
	}
	if (entry->key_file) {
		spdk_json_write_named_string(w, "key_file", entry->key_file);
	}
	spdk_json_write_object_end(w);
}

int
kvdev_rados_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name)
{
	struct kvdev_rados_cluster *entry;
	struct spdk_json_write_ctx *w;

	pthread_mutex_lock(&g_clusters_mutex);

	if (name) {
		STAILQ_FOREACH(entry, &g_clusters, link) {
			if (strcmp(name, entry->name) == 0) {
				w = spdk_jsonrpc_begin_result(request);
				kvdev_rados_dump_cluster(entry, w);
				spdk_jsonrpc_end_result(request, w);
				pthread_mutex_unlock(&g_clusters_mutex);
				return 0;
			}
		}
		pthread_mutex_unlock(&g_clusters_mutex);
		return -ENOENT;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	STAILQ_FOREACH(entry, &g_clusters, link) {
		kvdev_rados_dump_cluster(entry, w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	pthread_mutex_unlock(&g_clusters_mutex);
	return 0;
}

/* ---- IO path ------------------------------------------------------------- */

/* Translate a librados return value into a kvdev status for a given op. */
static int
kvdev_rados_xlate_status(enum kvdev_rados_op op, int ret)
{
	if (ret >= 0) {
		return SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	switch (ret) {
	case -ENOENT:
		return SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST;
	case -EEXIST:
		return SPDK_KVDEV_IO_STATUS_KEY_EXIST;
	case -ENOSPC:
	case -EDQUOT:
		return SPDK_KVDEV_IO_STATUS_NOMEM;
	case -EOPNOTSUPP:
	case -ENOSYS:
		/* KV Exec: the OSD has no such object class, or the class has no
		 * such method (rados returns -EOPNOTSUPP for an unregistered
		 * class/method). Surface NOT_SUPPORTED so the NVMf layer maps it to
		 * an NVMe not-supported/invalid-opcode status rather than a generic
		 * failure. (-ENOSYS is treated the same for older OSDs.) */
		if (op == KVDEV_RADOS_OP_EXEC) {
			return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
		}
		return SPDK_KVDEV_IO_STATUS_FAILED;
	default:
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
}

/*
 * rados-nkvx off-reactor completion (ADR-0009). Runs back ON THE SPDK THREAD
 * (spdk_thread_send_msg from the worker), so it is safe to touch io/cb state and
 * free the cold-fill buffer here. This fires the kvdev completion the NVMf layer
 * registered; the worker has already produced the result on its own thread.
 */
static void
kvdev_rados_nkvx_io_done(void *done_arg, int kvstatus, uint32_t out_len)
{
	struct kvdev_rados_io *io = done_arg;

	/*
	 * spdk-k3z: this run was dispatched because the result-cache probe MISSED
	 * (or the cache is disabled — then the key is invalid and the insert is a
	 * no-op). Memoize the freshly-computed result under the key built before
	 * dispatch so the NEXT consumer of the identical (module, object content,
	 * input) request is served without re-running the module off-reactor.
	 *
	 * Only the bytes that actually landed in the host buffer are stored
	 * (min(out_len, buf_len)); the TRUE length (out_len) is recorded so a later
	 * hit reports identical truncation. SUCCESS/BUFFER_TOO_SMALL only — the
	 * insert helper drops transient failures.
	 */
	if (io->nkvx_result_key_valid) {
		uint32_t bytes_len = io->host_out != NULL ?
				     spdk_min(out_len, io->buf_len) : 0;

		kvdev_rados_nkvx_result_insert(&io->nkvx_result_key, kvstatus, out_len,
					       io->host_out, bytes_len);
	}

	io->cb_fn(io->cb_arg, kvstatus, out_len);
	free(io->nkvx_input);
	free(io->nkvx_obj);
	free(io->nkvx_mod_buf);	/* TB3: the fetched module bytes (NULL on hit/legacy) */
	free(io);
}

/*
 * spdk-k3z: reactor-side RESULT-cache probe, run with the object CONTENT in hand
 * and BEFORE the off-reactor dispatch (mirroring the spdk-fbm cache-probe
 * short-circuit). Builds the canonical key from (module, module-sha256 for
 * verified wasm, object content, per-request input) and probes the cache.
 *
 *   - HIT: copy the memoized result into the host buffer, fire the completion
 *     with the memoized status/length, free io, and return true — NO off-reactor
 *     work. The log marker makes the hit observable.
 *   - MISS (or cache disabled / key build failed): stash the key on io (when
 *     valid) so io_done memoizes the result after the module runs, and return
 *     false so the caller proceeds to dispatch.
 *
 * \c object/\c object_len are the bytes the module will run on (the cold-filled
 * buffer, or the pinned cached object). On the pin path the caller holds the pin
 * across this call, so the bytes are stable.
 */
static bool
kvdev_rados_nkvx_result_probe(struct kvdev_rados_io *io,
			      const void *object, size_t object_len)
{
	const uint8_t *mod_sha256 = io->nkvx_has_mod ? io->nkvx_mod.sha256 : NULL;
	uint32_t result_len = 0;
	int kvstatus = 0;
	int rc;

	io->nkvx_result_key_valid = false;

	if (!kvdev_rados_nkvx_result_cache_enabled()) {
		SPDK_NOTICELOG("nkvx: result-cache DISABLED -> recompute module=%s oid=%s\n",
			       io->nkvx_module, io->nkvx_oid);
		return false;
	}

	rc = kvdev_rados_nkvx_result_key(io->nkvx_module, mod_sha256,
					 object, object_len,
					 io->nkvx_input, io->nkvx_input_len,
					 &io->nkvx_result_key);
	if (rc != 0) {
		/* Could not build the key (digest error): just recompute. */
		return false;
	}

	if (kvdev_rados_nkvx_result_lookup(&io->nkvx_result_key, io->host_out,
					   io->buf_len, &result_len, &kvstatus)) {
		SPDK_NOTICELOG("nkvx: result-cache HIT module=%s oid=%s obj_len=%zu "
			       "input_len=%u -> served WITHOUT off-reactor run "
			       "(status=%d len=%u)\n",
			       io->nkvx_module, io->nkvx_oid, object_len,
			       io->nkvx_input_len, kvstatus, result_len);
		io->cb_fn(io->cb_arg, kvstatus, result_len);
		free(io->nkvx_input);
		free(io->nkvx_obj);
		free(io->nkvx_mod_buf);
		free(io);
		return true;
	}

	SPDK_NOTICELOG("nkvx: result-cache MISS module=%s oid=%s obj_len=%zu "
		       "input_len=%u -> dispatch off-reactor run\n",
		       io->nkvx_module, io->nkvx_oid, object_len, io->nkvx_input_len);
	io->nkvx_result_key_valid = true;
	return false;
}

/*
 * Cold-fill of the nkvx object completed (harvested by the poller). Translate the
 * read result; on success hand the bytes to the OFF-REACTOR executor (which will
 * complete via kvdev_rados_nkvx_io_done). On any read failure complete inline.
 * librados read state (comp/read_op) is already released by the caller.
 */
static void
kvdev_rados_nkvx_dispatch_or_fail(struct kvdev_rados_io *io, int ret)
{
	int status;
	int rc;

	if (ret >= 0 && io->read_rval >= 0) {
		if (io->stat_size > io->nkvx_obj_cap) {
			/*
			 * TB1 cold-fills into a fixed buffer; an object larger than the
			 * cap is out of scope for the tracer bullet (TB-later: stream
			 * into the content-addressed raw-bdev cache). Report FAILED.
			 */
			SPDK_ERRLOG("nkvx: object %" PRIu64 " B exceeds TB1 cold-fill cap %u B\n",
				    (uint64_t)io->stat_size, io->nkvx_obj_cap);
			io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_FAILED, 0);
			goto done_free;
		}

		/*
		 * spdk-k3z: result-cache probe with the cold-filled object CONTENT in
		 * hand, BEFORE the off-reactor dispatch. A hit completes the io here
		 * (no worker run); a miss stashes the key so io_done memoizes the result.
		 */
		if (kvdev_rados_nkvx_result_probe(io, io->nkvx_obj, io->stat_size)) {
			return;	/* served from result cache; io already freed */
		}

		/* Dispatch the module off the reactor against the true object length.
		 * TB3: pass the verified-module arg (sha256 + fetched bytes, or NULL on
		 * the legacy/built-in path). The buffer + io live until io_done frees
		 * them. */
		rc = kvdev_rados_nkvx_dispatch(io->nkvx_module, kvdev_rados_nkvx_mod_arg(io),
					       io->nkvx_oid, NULL,
					       io->nkvx_obj, io->stat_size, io->host_out,
					       io->buf_len, kvdev_rados_nkvx_io_done, io);
		if (rc != 0) {
			SPDK_ERRLOG("nkvx: dispatch failed: %s\n", spdk_strerror(-rc));
			io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
			goto done_free;
		}
		/* On success the worker now owns io; do NOT free here. */
		return;
	}

	/* operate() succeeded but the read sub-op failed, or operate() failed. */
	status = ret >= 0 ? kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, io->read_rval)
			  : kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, ret);
	io->cb_fn(io->cb_arg, status, 0);

done_free:
	free(io->nkvx_input);
	free(io->nkvx_obj);
	free(io->nkvx_mod_buf);
	free(io);
}

/*
 * TB3 (spdk-fbm): the MODULE-object cold read completed (harvested by the poller).
 * On a successful read: verify the bytes against the bound sha256 and compile +
 * cache them by hash (kvdev_rados_nkvx_wasm_module_insert is the HARD gate — it
 * never compiles/caches bytes that fail the hash check), then CONTINUE to the
 * data-object phase. On any read/verify/compile failure: complete with a clear
 * status and free — the module is NEVER run. librados read state (comp/read_op)
 * is already released by the caller; the module ioctx is destroyed here.
 *
 * THE COMPILE IS OFF-REACTOR (spdk-5wi): the verified-hash GATE
 * (kvdev_rados_nkvx_wasm_module_verify) runs here ON THE REACTOR so a wrong/
 * tampered module is rejected synchronously and never handed to a worker; the
 * one-time Cranelift compile of a sha256-cache miss is then handed to the executor
 * worker (kvdev_rados_nkvx_dispatch_compile) so it never head-of-line-blocks the
 * poller. The fetched bytes (io->nkvx_mod_buf) are kept alive across the dispatch
 * and freed by the compile completion (kvdev_rados_nkvx_module_compiled); io is
 * owned by the worker job until then, so the bytes outlive the compile.
 */
static void
kvdev_rados_nkvx_module_compiled(void *done_arg, int kvstatus);

static void
kvdev_rados_nkvx_module_fetched(struct kvdev_rados_io *io, int ret)
{
	int status;
	int gate;
	int rc;

	/* Done with the module ioctx regardless of outcome. */
	if (io->nkvx_mod_ioctx != NULL) {
		rados_ioctx_destroy(io->nkvx_mod_ioctx);
		io->nkvx_mod_ioctx = NULL;
	}

	if (ret < 0 || io->nkvx_mod_read_rval < 0) {
		status = ret >= 0 ?
			 kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, io->nkvx_mod_read_rval) :
			 kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, ret);
		SPDK_ERRLOG("nkvx: module fetch of '%s' failed -> module not run\n",
			    io->nkvx_mod_key);
		io->cb_fn(io->cb_arg, status, 0);
		goto fail_free;
	}
	if (io->nkvx_mod_stat_size == 0 || io->nkvx_mod_stat_size > io->nkvx_mod_cap) {
		SPDK_ERRLOG("nkvx: module '%s' size %" PRIu64 " B invalid/exceeds fetch cap %u B "
			    "-> rejected\n", io->nkvx_mod_key,
			    (uint64_t)io->nkvx_mod_stat_size, io->nkvx_mod_cap);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_FAILED, 0);
		goto fail_free;
	}

	/*
	 * THE GATE (ADR-0010), ON THE REACTOR: verify the fetched bytes against the
	 * bound hash. A MISMATCH returns INVALID and the bytes are never compiled or
	 * run — surfaced distinctly so a tampered/wrong module is observably rejected.
	 * Only blessed bytes are handed to the off-reactor compiler below.
	 */
	gate = kvdev_rados_nkvx_wasm_module_verify(io->nkvx_mod.sha256, io->nkvx_mod_buf,
						  (size_t)io->nkvx_mod_stat_size);
	if (gate != SPDK_KVDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("nkvx: module '%s' rejected before compile (status %d: %s)\n",
			    io->nkvx_mod_key, gate,
			    gate == SPDK_KVDEV_IO_STATUS_INVALID ? "HASH MISMATCH" :
			    gate == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED ? "runtime unavailable" :
			    "verify failure");
		io->cb_fn(io->cb_arg, gate, 0);
		goto fail_free;
	}

	/*
	 * Hand the one-time compile OFF THE REACTOR (spdk-5wi). io->nkvx_mod_buf stays
	 * alive across the dispatch (the worker only reads it); the compile completion
	 * frees it. On a queue failure complete + free inline (done_fn will NOT fire).
	 *
	 * TEARDOWN RACE FIX (spdk-5wi): take a REAL channel reference for the duration
	 * of the off-reactor compile. While the worker owns io it is off the channel
	 * inflight list, so a concurrent runtime kvdev_rados_delete (which, unlike
	 * module_fini, does NOT stop the worker) would otherwise run
	 * destroy_channel_cb, free the channel ctx_buf, rdev and ioctx, and leave
	 * module_compiled -> start_data_phase dereferencing freed io->ch/ch->rdev and
	 * re-arming I/O on a destroyed channel. Holding spdk_io_channel here defers the
	 * channel destroy until module_compiled releases the ref, so the channel and
	 * its ioctx are guaranteed alive when the compile completes. Taken on the
	 * origin SPDK thread; released on the same thread in the completion.
	 */
	io->nkvx_compile_ch = spdk_get_io_channel(io->ch->rdev);
	if (io->nkvx_compile_ch == NULL) {
		SPDK_ERRLOG("nkvx: cannot ref channel for compile of '%s'\n", io->nkvx_mod_key);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		goto fail_free;
	}
	rc = kvdev_rados_nkvx_dispatch_compile(io->nkvx_mod.sha256, io->nkvx_mod_buf,
					       (size_t)io->nkvx_mod_stat_size,
					       kvdev_rados_nkvx_module_compiled, io);
	if (rc != 0) {
		SPDK_ERRLOG("nkvx: cannot dispatch compile of '%s': %s\n",
			    io->nkvx_mod_key, spdk_strerror(-rc));
		spdk_put_io_channel(io->nkvx_compile_ch);
		io->nkvx_compile_ch = NULL;
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		goto fail_free;
	}
	/* The worker now owns io until kvdev_rados_nkvx_module_compiled fires. */
	return;

fail_free:
	free(io->nkvx_input);
	free(io->nkvx_mod_buf);
	free(io->nkvx_obj);
	free(io);
}

/*
 * Off-reactor compile completed (spdk-5wi). Runs back ON THE ORIGINATING SPDK
 * thread via spdk_thread_send_msg, so it is safe to touch io state, free the
 * fetched bytes, and continue the Exec. The compiled artifact is now cached by
 * hash; on a compile failure the module is NEVER run.
 *
 * Teardown race (channel/modcache vs. an in-flight compile) — TWO distinct paths,
 * both covered:
 *
 *   1. Global shutdown (module_fini): kvdev_rados_nkvx_stop() JOINS the executor
 *      worker BEFORE any kvdev is deleted, so every dispatched compile has already
 *      run and queued its completion message to this thread before any channel is
 *      destroyed.
 *
 *   2. Runtime delete (RPC kvdev_rados_delete while the worker is RUNNING): this
 *      goes straight to spdk_kvdev_unregister -> destruct -> io_device unregister ->
 *      destroy_channel_cb WITHOUT stopping the worker, so a compile can still be
 *      in flight. destroy_channel_cb only drains the channel inflight list, and this
 *      io is OFF that list while the worker owns it (kvdev_rados_io_finish removed it
 *      before module_fetched ran) — so the drain would NOT wait for it. To stop the
 *      channel ctx_buf / rdev / ioctx from being freed under this completion (which
 *      re-arms channel I/O in start_data_phase), module_fetched took a REAL
 *      spdk_io_channel reference (io->nkvx_compile_ch) before dispatching; it is
 *      released here on EVERY exit. The held ref defers the io_channel destroy past
 *      the in-flight compile, giving the runtime-delete path the same guarantee the
 *      join gives the shutdown path.
 *
 * Once the data phase re-inserts a fresh aio onto inflight, the channel-drain waits
 * on it as before — identical to the inline path this replaced.
 */
static void
kvdev_rados_nkvx_module_compiled(void *done_arg, int kvstatus)
{
	struct kvdev_rados_io *io = done_arg;
	struct spdk_io_channel *compile_ch = io->nkvx_compile_ch;

	/* Release the compile's channel reference on EVERY path below. Defer the
	 * actual spdk_put_io_channel until after start_data_phase has re-armed I/O
	 * (and re-inserted onto inflight) so the channel cannot be torn down in the
	 * window between releasing the ref and start_data_phase touching io->ch. */
	io->nkvx_compile_ch = NULL;

	if (kvstatus != SPDK_KVDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("nkvx: module '%s' compile failed (status %d: %s) -> not run\n",
			    io->nkvx_mod_key, kvstatus,
			    kvstatus == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED ? "runtime unavailable" :
			    "compile/cache failure");
		io->cb_fn(io->cb_arg, kvstatus, 0);
		free(io->nkvx_input);
		free(io->nkvx_mod_buf);
		free(io->nkvx_obj);
		free(io);
		spdk_put_io_channel(compile_ch);
		return;
	}

	/*
	 * Module verified, compiled, and cached. The fetched bytes are no longer
	 * needed (the compiled artifact is cached by hash); free them now and continue
	 * to the data-object phase, which serves the cached compiled module.
	 */
	free(io->nkvx_mod_buf);
	io->nkvx_mod_buf = NULL;
	io->nkvx_in_module_fetch = false;

	/* Need a FRESH completion for the data-phase aio (the module-fetch comp was
	 * released by the caller). On failure, complete + free. */
	if (rados_aio_create_completion((void *)io, kvdev_rados_aio_cb, NULL, &io->comp) < 0) {
		SPDK_ERRLOG("nkvx: cannot create completion for data phase\n");
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		free(io->nkvx_input);
		free(io);
		spdk_put_io_channel(compile_ch);
		return;
	}
	/*
	 * start_data_phase either re-inserts io onto the channel inflight list (so the
	 * channel-drain now waits on it) or completes+frees io synchronously. EITHER
	 * way io->ch has been fully consumed before we release the compile's channel
	 * ref, so the ref is held across the last dereference of the channel. Release
	 * it AFTER start_data_phase returns. (compile_ch is a local copy taken before
	 * io could be freed.)
	 */
	kvdev_rados_nkvx_start_data_phase(io);
	spdk_put_io_channel(compile_ch);
}

/* Finish one harvested IO: derive status/value_len, fire the cb, free state. */
static void
kvdev_rados_io_finish(struct kvdev_rados_io *io)
{
	int ret = rados_aio_get_return_value(io->comp);
	int status = kvdev_rados_xlate_status(io->op, ret);
	uint32_t value_len = 0;

	/*
	 * NKVX_EXEC is special: the harvested aio is only the cold-fill read. On
	 * success we hand off OFF-REACTOR rather than completing inline, so release
	 * the librados state here and let the dispatch helper own io afterwards.
	 */
	if (io->op == KVDEV_RADOS_OP_NKVX_EXEC) {
		bool module_fetch = io->nkvx_in_module_fetch;

		rados_aio_release(io->comp);
		io->comp = NULL;
		if (io->read_op) {
			rados_release_read_op(io->read_op);
			io->read_op = NULL;
		}
		/* TB3: the module-fetch phase verifies+compiles+caches then continues to
		 * the data phase; the data phase hands the object to the worker. */
		if (module_fetch) {
			kvdev_rados_nkvx_module_fetched(io, ret);
		} else {
			kvdev_rados_nkvx_dispatch_or_fail(io, ret);
		}
		return;
	}

	if (io->op == KVDEV_RADOS_OP_RETRIEVE) {
		/* The read_op bundles read + stat in one round-trip. ret is the
		 * operate() return; the per-sub-op read result is io->read_rval and
		 * the TRUE object size is io->stat_size. Report the true length so
		 * the host can detect truncation and resize (matches kvdev_mem). */
		status = kvdev_rados_xlate_status(io->op, ret);
		if (ret >= 0 && io->read_rval >= 0) {
			uint64_t true_size = io->stat_size;

			value_len = (uint32_t)spdk_min(true_size, KVDEV_RADOS_MAX_VALUE_LEN);
			if (true_size > io->buf_len) {
				/* read_op copied at most buf_len bytes; report the
				 * full length so the host knows it was truncated. */
				status = SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL;
			} else {
				status = SPDK_KVDEV_IO_STATUS_SUCCESS;
			}
		} else if (ret >= 0) {
			/* operate() succeeded but the read sub-op failed. */
			status = kvdev_rados_xlate_status(io->op, io->read_rval);
		}
	} else if (io->op == KVDEV_RADOS_OP_EXIST && ret >= 0) {
		status = SPDK_KVDEV_IO_STATUS_SUCCESS;
	} else if (io->op == KVDEV_RADOS_OP_EXEC) {
		/*
		 * KV Exec runs via a rados read_op carrying rados_read_op_exec: ret is
		 * the operate() return and io->exec_rval is the cls method's own return
		 * code. librados ALLOCATES io->exec_out and sets io->exec_out_len to the
		 * cls's TRUE output length — there is NO caller buffer to over-run, so
		 * the old fixed-internal-buffer overflow is structurally impossible. We:
		 *   - take true_len = io->exec_out_len (the real output length),
		 *   - copy min(true_len, host buf_len) of exec_out into the host buffer,
		 *   - report true_len (bounded by the value-size cap, like Retrieve) so
		 *     the host can detect truncation and resize, and
		 *   - flag BUFFER_TOO_SMALL ONLY when true_len > host buf_len.
		 * This matches Retrieve's true-length semantics and the in-memory module
		 * exactly: an EXACT-FIT output (true_len == buf_len) reports SUCCESS with
		 * the correct length, not a spurious BUFFER_TOO_SMALL. exec_out is freed
		 * unconditionally below with rados_buffer_free. */
		status = kvdev_rados_xlate_status(io->op, ret);
		if (ret >= 0 && io->exec_rval >= 0) {
			uint64_t true_len = io->exec_out_len;
			uint32_t copy_len;

			value_len = (uint32_t)spdk_min(true_len, KVDEV_RADOS_MAX_VALUE_LEN);
			copy_len = (uint32_t)spdk_min(true_len, (uint64_t)io->buf_len);
			if (copy_len > 0 && io->host_out != NULL &&
			    io->exec_out != NULL) {
				memcpy(io->host_out, io->exec_out, copy_len);
			}
			if (true_len > io->buf_len) {
				status = SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL;
			} else {
				status = SPDK_KVDEV_IO_STATUS_SUCCESS;
			}
			SPDK_INFOLOG(kvdev_rados,
				     "KV Exec: true_len=%" PRIu64 " host_buf_len=%u -> %s\n",
				     true_len, io->buf_len,
				     status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL ?
				     "BUFFER_TOO_SMALL" : "SUCCESS");
		} else if (ret >= 0) {
			/* operate() succeeded but the cls method itself failed. */
			status = kvdev_rados_xlate_status(io->op, io->exec_rval);
		}
	}

	io->cb_fn(io->cb_arg, status, value_len);

	rados_aio_release(io->comp);
	if (io->write_op) {
		rados_release_write_op(io->write_op);
	}
	if (io->read_op) {
		rados_release_read_op(io->read_op);
	}
	/* exec_out is librados-allocated (rados_read_op_exec); free it on every
	 * path with rados_buffer_free, never plain free(). NULL is a safe no-op. */
	if (io->exec_out) {
		rados_buffer_free(io->exec_out);
	}
	free(io);
}

/*
 * Poller: harvest completed librados aios on the SPDK thread. librados fires its
 * own callbacks from internal threads, so completion delivery to the kvdev
 * consumer happens here, never in a librados thread.
 */
static int
kvdev_rados_poll(void *arg)
{
	struct kvdev_rados_io_channel *ch = arg;
	struct kvdev_rados_io *io, *tmp;
	int count = 0;

	TAILQ_FOREACH_SAFE(io, &ch->inflight, link, tmp) {
		if (!rados_aio_is_complete(io->comp)) {
			continue;
		}
		TAILQ_REMOVE(&ch->inflight, io, link);
		kvdev_rados_io_finish(io);
		count++;
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* librados completion callback. Intentionally a no-op: the poller harvests on
 * the SPDK thread. A callback is required by rados_aio_create_completion. */
static void
kvdev_rados_aio_cb(rados_completion_t comp, void *arg)
{
}

static struct kvdev_rados_io *
kvdev_rados_io_alloc(struct kvdev_rados_io_channel *ch, enum kvdev_rados_op op,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io *io;

	io = calloc(1, sizeof(*io));
	if (io == NULL) {
		return NULL;
	}
	io->op = op;
	io->ch = ch;
	io->cb_fn = cb_fn;
	io->cb_arg = cb_arg;

	if (rados_aio_create_completion((void *)io, kvdev_rados_aio_cb, NULL,
					&io->comp) < 0) {
		free(io);
		return NULL;
	}
	return io;
}

static int
kvdev_rados_store(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		  const void *value, uint32_t value_len,
		  const struct spdk_kvdev_store_opts *opts,
		  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	uint32_t flags = SPDK_KVDEV_STORE_FLAG_NONE;
	uint32_t ttl = 0;
	bool ttl_valid = false;
	int rc;

	if (value_len > rdev->kvdev.caps.max_value_len) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	/* Honour opts->size when reading extensible fields. */
	if (opts && opts->size >= offsetof(struct spdk_kvdev_store_opts, flags) +
	    sizeof(opts->flags)) {
		flags = opts->flags;
	}
	if ((flags & SPDK_KVDEV_STORE_F_TTL) && opts &&
	    opts->size >= offsetof(struct spdk_kvdev_store_opts, ttl) + sizeof(opts->ttl)) {
		ttl = opts->ttl;
		ttl_valid = true;
	}

	kvdev_rados_key_to_oid(key, key_len, oid);

	/*
	 * D2 (spdk-ii0): a Store changes the value, so any executor cache entry for
	 * this oid is now stale. Invalidate BEFORE issuing the write so a concurrent
	 * Exec cannot get a fresh cache hit on the old bytes after this point: the
	 * entry is dropped immediately and the next Exec cold-reads from RADOS. (A
	 * probe-hit already in flight keeps its pinned version, ordered before this
	 * Store — see kvdev_rados_nkvx_wasm_cache_pin.) Invalidating at submission is
	 * deliberately conservative: even if the write later fails, the only cost is a
	 * cache miss that re-reads the unchanged object — never a stale read.
	 */
	kvdev_rados_nkvx_wasm_cache_invalidate(oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_STORE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	/*
	 * Build an atomic write op so SIKE/SINKE and the value write happen as
	 * one rados operation:
	 *   SINKE -> create(EXCLUSIVE): fails -EEXIST if the object exists.
	 *   SIKE  -> assert_exists():   fails -ENOENT if the object is absent.
	 * write_full replaces the whole object (object-per-KV).
	 */
	io->write_op = rados_create_write_op();
	if (io->write_op == NULL) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	if (flags & SPDK_KVDEV_STORE_FLAG_SINKE) {
		rados_write_op_create(io->write_op, LIBRADOS_CREATE_EXCLUSIVE, NULL);
	} else if (flags & SPDK_KVDEV_STORE_FLAG_SIKE) {
		rados_write_op_assert_exists(io->write_op);
	}

	rados_write_op_write_full(io->write_op, value, value_len);

	if (ttl_valid) {
		/* Store-only TTL persisted as an xattr (ADR-0003 spirit). Never
		 * enforced: no lazy expiry / reaper. */
		char ttlbuf[16];
		int n = snprintf(ttlbuf, sizeof(ttlbuf), "%u", ttl);
		rados_write_op_setxattr(io->write_op, KVDEV_RADOS_TTL_XATTR, ttlbuf, n);
	}

	rc = rados_aio_write_op_operate(io->write_op, rdev->io_ctx, io->comp, oid,
					NULL, 0);
	if (rc < 0) {
		rados_release_write_op(io->write_op);
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_STORE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_retrieve(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		     void *value_buf, uint32_t buf_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_RETRIEVE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	io->buf_len = buf_len;

	/*
	 * Bundle read + stat in a single aio read_op so we learn the TRUE object
	 * size in the same round-trip: read copies min(size, buf_len) bytes, stat
	 * yields the full size. This lets Retrieve report the true value length
	 * (and BUFFER_TOO_SMALL when truncated), matching the in-memory module,
	 * without a second network round-trip.
	 */
	io->read_op = rados_create_read_op();
	if (io->read_op == NULL) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rados_read_op_read(io->read_op, 0, buf_len, value_buf, &io->bytes_read,
			   &io->read_rval);
	rados_read_op_stat(io->read_op, &io->stat_size, &io->stat_mtime, NULL);

	rc = rados_aio_read_op_operate(io->read_op, rdev->io_ctx, io->comp, oid, 0);
	if (rc < 0) {
		rados_release_read_op(io->read_op);
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_RETRIEVE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_op_delete(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	/* D2 (spdk-ii0): a Delete removes the value; drop any executor cache entry for
	 * this oid before issuing the remove so a subsequent Exec misses (and then
	 * fails the cold read with KEY_NOT_EXIST) rather than serving stale bytes. */
	kvdev_rados_nkvx_wasm_cache_invalidate(oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_DELETE, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rc = rados_aio_remove(rdev->io_ctx, oid, io->comp);
	if (rc < 0) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_DELETE, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_exist(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_OID_MAX];
	int rc;

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_EXIST, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rc = rados_aio_stat(rdev->io_ctx, oid, io->comp, &io->stat_size, &io->stat_mtime);
	if (rc < 0) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_EXIST, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

/*
 * rados-nkvx KV Exec (ADR-0009): the NEW local-execution path. A KV Exec whose
 * binding begins with "nkvx:" runs in the in-process sandboxed executor instead
 * of as a Ceph object-class. The text after the prefix names a built-in module
 * (TB1 statically binds "bytecount"/"identity"); op_id is not interpreted here
 * (it selected the binding upstream in the NVMf allowlist).
 *
 * Flow:
 *   1. cold-fill: a librados aio read_op (read 0..cap bytes + stat for the true
 *      size) into a malloc'd buffer, queued on the channel inflight list and
 *      harvested by the SAME poller as Retrieve — we never touch SPDK state from
 *      a librados thread.
 *   2. on read completion (kvdev_rados_io_finish -> nkvx_dispatch_or_fail) the
 *      object is handed OFF the reactor to the executor worker, which runs the
 *      module and hands the result back to this SPDK thread. The reactor is
 *      never blocked by module execution.
 */
/* Build a verified-module binding view for the worker from io's carried fields.
 * On the fetch-miss path bytes/len point at the just-fetched module buffer; on a
 * module-cache hit they are NULL/0 (the executor serves the cached compiled
 * module). Returns NULL when no verified binding is present (legacy/test path). */
static const struct kvdev_rados_nkvx_module *
kvdev_rados_nkvx_mod_arg(struct kvdev_rados_io *io)
{
	if (!io->nkvx_has_mod) {
		return NULL;
	}
	if (io->nkvx_mod_buf != NULL) {
		io->nkvx_mod.bytes = io->nkvx_mod_buf;
		io->nkvx_mod.bytes_len = (size_t)io->nkvx_mod_stat_size;
	} else {
		io->nkvx_mod.bytes = NULL;
		io->nkvx_mod.bytes_len = 0;
	}
	return &io->nkvx_mod;
}

/*
 * Start the DATA-OBJECT phase of an nkvx Exec: probe-and-pin the executor's
 * identity cache for oid; on a hit dispatch straight to the worker (no librados
 * read), on a miss issue the cold-fill aio. Runs ON THE REACTOR. Reused by the
 * module-cache-hit fast path and by the module-fetch harvest continuation. On a
 * synchronous failure it fires cb_fn and frees io (returns 0 like the rest of the
 * datapath). io->comp must be a fresh, unreleased completion on entry.
 */
static int
kvdev_rados_nkvx_start_data_phase(struct kvdev_rados_io *io)
{
	struct kvdev_rados_io_channel *ch = io->ch;
	struct kvdev_rados *rdev = ch->rdev;
	int rc;

	/*
	 * B2 (spdk-ii0): probe-and-PIN the data object's identity cache. A hit skips
	 * the librados data read entirely and dispatches straight to the worker.
	 * D2 race-safety: the pin defers the buffer free so the worker serves exactly
	 * the pinned version even across a concurrent Store/Delete invalidation.
	 */
	{
		void *pin = kvdev_rados_nkvx_wasm_cache_pin(io->nkvx_oid);

		if (pin != NULL) {
			const void *pobj = NULL;
			size_t pobj_len = 0;

			SPDK_NOTICELOG("nkvx: oid %s served from executor cache "
				       "(pinned, no librados read)\n", io->nkvx_oid);
			rados_aio_release(io->comp);
			io->comp = NULL;
			io->nkvx_obj = NULL;
			io->nkvx_obj_cap = 0;

			/*
			 * spdk-k3z: result-cache probe on the pinned object's CONTENT
			 * (read under the pin, so the bytes are stable) BEFORE the
			 * off-reactor dispatch. A hit completes here; we still own the pin,
			 * so release it before returning.
			 */
			kvdev_rados_nkvx_wasm_pin_object(pin, &pobj, &pobj_len);
			if (kvdev_rados_nkvx_result_probe(io, pobj, pobj_len)) {
				kvdev_rados_nkvx_wasm_cache_unpin(pin);
				return 0;	/* served from result cache; io already freed */
			}

			rc = kvdev_rados_nkvx_dispatch(io->nkvx_module,
						       kvdev_rados_nkvx_mod_arg(io),
						       io->nkvx_oid, pin,
						       NULL, 0, io->host_out, io->buf_len,
						       kvdev_rados_nkvx_io_done, io);
			if (rc != 0) {
				SPDK_ERRLOG("nkvx: cache-hit dispatch failed: %s\n",
					    spdk_strerror(-rc));
				kvdev_rados_nkvx_wasm_cache_unpin(pin);
				io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
				free(io->nkvx_input);
				free(io->nkvx_mod_buf);
				free(io);
				return 0;
			}
			return 0;
		}
	}

	SPDK_NOTICELOG("nkvx: oid %s cache miss -> librados cold-fill read\n", io->nkvx_oid);

	io->nkvx_obj_cap = KVDEV_RADOS_NKVX_COLDFILL_CAP;
	io->nkvx_obj = malloc(io->nkvx_obj_cap);
	if (io->nkvx_obj == NULL) {
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		free(io->nkvx_input);
		free(io->nkvx_mod_buf);
		free(io);
		return 0;
	}

	io->read_op = rados_create_read_op();
	if (io->read_op == NULL) {
		free(io->nkvx_obj);
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		free(io->nkvx_input);
		free(io->nkvx_mod_buf);
		free(io);
		return 0;
	}

	rados_read_op_read(io->read_op, 0, io->nkvx_obj_cap, io->nkvx_obj,
			   &io->bytes_read, &io->read_rval);
	rados_read_op_stat(io->read_op, &io->stat_size, &io->stat_mtime, NULL);

	rc = rados_aio_read_op_operate(io->read_op, rdev->io_ctx, io->comp, io->nkvx_oid, 0);
	if (rc < 0) {
		rados_release_read_op(io->read_op);
		free(io->nkvx_obj);
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, rc), 0);
		free(io->nkvx_input);
		free(io->nkvx_mod_buf);
		free(io);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

/*
 * TB3 (spdk-fbm): the module object lives at (module_namespace, module_key) in
 * RADOS, possibly a DIFFERENT pool/namespace than the data oid. module_namespace
 * is encoded as "pool" or "pool/namespace". Create an ioctx for it on the shared
 * cluster handle. Returns 0 on success (*out set, caller destroys it), negative
 * errno otherwise. NULL/empty module_namespace falls back to the data ioctx's pool
 * is NOT assumed — a wasm binding must name where its module lives.
 */
static int
kvdev_rados_nkvx_module_ioctx(struct kvdev_rados *rdev, const char *module_namespace,
			      rados_ioctx_t *out)
{
	char buf[256];
	char *slash;
	const char *pool;
	const char *ns = NULL;
	rados_ioctx_t ioctx;
	int rc;

	*out = NULL;
	if (module_namespace == NULL || module_namespace[0] == '\0') {
		SPDK_ERRLOG("nkvx: wasm binding has no module_namespace (module locator)\n");
		return -EINVAL;
	}
	if (snprintf(buf, sizeof(buf), "%s", module_namespace) >= (int)sizeof(buf)) {
		SPDK_ERRLOG("nkvx: module_namespace too long\n");
		return -EINVAL;
	}
	pool = buf;
	slash = strchr(buf, '/');
	if (slash != NULL) {
		*slash = '\0';
		ns = slash + 1;	/* may be "" => default namespace */
	}

	rc = rados_ioctx_create(*rdev->cluster_p, pool, &ioctx);
	if (rc < 0) {
		SPDK_ERRLOG("nkvx: cannot open module pool '%s': %s\n", pool, spdk_strerror(-rc));
		return rc;
	}
	if (ns != NULL && ns[0] != '\0') {
		rados_ioctx_set_namespace(ioctx, ns);
	}
	*out = ioctx;
	return 0;
}

/*
 * Issue the librados aio that cold-reads the MODULE object (read+stat) into
 * io->nkvx_mod_buf. Runs ON THE REACTOR; harvested by the channel poller, which
 * (on success) verifies+compiles+caches the bytes by hash and then continues to
 * the data-object phase. Only invoked on a sha256 module-cache MISS.
 */
static int
kvdev_rados_nkvx_start_module_fetch(struct kvdev_rados_io *io)
{
	struct kvdev_rados_io_channel *ch = io->ch;
	struct kvdev_rados *rdev = ch->rdev;
	int rc;

	rc = kvdev_rados_nkvx_module_ioctx(rdev, io->nkvx_mod_ns,
					   &io->nkvx_mod_ioctx);
	if (rc < 0) {
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		free(io->nkvx_input);
		free(io);
		return 0;
	}

	io->nkvx_mod_cap = KVDEV_RADOS_NKVX_MODULE_FETCH_CAP;
	io->nkvx_mod_buf = malloc(io->nkvx_mod_cap);
	if (io->nkvx_mod_buf == NULL) {
		rados_ioctx_destroy(io->nkvx_mod_ioctx);
		io->nkvx_mod_ioctx = NULL;
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		free(io->nkvx_input);
		free(io);
		return 0;
	}

	io->read_op = rados_create_read_op();
	if (io->read_op == NULL) {
		free(io->nkvx_mod_buf);
		io->nkvx_mod_buf = NULL;
		rados_ioctx_destroy(io->nkvx_mod_ioctx);
		io->nkvx_mod_ioctx = NULL;
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		free(io->nkvx_input);
		free(io);
		return 0;
	}

	io->nkvx_in_module_fetch = true;
	rados_read_op_read(io->read_op, 0, io->nkvx_mod_cap, io->nkvx_mod_buf,
			   &io->nkvx_mod_bytes_read, &io->nkvx_mod_read_rval);
	rados_read_op_stat(io->read_op, &io->nkvx_mod_stat_size, &io->nkvx_mod_stat_mtime, NULL);

	SPDK_NOTICELOG("nkvx: module-cache miss -> fetching module '%s' from '%s'\n",
		       io->nkvx_mod_key, io->nkvx_mod_ns);

	rc = rados_aio_read_op_operate(io->read_op, io->nkvx_mod_ioctx, io->comp,
				       io->nkvx_mod_key, 0);
	if (rc < 0) {
		rados_release_read_op(io->read_op);
		io->read_op = NULL;
		free(io->nkvx_mod_buf);
		io->nkvx_mod_buf = NULL;
		rados_ioctx_destroy(io->nkvx_mod_ioctx);
		io->nkvx_mod_ioctx = NULL;
		rados_aio_release(io->comp);
		io->cb_fn(io->cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, rc), 0);
		free(io->nkvx_input);
		free(io);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

static int
kvdev_rados_nkvx_exec(struct kvdev_rados_io_channel *ch, const void *key, uint8_t key_len,
		      const char *module, const struct spdk_kv_exec_binding *binding,
		      const void *input, uint32_t input_len,
		      void *output_buf, uint32_t output_buf_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_EXEC_OID_MAX];

	if (module[0] == '\0') {
		SPDK_ERRLOG("nkvx: empty module name in binding\n");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_NKVX_EXEC, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	io->buf_len = output_buf_len;
	io->host_out = output_buf;
	/*
	 * spdk-k3z: snapshot the per-request input for the result-cache key. The
	 * caller's input buffer need not outlive the async cold-fill, so copy it now.
	 * A failed copy is non-fatal — we simply skip result caching for this Exec
	 * (it recomputes), never a wrong answer.
	 */
	if (input != NULL && input_len > 0) {
		io->nkvx_input = malloc(input_len);
		if (io->nkvx_input != NULL) {
			memcpy(io->nkvx_input, input, input_len);
			io->nkvx_input_len = input_len;
		}
	}
	snprintf(io->nkvx_module, sizeof(io->nkvx_module), "%s", module);
	/* The oid (hex of the key) is the stable content/identity key for the
	 * executor's TB4 content-addressed object + warm-instance cache. */
	snprintf(io->nkvx_oid, sizeof(io->nkvx_oid), "%s", oid);

	/*
	 * TB3 (spdk-fbm): a real wasm binding carries the authorization anchor. Copy
	 * the bound sha256 + caps + module locator into the io (the binding strings are
	 * caller-owned and must not be referenced after we return). The module_key
	 * names the module OBJECT to fetch; module_namespace names its pool/ns. We keep
	 * the binding's module_namespace pointer only for the duration of THIS call —
	 * the fetch path copies what it needs (module_namespace is read synchronously
	 * in start_module_fetch before any async return).
	 */
	if (binding != NULL && binding->sha256_valid) {
		io->nkvx_has_mod = true;
		memcpy(io->nkvx_mod.sha256, binding->sha256, SPDK_KV_EXEC_SHA256_LEN);
		io->nkvx_mod.caps = binding->caps;
		io->nkvx_mod.bytes = NULL;
		io->nkvx_mod.bytes_len = 0;
		snprintf(io->nkvx_mod_key, sizeof(io->nkvx_mod_key), "%s",
			 binding->module_key != NULL ? binding->module_key : "");
		snprintf(io->nkvx_mod_ns, sizeof(io->nkvx_mod_ns), "%s",
			 binding->module_namespace != NULL ? binding->module_namespace : "");

		/*
		 * Reactor-side sha256 module-cache probe: a HIT means the compiled
		 * module is already cached, so we SKIP the librados module fetch and go
		 * straight to the data-object phase. A MISS fetches the module object
		 * first (deny-by-default still holds: the fetched bytes are verified
		 * against this sha256 before they are ever compiled/run).
		 */
		if (kvdev_rados_nkvx_wasm_module_cached(io->nkvx_mod.sha256)) {
			SPDK_NOTICELOG("nkvx: module sha256 cache HIT -> no module fetch\n");
			return kvdev_rados_nkvx_start_data_phase(io);
		}
		return kvdev_rados_nkvx_start_module_fetch(io);
	}

	/* No verified binding (legacy built-in / in-tree test path): straight to the
	 * data-object phase with no module verification (mod arg stays NULL). */
	return kvdev_rados_nkvx_start_data_phase(io);
}

#ifdef SPDK_CONFIG_MERCURY
/*
 * Slice C6c (bead spdk-v3w): completion trampoline that wraps the tenant cb_fn
 * for a TRACKED two-tier Exec forward. The forward has terminally completed
 * (normal result, transport error, OR an ABORTED from a per-command/teardown
 * cancel): drop this command's cancel-token retention entry, then fire the real
 * tenant completion. Runs on the reactor thread from the front progress poller
 * (same thread that inserted the entry — no locking). After this point the token
 * is stale, so a racing ABORT for the same cb_arg finds nothing and no-ops.
 */
static void
kvdev_rados_nkvx_fwd_done(void *cb_arg, int status, uint32_t value_len)
{
	struct kvdev_rados_nkvx_pending *p = cb_arg;
	struct kvdev_rados_io_channel *ch = p->ch;
	spdk_kvdev_io_completion_cb tenant_cb_fn = p->tenant_cb_fn;
	void *tenant_cb_arg = p->tenant_cb_arg;

	TAILQ_REMOVE(&ch->nkvx_pending, p, link);
	free(p);

	tenant_cb_fn(tenant_cb_arg, status, value_len);
}

/*
 * Slice C6c: submit a two-tier Exec forward WITH per-command cancel retention.
 * Allocates a pending entry keyed by the tenant cb_arg, forwards via the bridge
 * (which hands back a cancel token), and links the entry so a live NVMe ABORT
 * (kvdev_rados_exec_abort) can find and cancel THIS one forward. On any path
 * where the forward did not become a trackable in-flight call (synchronous
 * failure, or a forward that terminally completed inline via cb_fn) the entry is
 * dropped and the tenant cb_fn is invoked directly by the bridge — no stale
 * token is ever retained. Returns 0 (the tenant io is always completed via a
 * callback, mirroring the untracked path).
 */
static int
kvdev_rados_nkvx_exec_forward(struct kvdev_rados_io_channel *ch,
			      const void *key, uint8_t key_len,
			      uint32_t op_id, bool read_only, uint8_t runtime,
			      const char *module_key, const char *module_ns,
			      const uint8_t *sha256, bool sha256_valid,
			      uint64_t caps,
			      const void *input, uint32_t input_len,
			      void *output_buf, uint32_t output_buf_len,
			      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_pending *p;
	uint64_t token = KVDEV_RADOS_NKVX_TOKEN_NONE;
	int frc;

	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	p->ch = ch;
	p->tenant_cb_fn = cb_fn;
	p->tenant_cb_arg = cb_arg;

	/*
	 * Insert the retention entry BEFORE forwarding: the forward's done trampoline
	 * (kvdev_rados_nkvx_fwd_done) can only fire from a later progress tick on THIS
	 * thread, never synchronously inside the forward, so the entry is always on the
	 * list before any completion can try to remove it. On a synchronous submission
	 * failure the bridge returns negative WITHOUT calling our trampoline, so we
	 * unwind the entry and complete the tenant here.
	 */
	TAILQ_INSERT_TAIL(&ch->nkvx_pending, p, link);

	frc = kvdev_rados_nkvx_front_forward(ch->front, key, key_len, op_id, read_only,
					     runtime, module_key, module_ns,
					     sha256, sha256_valid, caps,
					     input, input_len, output_buf, output_buf_len,
					     kvdev_rados_nkvx_fwd_done, p, &token);
	if (frc != 0) {
		/* Not submitted: no trampoline will fire. Unwind retention and complete. */
		TAILQ_REMOVE(&ch->nkvx_pending, p, link);
		free(p);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, frc), 0);
		return 0;
	}
	p->token = token;
	return 0;
}

/*
 * B-i V2 (bead spdk-avu): like kvdev_rados_nkvx_exec_forward(), but the result
 * sink is a dma-buf (exported GPU VRAM) given by (sink_fd, sink_offset, sink_len)
 * rather than a host VA. Identical per-command cancel retention; only the bridge
 * call differs (kvdev_rados_nkvx_front_forward_dmabuf -> nkvx_front_forward_dmabuf).
 * sink_va is the advertised VA / cache key (NULL for a pure-VRAM sink).
 */
static int
kvdev_rados_nkvx_exec_forward_dmabuf(struct kvdev_rados_io_channel *ch,
				     const void *key, uint8_t key_len,
				     uint32_t op_id, bool read_only, uint8_t runtime,
				     const char *module_key, const char *module_ns,
				     const uint8_t *sha256, bool sha256_valid,
				     uint64_t caps,
				     const void *input, uint32_t input_len,
				     void *sink_va, uint32_t sink_len,
				     int sink_fd, uint64_t sink_offset,
				     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_pending *p;
	uint64_t token = KVDEV_RADOS_NKVX_TOKEN_NONE;
	int frc;

	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	p->ch = ch;
	p->tenant_cb_fn = cb_fn;
	p->tenant_cb_arg = cb_arg;

	/* Insert retention BEFORE forwarding (same ordering invariant as the VA path). */
	TAILQ_INSERT_TAIL(&ch->nkvx_pending, p, link);

	frc = kvdev_rados_nkvx_front_forward_dmabuf(ch->front, key, key_len, op_id,
						    read_only, runtime, module_key, module_ns,
						    sha256, sha256_valid, caps,
						    input, input_len, sink_va, sink_len,
						    sink_fd, sink_offset,
						    kvdev_rados_nkvx_fwd_done, p, &token);
	if (frc != 0) {
		TAILQ_REMOVE(&ch->nkvx_pending, p, link);
		free(p);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_NKVX_EXEC, frc), 0);
		return 0;
	}
	p->token = token;
	return 0;
}

/*
 * Slice C6c per-command (tenant NVMe ABORT) abort entrypoint. Find the in-flight
 * two-tier Exec forward submitted with this opaque \p cb_arg and route it through
 * the UAF-safe per-command cancel handshake (the executor stops PUSHing into the
 * tenant DPTR before it is released). The tenant io then completes ABORTED from
 * the forward's done trampoline exactly once.
 *
 * Returns 0 if a matching in-flight forward was found and entered cancel,
 * -ENOENT if no in-flight forward matches (already completed, never a two-tier
 * Exec, or not an Exec at all). NB this aborts ONLY two-tier (remote-executor)
 * Execs; single-tier in-process Execs and non-Exec ops have no cancel handle and
 * report -ENOENT (the command is left to run to natural completion, as today).
 */
static int
kvdev_rados_exec_abort(struct spdk_io_channel *_ch, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados_nkvx_pending *p;

	if (ch->front == NULL) {
		return -ENOENT;	/* single-tier channel: no remote forward to cancel */
	}

	TAILQ_FOREACH(p, &ch->nkvx_pending, link) {
		if (p->tenant_cb_arg == cb_arg) {
			/* Route through the shared two-phase begin-cancel. Idempotent: a
			 * duplicate ABORT for the same command re-enters harmlessly. The
			 * retention entry stays until the forward's trampoline reaps it (the
			 * tenant cb fires ABORTED there), so a second ABORT still matches.
			 * Propagate the result: false means the front call already resolved
			 * (not yet reaped) and nothing was actually cancelled — report
			 * not-aborted rather than lying "aborted" to the host. */
			return kvdev_rados_nkvx_front_cancel(ch->front, p->token) ? 0 : -ENOENT;
		}
	}
	return -ENOENT;
}
#endif /* SPDK_CONFIG_MERCURY */

/*
 * KV Exec (ADR-0005, structured binding per ADR-0010/0012/0014): the NVMf layer
 * resolves the data-plane op_id against the per-namespace allowlist and passes
 * the matching entry's STRUCTURED binding through to us. We route on
 * binding->runtime, never on a parsed string:
 *   - SPDK_KV_EXEC_RUNTIME_WASM: the new in-process sandboxed executor
 *     (rados-nkvx, ADR-0009). module_key names the built-in/wasm module
 *     ("bytecount", "identity", or "wasm:<name>").
 *   - SPDK_KV_EXEC_RUNTIME_CLS: the legacy Ceph object-class path below;
 *     module_namespace is the cls name, module_key is the method.
 * A NULL binding, an unknown runtime, or empty locator fields are rejected
 * INVALID before any rados call. op_id itself is not interpreted here.
 *
 * The cls runs server-side on the OSD owning oid = hex(key) via a rados read_op
 * carrying rados_read_op_exec, dispatched async with rados_aio_read_op_operate
 * (the same harvest/poller pattern as Retrieve — we never touch SPDK state from
 * a librados thread). librados ALLOCATES the cls output buffer to its TRUE
 * length (io->exec_out / io->exec_out_len); there is no caller buffer for
 * librados to over-run. kvdev_rados_io_finish then reports the true length,
 * copies back at most output_buf_len bytes, flags BUFFER_TOO_SMALL only on a
 * genuine over-flow (true_len > host len), and frees the librados buffer with
 * rados_buffer_free — see kvdev_rados_io_finish for the length/status contract.
 */
static int
kvdev_rados_exec(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
		 uint32_t op_id, bool read_only,
		 const struct spdk_kv_exec_binding *binding,
		 const void *input, uint32_t input_len,
		 void *output_buf, uint32_t output_buf_len,
		 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct kvdev_rados *rdev = ch->rdev;
	struct kvdev_rados_io *io;
	char oid[KVDEV_RADOS_EXEC_OID_MAX];
	const char *cls, *method;
	int rc;

	(void)op_id;

	if (binding == NULL) {
		SPDK_ERRLOG("KV Exec: missing structured binding\n");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	/*
	 * rados-nkvx routing (ADR-0009/0010, TB3 spdk-fbm): a wasm-runtime binding goes
	 * to the new in-process executor, not the legacy cls path below.
	 *
	 * DENY-BY-DEFAULT (ADR-0010): the sha256 is the SOLE authorization + integrity
	 * anchor. A wasm binding WITHOUT a valid bound sha256 is REJECTED here — we do
	 * NOT fall back to any implicit/legacy/built-in module. (The op-ID -> binding
	 * lookup and the allowlist membership check already happened in the NVMf control
	 * plane; an op-ID absent from the namespace allowlist never reaches this path.
	 * This is the second, backend gate: even an allowlisted wasm op must carry the
	 * hash that authorizes the bytes it will run.) The module object is fetched from
	 * (module_namespace, module_key) and verified against this sha256 before it is
	 * ever compiled or run; see kvdev_rados_nkvx_exec / the executor's hash gate.
	 */
	if (binding->runtime == SPDK_KV_EXEC_RUNTIME_WASM) {
		const char *module = binding->module_key;

		if (module == NULL || module[0] == '\0') {
			SPDK_ERRLOG("nkvx: wasm binding has no module_key\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		if (!binding->sha256_valid) {
			SPDK_ERRLOG("nkvx: wasm binding has no bound sha256 — REJECTED "
				    "(deny-by-default; unverified modules are never run)\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		if (binding->module_namespace == NULL || binding->module_namespace[0] == '\0') {
			SPDK_ERRLOG("nkvx: wasm binding has no module_namespace (module locator) "
				    "— REJECTED\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		/* The nkvx wasm executor is read-only by contract (ADR-0014): it
		 * computes over the cold-fetched value and never writes it back, so
		 * it is permitted on a read-only namespace. (A future write-capable
		 * module class must consult read_only at its own mutation point.) */
#ifdef SPDK_CONFIG_MERCURY
		/* Two-tier (Slice C4): the front gates above are authoritative
		 * (design §2.2); only the terminal dispatch differs — forward the
		 * verified wasm binding (key-only, design §2) to the remote executor
		 * instead of running it in-process. The executor re-validates sha256. */
		if (ch->front != NULL) {
			/* Slice C6c (bead spdk-v3w): submit WITH per-command cancel
			 * retention so a live NVMe ABORT can cancel THIS forward via the
			 * executor-side handshake (kvdev_rados_exec_abort). The helper always
			 * completes the tenant io through a callback. */
			return kvdev_rados_nkvx_exec_forward(ch, key, key_len,
					op_id, read_only, (uint8_t)binding->runtime,
					binding->module_key, binding->module_namespace,
					binding->sha256, true, binding->caps,
					input, input_len, output_buf, output_buf_len,
					cb_fn, cb_arg);
		}
#endif
		return kvdev_rados_nkvx_exec(ch, key, key_len, module, binding,
					     input, input_len,
					     output_buf, output_buf_len, cb_fn, cb_arg);
	}

	/*
	 * rados-nkvx BUILT-IN routing (ADR-0009): a legacy "nkvx:<module>" allowlist
	 * string decodes (in nvmf_rpc_item_to_kv_exec_binding) to a cls binding with
	 * module_namespace == "nkvx" and module_key == <module>. This is the in-process
	 * executor's BUILT-IN path (bytecount/identity): the modules are compiled-in C,
	 * so there are NO untrusted bytes to fetch or verify and the deny-by-default
	 * sha256 gate that guards REAL wasm does not apply. Route it to the executor
	 * here so the "nkvx:" shorthand keeps reaching the off-reactor built-ins after
	 * the structured-binding migration (ADR-0014) reworked the wasm route to key on
	 * binding->runtime. A real wasm module name ("wasm:<name>") MUST instead use a
	 * structured wasm binding (it carries the hash that authorizes its bytes), so it
	 * is rejected on this legacy route with a clear error rather than run unverified.
	 */
	if (binding->runtime == SPDK_KV_EXEC_RUNTIME_CLS &&
	    binding->module_namespace != NULL &&
	    strcmp(binding->module_namespace, KVDEV_RADOS_NKVX_CLS_NAMESPACE) == 0) {
		const char *module = binding->module_key;

		if (module == NULL || module[0] == '\0') {
			SPDK_ERRLOG("nkvx: built-in binding has no module name\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		if (strncmp(module, KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX,
			    strlen(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX)) == 0) {
			SPDK_ERRLOG("nkvx: real-wasm module '%s' requires a structured wasm "
				    "binding (sha256 + module locator); the legacy 'nkvx:%s' "
				    "string cannot authorize it — REJECTED\n", module, module);
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		/* Built-in: no verified binding (mod arg stays NULL in the executor). The
		 * executor is read-only by contract, so it is permitted on a read-only ns. */
#ifdef SPDK_CONFIG_MERCURY
		/* Two-tier (Slice C4): forward the built-in as the wire form the
		 * executor expects — runtime=CLS, module_ns="nkvx", module_key=<name>,
		 * sha256_valid=false (mirrors what this front received; the executor
		 * routes it to its built-ins, design §2.2). */
		if (ch->front != NULL) {
			/* Slice C6c: tracked submit (per-command ABORT-cancellable). */
			return kvdev_rados_nkvx_exec_forward(ch, key, key_len,
					op_id, read_only, (uint8_t)SPDK_KV_EXEC_RUNTIME_CLS,
					module, KVDEV_RADOS_NKVX_CLS_NAMESPACE,
					NULL, false, 0,
					input, input_len, output_buf, output_buf_len,
					cb_fn, cb_arg);
		}
#endif
		return kvdev_rados_nkvx_exec(ch, key, key_len, module, NULL,
					     input, input_len,
					     output_buf, output_buf_len, cb_fn, cb_arg);
	}

	/* Legacy Ceph object-class path: (module_namespace, module_key) = (class, method). */
	if (binding->runtime != SPDK_KV_EXEC_RUNTIME_CLS) {
		SPDK_ERRLOG("KV Exec: unsupported binding runtime %d\n", binding->runtime);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}
	/* A Ceph object-class method can perform arbitrary server-side writes and
	 * we cannot prove non-mutation from the binding, so the cls path is treated
	 * as mutating: reject it on a read-only namespace. The invariant is enforced
	 * here, at the mutation point (ADR-0014), not assumed by the opcode gate. */
	if (read_only) {
		SPDK_DEBUGLOG(kvdev_rados, "KV Exec cls path rejected on read-only namespace\n");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_READ_ONLY, 0);
		return 0;
	}
	cls = binding->module_namespace;
	method = binding->module_key;
	if (cls == NULL || cls[0] == '\0' || method == NULL || method[0] == '\0') {
		SPDK_ERRLOG("KV Exec: cls binding has empty class or method\n");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}

	kvdev_rados_key_to_oid(key, key_len, oid);

	io = kvdev_rados_io_alloc(ch, KVDEV_RADOS_OP_EXEC, cb_fn, cb_arg);
	if (io == NULL) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}
	io->buf_len = output_buf_len;
	io->host_out = output_buf;

	/*
	 * Build a read_op carrying rados_read_op_exec. librados ALLOCATES the cls
	 * output buffer to its true length (io->exec_out, io->exec_out_len) and the
	 * cls method's own return code lands in io->exec_rval — there is no caller
	 * buffer for librados to over-run, so a >cap cls output cannot corrupt any
	 * of our memory. io_finish copies back min(true_len, output_buf_len),
	 * reports the true length, and frees io->exec_out with rados_buffer_free.
	 * Dispatched async via rados_aio_read_op_operate, exactly like Retrieve.
	 */
	io->read_op = rados_create_read_op();
	if (io->read_op == NULL) {
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_NOMEM, 0);
		return 0;
	}

	rados_read_op_exec(io->read_op, cls, method,
			   (const char *)input, input_len,
			   &io->exec_out, &io->exec_out_len, &io->exec_rval);

	rc = rados_aio_read_op_operate(io->read_op, rdev->io_ctx, io->comp, oid, 0);
	if (rc < 0) {
		rados_release_read_op(io->read_op);
		rados_aio_release(io->comp);
		free(io);
		cb_fn(cb_arg, kvdev_rados_xlate_status(KVDEV_RADOS_OP_EXEC, rc), 0);
		return 0;
	}

	TAILQ_INSERT_TAIL(&ch->inflight, io, link);
	return 0;
}

/*
 * B-i V2 (bead spdk-avu): KV Exec whose RESULT SINK is a dma-buf (exported GPU
 * VRAM) rather than a host VA. ONLY the two-tier (ch->front != NULL) wasm/built-in
 * path is supported: the dma-buf fd is forwarded to the remote executor, which
 * RDMA-WRITEs the result straight into VRAM (nkvx_front_forward_dmabuf). Any other
 * shape -- single-tier (in-process executor / legacy cls, which have no remote
 * RDMA sink), a non-wasm/non-built-in binding, or a build without Mercury --
 * returns -ENOTSUP so the NVMf layer fails the command rather than mis-targeting
 * the result. The auth/runtime gating mirrors kvdev_rados_exec() exactly.
 */
static int
kvdev_rados_exec_dmabuf(struct spdk_io_channel *_ch, const void *key, uint8_t key_len,
			uint32_t op_id, bool read_only,
			const struct spdk_kv_exec_binding *binding,
			const void *input, uint32_t input_len,
			int sink_fd, uint64_t sink_offset, uint32_t sink_len,
			uint64_t sink_va,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	/*
	 * B-i V3: sink_va is the guest IOVA the result_sink SGL segment advertised. It
	 * is load-bearing as the verbs/irdma dma-buf MR base (FI_MR_VIRT_ADDR) and is
	 * forwarded as the bulk's advertised VA; it is NEVER dereferenced here.
	 */
	void *sink_va_p = (void *)(uintptr_t)sink_va;

	(void)op_id;

	if (binding == NULL) {
		SPDK_ERRLOG("KV Exec (dma-buf): missing structured binding\n");
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
		return 0;
	}
	if (sink_fd < 0) {
		return -EINVAL;
	}

#ifdef SPDK_CONFIG_MERCURY
	/* Two-tier only: a dma-buf sink has no meaning for the in-process executor. */
	if (ch->front == NULL) {
		return -ENOTSUP;
	}

	/* Verified wasm binding (same deny-by-default gates as kvdev_rados_exec). */
	if (binding->runtime == SPDK_KV_EXEC_RUNTIME_WASM) {
		const char *module = binding->module_key;

		if (module == NULL || module[0] == '\0' || !binding->sha256_valid ||
		    binding->module_namespace == NULL || binding->module_namespace[0] == '\0') {
			SPDK_ERRLOG("nkvx(dma-buf): wasm binding missing module/sha256/locator "
				    "— REJECTED\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		return kvdev_rados_nkvx_exec_forward_dmabuf(ch, key, key_len, op_id,
				read_only, (uint8_t)binding->runtime,
				binding->module_key, binding->module_namespace,
				binding->sha256, true, binding->caps,
				input, input_len, sink_va_p, sink_len,
				sink_fd, sink_offset, cb_fn, cb_arg);
	}

	/* Built-in "nkvx:<module>" (cls namespace == "nkvx", no sha256). */
	if (binding->runtime == SPDK_KV_EXEC_RUNTIME_CLS &&
	    binding->module_namespace != NULL &&
	    strcmp(binding->module_namespace, KVDEV_RADOS_NKVX_CLS_NAMESPACE) == 0) {
		const char *module = binding->module_key;

		if (module == NULL || module[0] == '\0' ||
		    strncmp(module, KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX,
			    strlen(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX)) == 0) {
			SPDK_ERRLOG("nkvx(dma-buf): invalid/real-wasm built-in module — REJECTED\n");
			cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_INVALID, 0);
			return 0;
		}
		return kvdev_rados_nkvx_exec_forward_dmabuf(ch, key, key_len, op_id,
				read_only, (uint8_t)SPDK_KV_EXEC_RUNTIME_CLS,
				module, KVDEV_RADOS_NKVX_CLS_NAMESPACE,
				NULL, false, 0,
				input, input_len, sink_va_p, sink_len,
				sink_fd, sink_offset, cb_fn, cb_arg);
	}

	/* A legacy Ceph object-class method has no remote RDMA sink: unsupported. */
	return -ENOTSUP;
#else
	(void)ch; (void)key; (void)key_len; (void)read_only; (void)binding;
	(void)input; (void)input_len; (void)sink_offset; (void)sink_len; (void)cb_fn;
	(void)cb_arg; (void)sink_va_p;
	/* No Mercury: no two-tier remote executor to RDMA into the dma-buf. */
	return -ENOTSUP;
#endif
}

/*
 * List is deferred for the rados backend (ADR-0002): rados object enumeration is
 * an unordered cursor that cannot seek to a spec start-key, so spec-conformant
 * paginated List is out of scope for this slice. Report command-not-supported.
 */
static int
kvdev_rados_list(struct spdk_io_channel *_ch, const void *start_key, uint8_t start_key_len,
		 spdk_kvdev_list_cb iter_cb, void *iter_arg,
		 spdk_kvdev_list_done_cb done_cb, void *done_arg)
{
	done_cb(done_arg, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0);
	return 0;
}

/* ---- channel / lifecycle ------------------------------------------------- */

#ifdef SPDK_CONFIG_MERCURY
/* Per-channel Mercury progress poller (design §4.2): drive HG_Progress/HG_Trigger
 * non-blocking on the reactor thread so RPC completions fire here. */
static int
kvdev_rados_front_poll(void *arg)
{
	struct kvdev_rados_io_channel *ch = arg;
	int n = kvdev_rados_nkvx_front_progress(ch->front);

	return (n > 0) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
#endif

static int
kvdev_rados_create_channel_cb(void *io_device, void *ctx_buf)
{
	struct kvdev_rados_io_channel *ch = ctx_buf;

	ch->rdev = io_device;
	TAILQ_INIT(&ch->inflight);
#ifdef SPDK_CONFIG_MERCURY
	TAILQ_INIT(&ch->nkvx_pending);	/* Slice C6c per-command cancel retention */
#endif
	/* 0 period -> poll every reactor tick; aios complete out of band. */
	ch->poller = SPDK_POLLER_REGISTER(kvdev_rados_poll, ch, 0);
	if (ch->poller == NULL) {
		return -ENOMEM;
	}

#ifdef SPDK_CONFIG_MERCURY
	/* Two-tier (Slice C4): stand up this channel's front client + progress
	 * poller when a remote executor is configured. The executor (Slice C2) must
	 * be up and listening before the first channel is created (bootstrap, OQ-8). */
	if (ch->rdev->remote_executor != NULL) {
		int rc = kvdev_rados_nkvx_front_create(ch->rdev->remote_executor, &ch->front);
		if (rc != 0) {
			SPDK_ERRLOG("nkvx: front client to executor '%s' failed: %s\n",
				    ch->rdev->remote_executor, spdk_strerror(-rc));
			spdk_poller_unregister(&ch->poller);
			return rc;
		}
		ch->front_poller = SPDK_POLLER_REGISTER(kvdev_rados_front_poll, ch, 0);
		if (ch->front_poller == NULL) {
			kvdev_rados_nkvx_front_destroy(ch->front);
			ch->front = NULL;
			spdk_poller_unregister(&ch->poller);
			return -ENOMEM;
		}
	}
#endif
	return 0;
}

static void
kvdev_rados_destroy_channel_cb(void *io_device, void *ctx_buf)
{
	struct kvdev_rados_io_channel *ch = ctx_buf;

	/* Drain any still-inflight aios so we neither leak nor complete after
	 * the channel is gone. */
	while (!TAILQ_EMPTY(&ch->inflight)) {
		struct kvdev_rados_io *io = TAILQ_FIRST(&ch->inflight);

		rados_aio_wait_for_complete(io->comp);
		TAILQ_REMOVE(&ch->inflight, io, link);
		kvdev_rados_io_finish(io);
	}
	spdk_poller_unregister(&ch->poller);
#ifdef SPDK_CONFIG_MERCURY
	if (ch->front != NULL) {
		/*
		 * Slice C6a channel-destroy teardown of two-tier Exec forwards. These live
		 * on the front's own in-flight list (not ch->inflight); a forward in flight
		 * has the tenant DPTR registered as a bulk handle. We must reach
		 * outstanding==0 (every call ctx freed / removed) before destroying the
		 * front, both so the tenant io completion fires (never hangs) and so the
		 * front is not finalized over a live in-flight list / leaked ctxs.
		 *
		 * BEST-EFFORT, NOT fully UAF-safe (bead spdk-5ia): HG_Cancel is ORIGIN-side
		 * only — the executor has no do-not-PUSH awareness and may still PUSH into a
		 * result_sink after we cancel. That residual cross-process late PUSH is
		 * acceptable ONLY because this is channel destroy (front AND executor torn
		 * down, the DPTR not reused per-command). A safe per-command abort needs the
		 * executor-side cancel protocol (spdk-5ia).
		 *
		 * Teardown procedure:
		 *   1. Cancel ALL once (idempotent; origin-side withdraw of every forward).
		 *   2. BOUNDED drain: progress with a small timeout up to a wall-clock
		 *      budget (no busy-spin, no infinite loop on a wedged/dead executor).
		 *   3. If the drain did NOT reach 0 within the budget (or progress failed),
		 *      FORCE-complete every remaining call (tenant cb FAILED + detach) so
		 *      the io completes and outstanding hits 0 — no hang, no leaked ctxs.
		 */
		uint64_t hz = spdk_get_ticks_hz();
		/* ~250 ms wall-clock budget: generous vs. a healthy origin-side cancel
		 * (which is prompt on sm/tcp), tight enough not to stall destroy on a dead
		 * executor. hz==0 (no TSC calibration) degrades to the iteration cap. */
		uint64_t deadline = hz ? spdk_get_ticks() + hz / 4 : 0;
		unsigned iter_cap = 200;	/* ~400 ms hard cap (2 ms/tick) if clock unusable */

		kvdev_rados_nkvx_front_cancel_all(ch->front);
		while (kvdev_rados_nkvx_front_outstanding(ch->front) > 0) {
			/* Block briefly per tick (teardown path) so we are not pinning the
			 * core; cancellations resolve to terminal completions here. */
			if (kvdev_rados_nkvx_front_drain_progress(ch->front, 2) < 0) {
				break;	/* fatal progress error: force-complete below */
			}
			if (deadline) {
				if (spdk_get_ticks() >= deadline) {
					break;
				}
			} else if (--iter_cap == 0) {
				break;
			}
		}
		if (kvdev_rados_nkvx_front_outstanding(ch->front) > 0) {
			/* Wedged/dead executor: do NOT silently destroy with live calls.
			 * Force-complete the tenant side (FAILED) and detach so no io hangs
			 * and no ctx leaks; handle teardown is left to HG_Finalize below
			 * (memory-safe — see nkvx_front_fail_all_pending). */
			SPDK_WARNLOG("nkvx front: %u Exec forward(s) did not drain within the "
				     "channel-destroy budget; force-completing FAILED "
				     "(executor wedged?)\n",
				     kvdev_rados_nkvx_front_outstanding(ch->front));
			kvdev_rados_nkvx_front_fail_all_pending(ch->front,
								SPDK_KVDEV_IO_STATUS_FAILED);
		}
		spdk_poller_unregister(&ch->front_poller);
		kvdev_rados_nkvx_front_destroy(ch->front);
		ch->front = NULL;
		/* The cancel_all + fail_all_pending drain above fires every forward's
		 * tenant cb through kvdev_rados_nkvx_fwd_done, which reaps its retention
		 * entry; the per-command list must therefore be empty now. */
		assert(TAILQ_EMPTY(&ch->nkvx_pending));
	}
#endif
}

static struct spdk_io_channel *
kvdev_rados_get_io_channel(void *ctx)
{
	struct kvdev_rados *rdev = ctx;

	return spdk_get_io_channel(rdev);
}

static void
kvdev_rados_free(struct kvdev_rados *rdev)
{
	if (rdev == NULL) {
		return;
	}
	if (rdev->io_ctx) {
		rados_ioctx_destroy(rdev->io_ctx);
	}
	kvdev_rados_put_cluster(&rdev->cluster_p);
	free(rdev->cluster_name);
	free(rdev->pool_name);
	free(rdev->namespace_name);
#ifdef SPDK_CONFIG_MERCURY
	free(rdev->remote_executor);
#endif
	free(rdev->kvdev.name);
	free(rdev);
}

static void
kvdev_rados_unregister_io_device_done(void *io_device)
{
	kvdev_rados_free(io_device);
}

static int
kvdev_rados_destruct(void *ctx)
{
	struct kvdev_rados *rdev = ctx;

	TAILQ_REMOVE(&g_kvdevs, rdev, tailq);
	spdk_io_device_unregister(rdev, kvdev_rados_unregister_io_device_done);
	return 0;
}

static const struct spdk_kvdev_fn_table kvdev_rados_fn_table = {
	.destruct	= kvdev_rados_destruct,
	.get_io_channel	= kvdev_rados_get_io_channel,
	.store		= kvdev_rados_store,
	.retrieve	= kvdev_rados_retrieve,
	.del		= kvdev_rados_op_delete,
	.exist		= kvdev_rados_exist,
	.list		= kvdev_rados_list,
	.exec		= kvdev_rados_exec,
	/* B-i V2 (bead spdk-avu): VRAM-direct (dma-buf) result-sink Exec. Always
	 * present; without CONFIG_MERCURY (or single-tier) it returns -ENOTSUP. */
	.exec_dmabuf	= kvdev_rados_exec_dmabuf,
#ifdef SPDK_CONFIG_MERCURY
	/* Slice C6c: per-command (tenant NVMe ABORT) cancel of an in-flight two-tier
	 * Exec forward. Only present under CONFIG_MERCURY: a non-Mercury build has no
	 * remote forward to cancel, so the abort op stays NULL (-> -ENOTSUP -> "command
	 * not aborted") and single-tier Execs run to natural completion as before. */
	.abort		= kvdev_rados_exec_abort,
#endif
};

/*
 * Create the per-instance ioctx on the pool (== subsystem) and set the rados
 * namespace (== KV namespace) on it. Runs unaffinitized to mirror bdev_rbd's
 * ioctx creation. Pools are operator pre-provisioned (ADR-0004): ioctx_create
 * fails if the pool is absent — we surface that rather than auto-create.
 */
static void *
kvdev_rados_init_ioctx(void *arg)
{
	struct kvdev_rados *rdev = arg;
	int rc;

	rc = rados_ioctx_create(*rdev->cluster_p, rdev->pool_name, &rdev->io_ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create ioctx on pool '%s': %s\n",
			    rdev->pool_name, spdk_strerror(-rc));
		rdev->io_ctx = NULL;
		return NULL;
	}

	if (rdev->namespace_name) {
		rados_ioctx_set_namespace(rdev->io_ctx, rdev->namespace_name);
	}

	return arg;
}

int
kvdev_rados_create(const struct kvdev_rados_opts *opts, struct spdk_kvdev **_kvdev)
{
	struct kvdev_rados *rdev;
	uint32_t max_value_len;
	int rc;

	if (opts == NULL || opts->name == NULL || opts->cluster_name == NULL ||
	    opts->pool_name == NULL) {
		return -EINVAL;
	}

	max_value_len = opts->max_value_len ? opts->max_value_len : KVDEV_RADOS_MAX_VALUE_LEN;

	rdev = calloc(1, sizeof(*rdev));
	if (rdev == NULL) {
		return -ENOMEM;
	}

	rdev->kvdev.name = strdup(opts->name);
	rdev->cluster_name = strdup(opts->cluster_name);
	rdev->pool_name = strdup(opts->pool_name);
	if (rdev->kvdev.name == NULL || rdev->cluster_name == NULL ||
	    rdev->pool_name == NULL) {
		rc = -ENOMEM;
		goto err;
	}
	if (opts->namespace_name) {
		rdev->namespace_name = strdup(opts->namespace_name);
		if (rdev->namespace_name == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
#ifdef SPDK_CONFIG_MERCURY
	/* Two-tier (Slice C4): an optional remote executor address selects the
	 * Mercury-forwarded nkvx Exec path; absent -> single-tier (in-process). */
	if (opts->remote_executor) {
		rdev->remote_executor = strdup(opts->remote_executor);
		if (rdev->remote_executor == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
#endif

	rc = kvdev_rados_get_cluster(rdev->cluster_name, &rdev->cluster_p);
	if (rc < 0) {
		SPDK_ERRLOG("Unknown rados cluster '%s' (register it first)\n",
			    rdev->cluster_name);
		goto err;
	}

	/* ioctx creation/connect can fail (missing pool, etc.); clean up fully. */
	if (spdk_call_unaffinitized(kvdev_rados_init_ioctx, rdev) == NULL) {
		rc = -ENODEV;
		goto err;
	}

	rdev->kvdev.ctxt = rdev;
	rdev->kvdev.fn_table = &kvdev_rados_fn_table;
	rdev->kvdev.module = &g_kvdev_rados_module;
	rdev->kvdev.caps.max_key_len = SPDK_KVDEV_KEY_MAX_LEN;
	rdev->kvdev.caps.max_value_len = max_value_len;
	rdev->kvdev.caps.max_num_keys = 0;	/* unbounded; rados scales out */
	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&rdev->kvdev.uuid, &opts->uuid);
	}

	spdk_io_device_register(rdev, kvdev_rados_create_channel_cb,
				kvdev_rados_destroy_channel_cb,
				sizeof(struct kvdev_rados_io_channel), opts->name);

	rc = spdk_kvdev_register(&rdev->kvdev);
	if (rc != 0) {
		spdk_io_device_unregister(rdev, NULL);
		goto err;
	}

	TAILQ_INSERT_TAIL(&g_kvdevs, rdev, tailq);

	*_kvdev = &rdev->kvdev;
	SPDK_NOTICELOG("Created rados kvdev '%s' (pool=%s, namespace=%s, max_value_len=%u)\n",
		       opts->name, opts->pool_name,
		       opts->namespace_name ? opts->namespace_name : "(default)",
		       max_value_len);
	return 0;

err:
	kvdev_rados_free(rdev);
	return rc;
}

int
kvdev_rados_delete(const char *name)
{
	struct spdk_kvdev *kvdev;

	kvdev = spdk_kvdev_get_by_name(name);
	if (kvdev == NULL) {
		return -ENODEV;
	}
	if (kvdev->module != &g_kvdev_rados_module) {
		SPDK_ERRLOG("kvdev '%s' is not a rados kvdev\n", name);
		return -EINVAL;
	}
	return spdk_kvdev_unregister(kvdev);
}

/*
 * module_init clears g_shutting_down so a finish-then-reinit of the kvdev
 * subsystem in the same process starts clean: module_fini latches it true, and
 * without this reset a subsequent re-init could shut a cluster down out from
 * under a live kvdev when a ref drops to 0.
 */
static int
kvdev_rados_module_init(void)
{
	g_shutting_down = false;
	/* Start the rados-nkvx off-reactor executor worker (ADR-0009). */
	return kvdev_rados_nkvx_start();
}

static void
kvdev_rados_module_fini(void)
{
	struct kvdev_rados *rdev, *tmp;
	struct kvdev_rados_cluster *c, *ctmp;

	g_shutting_down = true;

	/* Stop the rados-nkvx executor worker (ADR-0009): joins the worker thread
	 * so no off-reactor job outlives the module. */
	kvdev_rados_nkvx_stop();

	/* Deleting a kvdev frees it asynchronously (io_device unregister), and the
	 * async free releases the cluster ref. With g_shutting_down set, the last
	 * release shuts the cluster down (see kvdev_rados_put_cluster). */
	TAILQ_FOREACH_SAFE(rdev, &g_kvdevs, tailq, tmp) {
		kvdev_rados_delete(rdev->kvdev.name);
	}

	/* Sweep any clusters never referenced by a kvdev (ref == 0). Referenced
	 * ones are shut down by the async put_cluster above. */
	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH_SAFE(c, &g_clusters, link, ctmp) {
		if (c->ref != 0) {
			continue;
		}
		STAILQ_REMOVE(&g_clusters, c, kvdev_rados_cluster, link);
		rados_shutdown(c->cluster);
		kvdev_rados_cluster_free(c);
	}
	pthread_mutex_unlock(&g_clusters_mutex);
}

void
kvdev_rados_write_config_json(struct spdk_json_write_ctx *w)
{
	struct kvdev_rados_cluster *c;
	struct kvdev_rados *rdev;

	/* Clusters first: a kvdev_rados_create references one by name. */
	pthread_mutex_lock(&g_clusters_mutex);
	STAILQ_FOREACH(c, &g_clusters, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "kvdev_rados_register_cluster");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", c->name);
		if (c->user_id) {
			spdk_json_write_named_string(w, "user_id", c->user_id);
		}
		if (c->config_file) {
			spdk_json_write_named_string(w, "config_file", c->config_file);
		}
		if (c->key_file) {
			spdk_json_write_named_string(w, "key_file", c->key_file);
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	pthread_mutex_unlock(&g_clusters_mutex);

	TAILQ_FOREACH(rdev, &g_kvdevs, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "kvdev_rados_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", rdev->kvdev.name);
		spdk_json_write_named_string(w, "cluster_name", rdev->cluster_name);
		spdk_json_write_named_string(w, "pool_name", rdev->pool_name);
		if (rdev->namespace_name) {
			spdk_json_write_named_string(w, "namespace", rdev->namespace_name);
		}
		spdk_json_write_named_uint32(w, "max_value_len", rdev->kvdev.caps.max_value_len);
		if (!spdk_uuid_is_null(&rdev->kvdev.uuid)) {
			spdk_json_write_named_uuid(w, "uuid", &rdev->kvdev.uuid);
		}
#ifdef SPDK_CONFIG_MERCURY
		if (rdev->remote_executor) {
			spdk_json_write_named_string(w, "remote_executor", rdev->remote_executor);
		}
#endif
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(kvdev_rados)
