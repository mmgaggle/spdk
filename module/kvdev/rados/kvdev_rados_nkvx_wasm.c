/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/config.h"	/* SPDK_CONFIG_WASM (must precede the #if below) */
#include "spdk/log.h"
#include "spdk/kvdev.h"
#include "spdk/util.h"

#include "kvdev_rados_nkvx.h"		/* KVDEV_RADOS_NKVX_WASM_DIR_ENV */
#include "kvdev_rados_nkvx_wasm.h"

/*
 * dlopen-backed wasmtime runtime for the rados-nkvx Exec path. See the header
 * for the design contract (ADR-0013). This file is only meaningfully compiled
 * when SPDK_CONFIG_WASM is set; with --without-wasm it collapses to a stub that
 * always reports the wasm runtime unavailable (NOT_SUPPORTED), so a build never
 * requires anything wasm.
 */

#if defined(SPDK_CONFIG_WASM)

#include <dlfcn.h>

/* Vendored wasmtime C-API headers (signatures only; no link dependency). */
#include "wasmtime.h"

/*
 * Module ABI offsets — must match test/nvmf/kv/wasm/bytecount.c:
 *   - object bytes are copied into linear memory at WASM_OBJ_OFF,
 *   - the module writes its result at WASM_RES_OFF and returns the result length.
 * A single 64KiB page of linear memory is plenty for the tracer-bullet object.
 */
#define WASM_OBJ_OFF 256u
#define WASM_RES_OFF 8u

/*
 * Per-invocation resource caps (TB2: the Phase-1 safety exit). A runaway or
 * over-allocating module must be CONTAINED, never crash/hang the target.
 *
 *   - fuel_ceiling: units of wasmtime "fuel" the module may consume before it
 *     traps (compute-runaway kill). 0 => fuel disabled for this run.
 *   - epoch_deadline_ticks: how many epoch ticks beyond the current epoch the
 *     module may run before it is interrupted (wall-clock-runaway kill — catches
 *     what fuel under-counts). 0 => epoch deadline disabled for this run.
 *   - max_memory_bytes: linear-memory ceiling enforced by the store limiter, so
 *     a memory.grow past the cap fails (contained, not an OOM of the target).
 *     0 => memory unlimited for this run.
 *
 * SOURCE OF CAPS (this slice): provisional defaults below, each overridable per
 * invocation by an environment variable (read once on every run, so they ARE
 * configurable per invocation). Wiring caps to the per-op allowlist binding is
 * TB3 (the binding-encoding freeze is in progress) — NOT this slice. The plumbing
 * in nkvx_wasm_caps_load() is exactly where the TB3 allowlist source will replace
 * the env/default source.
 */
struct nkvx_wasm_caps {
	uint64_t	fuel_ceiling;
	uint64_t	epoch_deadline_ticks;
	uint64_t	max_memory_bytes;
};

/* Provisional defaults (TB2). Tuned so the checked-in test modules behave as the
 * acceptance matrix requires: bytecount completes well under these; the runaway
 * modules blow past them. */
#define NKVX_WASM_DEFAULT_FUEL		(100ull * 1000ull * 1000ull)	/* 100M fuel units */
#define NKVX_WASM_DEFAULT_EPOCH_TICKS	(10ull)				/* 10 ticks @ tick interval */
#define NKVX_WASM_DEFAULT_MAX_MEMORY	(8ull * 1024ull * 1024ull)	/* 8 MiB linear memory */

/* Env overrides (per-invocation; TB3 replaces these with the allowlist source). */
#define NKVX_WASM_ENV_FUEL		"SPDK_NKVX_WASM_FUEL"
#define NKVX_WASM_ENV_EPOCH_TICKS	"SPDK_NKVX_WASM_EPOCH_TICKS"
#define NKVX_WASM_ENV_MAX_MEMORY	"SPDK_NKVX_WASM_MAX_MEMORY"

/* Epoch ticker interval. The background timer thread bumps the engine epoch at
 * this cadence; epoch_deadline_ticks * this interval is the effective wall-clock
 * budget. 10ms * 10 ticks => ~100ms wall-clock ceiling by default. */
#define NKVX_WASM_EPOCH_TICK_NS		(10ull * 1000ull * 1000ull)	/* 10 ms */

/*
 * dlsym table — the symbols a capped off-reactor run needs (ADR-0013: keep the
 * set minimal, but TB2 adds the config/fuel/epoch/limiter symbols). Version-gated
 * by symbol-presence probing (no wasmtime version function exists); a missing
 * symbol => the table fails to load and we fall back to "runtime unavailable".
 */
struct nkvx_wasm_api {
	/* Config + engine: a config carries the fuel/epoch feature toggles, so the
	 * engine is created via wasm_engine_new_with_config (NOT wasm_engine_new). */
	wasm_config_t *(*config_new)(void);
	void (*config_consume_fuel_set)(wasm_config_t *, bool);
	void (*config_epoch_interruption_set)(wasm_config_t *, bool);
	wasm_engine_t *(*engine_new_with_config)(wasm_config_t *);
	void (*engine_delete)(wasm_engine_t *);
	/* Wall-clock interrupt source: the timer thread calls this. */
	void (*engine_increment_epoch)(wasm_engine_t *);

	wasmtime_store_t *(*store_new)(wasm_engine_t *, void *, void *);
	wasmtime_context_t *(*store_context)(wasmtime_store_t *);
	void (*store_delete)(wasmtime_store_t *);
	/* Linear-memory (and friends) ceiling. */
	void (*store_limiter)(wasmtime_store_t *, int64_t, int64_t, int64_t,
			      int64_t, int64_t);

	/* Per-invocation caps applied to the store's context. */
	wasmtime_error_t *(*context_set_fuel)(wasmtime_context_t *, uint64_t);
	void (*context_set_epoch_deadline)(wasmtime_context_t *, uint64_t);

	wasmtime_error_t *(*module_new)(wasm_engine_t *, const uint8_t *, size_t,
					wasmtime_module_t **);
	void (*module_delete)(wasmtime_module_t *);

	wasmtime_error_t *(*instance_new)(wasmtime_context_t *, const wasmtime_module_t *,
					  const wasmtime_extern_t *, size_t,
					  wasmtime_instance_t *, wasm_trap_t **);
	bool (*instance_export_get)(wasmtime_context_t *, const wasmtime_instance_t *,
				    const char *, size_t, wasmtime_extern_t *);

	uint8_t *(*memory_data)(const wasmtime_context_t *, const wasmtime_memory_t *);
	size_t (*memory_data_size)(const wasmtime_context_t *, const wasmtime_memory_t *);

	wasmtime_error_t *(*func_call)(wasmtime_context_t *, const wasmtime_func_t *,
				       const wasmtime_val_t *, size_t,
				       wasmtime_val_t *, size_t, wasm_trap_t **);

	void (*error_message)(const wasmtime_error_t *, wasm_byte_vec_t *);
	void (*error_delete)(wasmtime_error_t *);
	void (*trap_message)(const wasm_trap_t *, wasm_byte_vec_t *);
	void (*trap_delete)(wasm_trap_t *);
	void (*byte_vec_delete)(wasm_byte_vec_t *);
};

/*
 * Lazy, cached load of libwasmtime.so + the symbol table. Loaded once on first
 * use; protected by a once-init. g_api stays NULL if the library or any required
 * symbol is missing — that is the fail-soft signal.
 */
static struct nkvx_wasm_api g_api;
static const struct nkvx_wasm_api *g_api_ready;
static pthread_once_t g_api_once = PTHREAD_ONCE_INIT;
static void *g_lib_handle;

/* Candidate paths for libwasmtime.so, tried in order. The bare soname covers the
 * ldconfig/LD_LIBRARY_PATH case; the explicit paths cover the documented
 * install location and the in-tree vendored copy. */
static const char *g_lib_candidates[] = {
	"libwasmtime.so",
	"/usr/local/lib/libwasmtime.so",
	"/usr/lib/libwasmtime.so",
	"/usr/lib64/libwasmtime.so",
};

static bool
nkvx_wasm_resolve(void *h)
{
	bool ok = true;

#define SYM(field, name) do { \
		*(void **)(&g_api.field) = dlsym(h, name); \
		if (g_api.field == NULL) { \
			SPDK_ERRLOG("nkvx/wasm: missing symbol %s in libwasmtime.so\n", name); \
			ok = false; \
		} \
	} while (0)

	SYM(config_new, "wasm_config_new");
	SYM(config_consume_fuel_set, "wasmtime_config_consume_fuel_set");
	SYM(config_epoch_interruption_set, "wasmtime_config_epoch_interruption_set");
	SYM(engine_new_with_config, "wasm_engine_new_with_config");
	SYM(engine_delete, "wasm_engine_delete");
	SYM(engine_increment_epoch, "wasmtime_engine_increment_epoch");
	SYM(store_new, "wasmtime_store_new");
	SYM(store_context, "wasmtime_store_context");
	SYM(store_delete, "wasmtime_store_delete");
	SYM(store_limiter, "wasmtime_store_limiter");
	SYM(context_set_fuel, "wasmtime_context_set_fuel");
	SYM(context_set_epoch_deadline, "wasmtime_context_set_epoch_deadline");
	SYM(module_new, "wasmtime_module_new");
	SYM(module_delete, "wasmtime_module_delete");
	SYM(instance_new, "wasmtime_instance_new");
	SYM(instance_export_get, "wasmtime_instance_export_get");
	SYM(memory_data, "wasmtime_memory_data");
	SYM(memory_data_size, "wasmtime_memory_data_size");
	SYM(func_call, "wasmtime_func_call");
	SYM(error_message, "wasmtime_error_message");
	SYM(error_delete, "wasmtime_error_delete");
	SYM(trap_message, "wasm_trap_message");
	SYM(trap_delete, "wasm_trap_delete");
	SYM(byte_vec_delete, "wasm_byte_vec_delete");
#undef SYM

	return ok;
}

static void
nkvx_wasm_load_once(void)
{
	void *h = NULL;
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_lib_candidates); i++) {
		h = dlopen(g_lib_candidates[i], RTLD_NOW | RTLD_LOCAL);
		if (h != NULL) {
			SPDK_NOTICELOG("nkvx/wasm: loaded %s\n", g_lib_candidates[i]);
			break;
		}
	}
	if (h == NULL) {
		SPDK_WARNLOG("nkvx/wasm: libwasmtime.so not found (wasm Exec path "
			     "unavailable; built-ins still work): %s\n", dlerror());
		return;
	}

	if (!nkvx_wasm_resolve(h)) {
		SPDK_ERRLOG("nkvx/wasm: incompatible libwasmtime.so (missing symbols); "
			    "wasm Exec path unavailable\n");
		dlclose(h);
		return;
	}

	g_lib_handle = h;
	g_api_ready = &g_api;
}

static const struct nkvx_wasm_api *
nkvx_wasm_api(void)
{
	pthread_once(&g_api_once, nkvx_wasm_load_once);
	return g_api_ready;
}

/*
 * EPOCH TICKER THREAD (the wall-clock interrupt source).
 *
 * Fuel under-counts wall-clock (a tight loop of cheap ops, or a host trap loop,
 * can burn real time without burning much fuel). Epoch interruption catches that:
 * a background thread bumps the engine-local epoch at a fixed cadence, and any
 * run whose store has set an epoch deadline traps once enough ticks elapse.
 *
 * On-demand (ADR-0013) means at most one engine is live at a time on the single
 * executor worker, so the ticker tracks a single "current engine" pointer under a
 * mutex. nkvx_wasm_epoch_register()/_unregister() bracket each run; the thread is
 * started once (lazily) and runs for process lifetime (it is a cheap sleeper and
 * there is no teardown hook on this path). If a second engine ever overlapped,
 * the register call would simply replace the pointer — acceptable because each
 * run also has its own fuel ceiling as a second, independent guard.
 */
static struct {
	pthread_mutex_t		mutex;
	pthread_cond_t		cond;
	pthread_t		tid;
	bool			started;
	wasm_engine_t		*engine;	/* engine to tick, or NULL when idle */
	const struct nkvx_wasm_api *api;
} g_epoch = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

static void *
nkvx_wasm_epoch_thread(void *arg)
{
	(void)arg;

	for (;;) {
		struct timespec ts;

		/* Sleep one tick interval (NKVX_WASM_EPOCH_TICK_NS). nanosleep is
		 * fine here; this thread does nothing else. */
		ts.tv_sec = (time_t)(NKVX_WASM_EPOCH_TICK_NS / 1000000000ull);
		ts.tv_nsec = (long)(NKVX_WASM_EPOCH_TICK_NS % 1000000000ull);
		nanosleep(&ts, NULL);

		pthread_mutex_lock(&g_epoch.mutex);
		if (g_epoch.engine != NULL && g_epoch.api != NULL) {
			/* Bump the epoch of the in-flight run's engine. A run that has
			 * set an epoch deadline traps once its tick budget elapses. */
			g_epoch.api->engine_increment_epoch(g_epoch.engine);
		}
		pthread_mutex_unlock(&g_epoch.mutex);
	}
	return NULL;
}

/* Start the ticker thread once; arm it for this run's engine. */
static void
nkvx_wasm_epoch_register(const struct nkvx_wasm_api *api, wasm_engine_t *engine)
{
	pthread_mutex_lock(&g_epoch.mutex);
	if (!g_epoch.started) {
		if (pthread_create(&g_epoch.tid, NULL, nkvx_wasm_epoch_thread, NULL) == 0) {
			g_epoch.started = true;
		} else {
			SPDK_ERRLOG("nkvx/wasm: epoch ticker thread create failed; "
				    "wall-clock cap inactive (fuel cap still applies)\n");
		}
	}
	g_epoch.api = api;
	g_epoch.engine = engine;
	pthread_mutex_unlock(&g_epoch.mutex);
}

/* Disarm the ticker (the engine is about to be torn down). */
static void
nkvx_wasm_epoch_unregister(wasm_engine_t *engine)
{
	pthread_mutex_lock(&g_epoch.mutex);
	if (g_epoch.engine == engine) {
		g_epoch.engine = NULL;
	}
	pthread_mutex_unlock(&g_epoch.mutex);
}

/* Parse a uint64 env override; returns def when unset/empty/unparseable. */
static uint64_t
nkvx_wasm_env_u64(const char *name, uint64_t def)
{
	const char *v = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (v == NULL || v[0] == '\0') {
		return def;
	}
	errno = 0;
	parsed = strtoull(v, &end, 0);
	if (errno != 0 || end == v || (end != NULL && *end != '\0')) {
		SPDK_WARNLOG("nkvx/wasm: bad value '%s' for %s; using default %" PRIu64 "\n",
			     v, name, def);
		return def;
	}
	return (uint64_t)parsed;
}

/*
 * Load the per-invocation caps for THIS run. TB2 source: provisional compile-time
 * defaults, each overridable by an environment variable (read on every run, so
 * caps are genuinely per-invocation configurable). TB3 will replace the body here
 * with the per-op allowlist binding once the binding-encoding freeze lands.
 */
static void
nkvx_wasm_caps_load(struct nkvx_wasm_caps *caps)
{
	caps->fuel_ceiling = nkvx_wasm_env_u64(NKVX_WASM_ENV_FUEL,
					       NKVX_WASM_DEFAULT_FUEL);
	caps->epoch_deadline_ticks = nkvx_wasm_env_u64(NKVX_WASM_ENV_EPOCH_TICKS,
				     NKVX_WASM_DEFAULT_EPOCH_TICKS);
	caps->max_memory_bytes = nkvx_wasm_env_u64(NKVX_WASM_ENV_MAX_MEMORY,
				 NKVX_WASM_DEFAULT_MAX_MEMORY);
}

/*
 * Classify a wasmtime trap as a resource-cap kill (fuel/epoch) versus an ordinary
 * module trap. The wasmtime C API exposes the trap message; cap-induced traps
 * carry a recognisable message ("all fuel consumed", "epoch deadline" / "interrupt").
 * On match we surface ABORTED; otherwise FAILED. Either way the worker returns
 * cleanly — the target never crashes.
 */
static bool
nkvx_wasm_trap_is_cap(const struct nkvx_wasm_api *api, wasm_trap_t *trap)
{
	wasm_byte_vec_t msg;
	bool is_cap = false;

	api->trap_message(trap, &msg);
	if (msg.data != NULL && msg.size > 0) {
		/* Case-insensitive substring search for the known cap messages. */
		static const char *needles[] = {
			"fuel", "epoch", "interrupt",
		};
		size_t i;

		for (i = 0; i < SPDK_COUNTOF(needles); i++) {
			size_t nlen = strlen(needles[i]);

			if (msg.size >= nlen) {
				size_t j;

				for (j = 0; j + nlen <= msg.size; j++) {
					if (strncasecmp(msg.data + j, needles[i], nlen) == 0) {
						is_cap = true;
						break;
					}
				}
			}
			if (is_cap) {
				break;
			}
		}
		SPDK_NOTICELOG("nkvx/wasm: trap message: %.*s (cap=%s)\n",
			       (int)msg.size, msg.data, is_cap ? "YES" : "no");
	}
	api->byte_vec_delete(&msg);
	return is_cap;
}

/* Log and free a wasmtime error. */
static void
nkvx_wasm_log_error(const struct nkvx_wasm_api *api, const char *what, wasmtime_error_t *err)
{
	wasm_byte_vec_t msg;

	api->error_message(err, &msg);
	SPDK_ERRLOG("nkvx/wasm: %s: %.*s\n", what, (int)msg.size, msg.data);
	api->byte_vec_delete(&msg);
	api->error_delete(err);
}

/* Resolve <name>.wasm under the nkvx wasm module directory into pathbuf. */
static int
nkvx_wasm_module_path(const char *name, char *pathbuf, size_t pathbuf_len)
{
	const char *dir = getenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV);

	if (dir == NULL || dir[0] == '\0') {
		/* No directory configured: cannot locate the module. The e2e/unit
		 * tests set SPDK_NKVX_WASM_DIR explicitly. */
		SPDK_ERRLOG("nkvx/wasm: %s unset; cannot locate '%s.wasm'\n",
			    KVDEV_RADOS_NKVX_WASM_DIR_ENV, name);
		return -1;
	}
	if ((size_t)snprintf(pathbuf, pathbuf_len, "%s/%s.wasm", dir, name) >= pathbuf_len) {
		SPDK_ERRLOG("nkvx/wasm: module path too long for '%s'\n", name);
		return -1;
	}
	return 0;
}

static uint8_t *
nkvx_wasm_read_file(const char *path, size_t *out_len)
{
	FILE *f;
	long sz;
	uint8_t *buf;

	f = fopen(path, "rb");
	if (f == NULL) {
		SPDK_ERRLOG("nkvx/wasm: cannot open module '%s': %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		SPDK_ERRLOG("nkvx/wasm: cannot size module '%s'\n", path);
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		SPDK_ERRLOG("nkvx/wasm: short read of module '%s'\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_len = (size_t)sz;
	return buf;
}

int
kvdev_rados_nkvx_wasm_run(const char *name,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len, uint32_t *result_len)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	char path[1024];
	uint8_t *wasm = NULL;
	size_t wasm_len = 0;
	int status = SPDK_KVDEV_IO_STATUS_FAILED;

	wasm_config_t *config = NULL;
	wasm_engine_t *engine = NULL;
	bool epoch_armed = false;
	wasmtime_store_t *store = NULL;
	wasmtime_context_t *ctx = NULL;
	wasmtime_module_t *module = NULL;
	wasmtime_error_t *err = NULL;
	wasm_trap_t *trap = NULL;
	wasmtime_instance_t instance;
	wasmtime_extern_t mem_ext, fn_ext;
	uint8_t *mem_base;
	size_t mem_size;
	wasmtime_val_t args[2], results[1];
	struct nkvx_wasm_caps caps;

	*result_len = 0;

	/* Fail-soft when the runtime is unavailable. */
	if (api == NULL) {
		SPDK_WARNLOG("nkvx/wasm: runtime unavailable for module '%s'\n", name);
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}

	if (nkvx_wasm_module_path(name, path, sizeof(path)) != 0) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	wasm = nkvx_wasm_read_file(path, &wasm_len);
	if (wasm == NULL) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}

	/* Per-invocation caps (TB2). See nkvx_wasm_caps_load() for the source. */
	nkvx_wasm_caps_load(&caps);
	SPDK_NOTICELOG("nkvx/wasm: module '%s' caps fuel=%" PRIu64 " epoch_ticks=%" PRIu64
		       " max_mem=%" PRIu64 "B\n", name,
		       caps.fuel_ceiling, caps.epoch_deadline_ticks, caps.max_memory_bytes);

	/*
	 * On-demand instance strategy (ADR-0013): a fresh engine/store/module/
	 * instance per run, NO warm reuse/pooling. TB2 adds per-invocation caps:
	 * the engine carries fuel + epoch-interruption feature toggles (set on the
	 * config BEFORE the engine is created); the store carries the fuel ceiling,
	 * epoch deadline, and linear-memory limiter.
	 */
	config = api->config_new();
	if (config == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasm_config_new failed\n");
		goto out;
	}
	/* Enable the features the caps need. Fuel and epoch are independent guards
	 * (fuel = deterministic compute budget; epoch = wall-clock backstop). */
	api->config_consume_fuel_set(config, caps.fuel_ceiling > 0);
	api->config_epoch_interruption_set(config, caps.epoch_deadline_ticks > 0);

	/* engine_new_with_config CONSUMES the config. */
	engine = api->engine_new_with_config(config);
	config = NULL;
	if (engine == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasm_engine_new_with_config failed\n");
		goto out;
	}

	/* Arm the wall-clock ticker for this engine BEFORE the module runs, so an
	 * epoch deadline can actually fire. Disarmed before engine teardown. */
	if (caps.epoch_deadline_ticks > 0) {
		nkvx_wasm_epoch_register(api, engine);
		epoch_armed = true;
	}

	store = api->store_new(engine, NULL, NULL);
	if (store == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasmtime_store_new failed\n");
		goto out;
	}
	ctx = api->store_context(store);

	/* MEMORY CAP: bound linear memory so a memory.grow past the cap fails and
	 * the module is contained, never OOMing the target. Other limits left at
	 * wasmtime defaults (negative => keep default). */
	if (caps.max_memory_bytes > 0) {
		api->store_limiter(store, (int64_t)caps.max_memory_bytes,
				   -1, -1, -1, -1);
	}

	err = api->module_new(engine, wasm, wasm_len, &module);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "module_new", err);
		goto out;
	}

	/* No imports: imports=NULL, nimports=0 (no WASI). */
	err = api->instance_new(ctx, module, NULL, 0, &instance, &trap);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "instance_new", err);
		goto out;
	}
	if (trap != NULL) {
		SPDK_ERRLOG("nkvx/wasm: instantiation trapped\n");
		api->trap_delete(trap);
		trap = NULL;
		goto out;
	}

	/* The module must export a linear memory named "memory" and a function
	 * named <name>. */
	if (!api->instance_export_get(ctx, &instance, "memory", strlen("memory"), &mem_ext) ||
	    mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported 'memory'\n", name);
		goto out;
	}
	if (!api->instance_export_get(ctx, &instance, name, strlen(name), &fn_ext) ||
	    fn_ext.kind != WASMTIME_EXTERN_FUNC) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported function '%s'\n", name, name);
		goto out;
	}

	mem_base = api->memory_data(ctx, &mem_ext.of.memory);
	mem_size = api->memory_data_size(ctx, &mem_ext.of.memory);

	/* PLAIN COPY of the object bytes into wasm linear memory (this slice's
	 * memory strategy — not zero-copy, that is TB4). */
	if ((size_t)WASM_OBJ_OFF + object_len > mem_size) {
		SPDK_ERRLOG("nkvx/wasm: object (%zu B) does not fit linear memory (%zu B) "
			    "at off %u\n", object_len, mem_size, WASM_OBJ_OFF);
		goto out;
	}
	if (object_len > 0 && object != NULL) {
		memcpy(mem_base + WASM_OBJ_OFF, object, object_len);
	}

	/*
	 * Apply the per-invocation FUEL + EPOCH caps to the store context, right
	 * before the call (fuel must be (re)set per invocation; the store starts
	 * with 0 fuel => an immediate trap if we forgot). A fuel-runaway exhausts
	 * its budget and traps; an epoch-runaway (which fuel may under-count) trips
	 * the wall-clock deadline the ticker thread advances.
	 */
	if (caps.fuel_ceiling > 0) {
		err = api->context_set_fuel(ctx, caps.fuel_ceiling);
		if (err != NULL) {
			nkvx_wasm_log_error(api, "context_set_fuel", err);
			goto out;
		}
	}
	if (caps.epoch_deadline_ticks > 0) {
		api->context_set_epoch_deadline(ctx, caps.epoch_deadline_ticks);
	}

	/* Call <name>(obj_off, obj_len) -> result_len. */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)WASM_OBJ_OFF;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)object_len;
	err = api->func_call(ctx, &fn_ext.of.func, args, 2, results, 1, &trap);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "func_call", err);
		goto out;
	}
	if (trap != NULL) {
		/* A trap here is either a per-invocation cap kill (fuel/epoch) or an
		 * ordinary module fault. Classify it so a cap kill surfaces the
		 * distinct ABORTED ("resource exhausted") status; either way the
		 * worker returns cleanly and the SPDK-thread completion still fires. */
		bool cap = nkvx_wasm_trap_is_cap(api, trap);

		SPDK_ERRLOG("nkvx/wasm: module '%s' trapped during execution (%s)\n",
			    name, cap ? "RESOURCE CAP — aborted" : "fault");
		api->trap_delete(trap);
		trap = NULL;
		status = cap ? SPDK_KVDEV_IO_STATUS_ABORTED : SPDK_KVDEV_IO_STATUS_FAILED;
		goto out;
	}
	if (results[0].kind != WASMTIME_I32) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' returned unexpected kind %d\n",
			    name, results[0].kind);
		goto out;
	}

	{
		uint32_t produced = (uint32_t)results[0].of.i32;

		/* Bound the produced length to what the module could have written. */
		if ((size_t)WASM_RES_OFF + produced > mem_size) {
			SPDK_ERRLOG("nkvx/wasm: module '%s' result (%u B at off %u) exceeds "
				    "linear memory\n", name, produced, WASM_RES_OFF);
			goto out;
		}

		*result_len = produced;

		/* Read the result out of linear memory into the host buffer. */
		uint32_t copy = (uint32_t)spdk_min(produced, out_len);
		if (copy > 0 && out != NULL) {
			memcpy(out, mem_base + WASM_RES_OFF, copy);
		}
		status = (produced > out_len) ?
			 SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
			 SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

out:
	if (module != NULL) {
		api->module_delete(module);
	}
	if (store != NULL) {
		api->store_delete(store);
	}
	/* Disarm the ticker BEFORE deleting the engine it points at (avoids the
	 * ticker thread touching a freed engine). */
	if (epoch_armed) {
		nkvx_wasm_epoch_unregister(engine);
	}
	if (engine != NULL) {
		api->engine_delete(engine);
	}
	/* config is always NULL here: engine_new_with_config consumes it (even on
	 * failure) and we NULL it immediately, and no goto jumps in between. */
	(void)config;
	free(wasm);
	return status;
}

#else /* !SPDK_CONFIG_WASM */

int
kvdev_rados_nkvx_wasm_run(const char *name,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len, uint32_t *result_len)
{
	(void)object;
	(void)object_len;
	(void)out;
	(void)out_len;

	/* Built --without-wasm: the wasm Exec path is compiled out. Fail-soft with a
	 * distinct "runtime unavailable" status; never a crash. */
	*result_len = 0;
	SPDK_WARNLOG("nkvx/wasm: built without --with-wasm; module '%s' unavailable\n", name);
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

#endif /* SPDK_CONFIG_WASM */
