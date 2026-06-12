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
 * Minimal dlsym table — ONLY the symbols a trivial off-reactor run needs
 * (ADR-0013: keep the symbol set minimal). Version-gated by symbol-presence
 * probing (no wasmtime version function exists); a missing symbol => the table
 * fails to load and we fall back to "runtime unavailable".
 */
struct nkvx_wasm_api {
	wasm_engine_t *(*engine_new)(void);
	void (*engine_delete)(wasm_engine_t *);

	wasmtime_store_t *(*store_new)(wasm_engine_t *, void *, void *);
	wasmtime_context_t *(*store_context)(wasmtime_store_t *);
	void (*store_delete)(wasmtime_store_t *);

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

	SYM(engine_new, "wasm_engine_new");
	SYM(engine_delete, "wasm_engine_delete");
	SYM(store_new, "wasmtime_store_new");
	SYM(store_context, "wasmtime_store_context");
	SYM(store_delete, "wasmtime_store_delete");
	SYM(module_new, "wasmtime_module_new");
	SYM(module_delete, "wasmtime_module_delete");
	SYM(instance_new, "wasmtime_instance_new");
	SYM(instance_export_get, "wasmtime_instance_export_get");
	SYM(memory_data, "wasmtime_memory_data");
	SYM(memory_data_size, "wasmtime_memory_data_size");
	SYM(func_call, "wasmtime_func_call");
	SYM(error_message, "wasmtime_error_message");
	SYM(error_delete, "wasmtime_error_delete");
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

	wasm_engine_t *engine = NULL;
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

	/*
	 * On-demand instance strategy (ADR-0013 for THIS slice): a fresh
	 * engine/store/module/instance per run. No warm reuse, no pooling, no caps.
	 */
	engine = api->engine_new();
	if (engine == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasm_engine_new failed\n");
		goto out;
	}
	store = api->store_new(engine, NULL, NULL);
	if (store == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasmtime_store_new failed\n");
		goto out;
	}
	ctx = api->store_context(store);

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
		SPDK_ERRLOG("nkvx/wasm: module '%s' trapped during execution\n", name);
		api->trap_delete(trap);
		trap = NULL;
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
	if (engine != NULL) {
		api->engine_delete(engine);
	}
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
