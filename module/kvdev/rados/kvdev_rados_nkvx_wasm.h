/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * rados-nkvx real-wasm runtime (TB-WIMP / ADR-0013): the dlopen-backed wasmtime
 * Exec path. This is the THINNEST viable real-wasm slice:
 *
 *   - wasmtime is loaded via dlopen("libwasmtime.so") + dlsym (NOT statically
 *     linked); an SPDK build carries no wasmtime link dependency. The vendored
 *     wasmtime C-API headers supply only the signatures. The symbol table is
 *     loaded lazily on first use and cached.
 *   - Memory strategy for this slice: on-demand instance + a PLAIN COPY of the
 *     cold-filled object bytes into the module's exported wasm linear memory.
 *     No custom host memory creator (TB4), no pooling allocator, no caps
 *     (fuel/epoch/memory — that's TB2).
 *   - Graceful degradation: if SPDK was built --without-wasm, or libwasmtime.so
 *     is absent, the call returns a DISTINCT "runtime unavailable" status; it
 *     never crashes. The C built-ins remain fully functional without wasmtime.
 *
 * The module ABI (matching test/nvmf/kv/wasm/bytecount.c):
 *   - The module exports a linear memory named "memory" and a function named
 *     <module>, e.g. "bytecount", with signature (i32 obj_off, i32 obj_len) ->
 *     i32 result_len. The host copies the object bytes into linear memory at
 *     obj_off, calls the function, then reads result_len bytes back from a fixed
 *     result offset in linear memory.
 */

#ifndef SPDK_KVDEV_RADOS_NKVX_WASM_H
#define SPDK_KVDEV_RADOS_NKVX_WASM_H

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"		/* SPDK_KV_EXEC_SHA256_LEN */

/*
 * TB3 (spdk-fbm / ADR-0010): the verified, content-addressed module binding for a
 * single Exec. When a run is given a non-NULL \c struct kvdev_rados_nkvx_module
 * the executor SKIPS the legacy filesystem-load-by-name path and instead:
 *
 *   - serves the COMPILED module from the sha256-keyed module cache on a hit
 *     (no recompile), or
 *   - on a miss, VERIFIES the supplied \c bytes hash to \c sha256 (a hard gate,
 *     ADR-0010) BEFORE compiling, then caches the compiled artifact keyed by the
 *     content hash so the NEXT Exec of the same hash skips refetch+recompile.
 *
 * This is the deny-by-default authorization anchor: the only modules that ever
 * reach the compiler are ones whose fetched bytes matched the control-plane's
 * bound sha256. The module cache is DISTINCT from the oid-keyed object cache and
 * the (module,oid) warm-instance cache.
 *
 * \c bytes/\c bytes_len carry the librados-fetched .wasm on the fetch-miss path
 * (the caller owns them; they need only outlive the run). They may be NULL/0 when
 * the caller already knows the hash is cached (a module-cache hit) — the executor
 * then serves the cached compiled module and never needs the raw bytes.
 *
 * \c caps is the per-invocation capability word from the allowlist binding
 * (ADR-0014). 0 means "use the executor defaults" (env-overridable), preserving
 * the TB2/TB4 behaviour; a non-zero value selects a capability tier (see
 * kvdev_rados_nkvx_wasm.c, nkvx_wasm_caps_load).
 */
struct kvdev_rados_nkvx_module {
	uint8_t		sha256[SPDK_KV_EXEC_SHA256_LEN];
	const void	*bytes;		/* fetched .wasm (fetch-miss path); may be NULL */
	size_t		bytes_len;
	uint64_t	caps;		/* per-invocation caps word (0 => defaults) */
};

/*
 * Probe whether a COMPILED module for \c sha256 is already in the content-addressed
 * module cache. Reactor-callable (own lock); lets the datapath skip the librados
 * module fetch entirely on a hit. Returns false in the stub build.
 */
bool kvdev_rados_nkvx_wasm_module_cached(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN]);

/*
 * VERIFY \c bytes against \c sha256 (the hard ADR-0010 gate), and on success
 * compile + insert into the content-addressed module cache (idempotent). Returns
 * SPDK_KVDEV_IO_STATUS_SUCCESS when the module is cached (verified+compiled, or
 * already present), SPDK_KVDEV_IO_STATUS_INVALID on a hash MISMATCH (never
 * compiled), SPDK_KVDEV_IO_STATUS_FAILED on a compile failure, or
 * SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED when the runtime is unavailable. NEVER
 * compiles or caches bytes that fail the hash check.
 */
int kvdev_rados_nkvx_wasm_module_insert(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
					const void *bytes, size_t bytes_len);

/* Drop the entire compiled-module (sha256) cache (test teardown / shutdown). */
void kvdev_rados_nkvx_wasm_module_cache_reset(void);

/* Test-only: count of compiled modules currently held in the sha256 cache. */
uint64_t kvdev_rados_nkvx_wasm_module_cache_count(void);

/*
 * Run a precompiled wasm module <name> against the object bytes, OFF the SPDK
 * reactor (called from the executor worker thread).
 *
 * \param name        wasm module name (the text after "nkvx:wasm:"), e.g.
 *                    "bytecount". Selects <name>.wasm from the nkvx wasm dir.
 * \param object      cold-filled object bytes.
 * \param object_len  length of \c object.
 * \param out         host output buffer for the module result.
 * \param out_len     capacity of \c out.
 * \param result_len  set to the TRUE result length the module produced.
 *
 * Returns an SPDK_KVDEV_IO_STATUS_* code:
 *   SUCCESS              result copied (or BUFFER_TOO_SMALL if it did not fit,
 *                        with the true length still reported);
 *   NOT_SUPPORTED        wasm runtime unavailable (built --without-wasm, or
 *                        libwasmtime.so could not be loaded) — fail-soft;
 *   FAILED               the module file/instantiation/execution failed.
 */
int kvdev_rados_nkvx_wasm_run(const char *name,
			      const struct kvdev_rados_nkvx_module *mod,
			      const void *object, size_t object_len,
			      void *out, uint32_t out_len, uint32_t *result_len);

/*
 * TB4 (spdk-ii0 / ADR-0013): IDENTITY(oid)-addressed object cache with
 * invalidation-on-write + zero-copy DMA into wasm linear memory, with a
 * warm-instance cache keyed by (module, object).
 *
 * NOTE on naming: this is an IDENTITY cache keyed by the object id (oid =
 * hex(key)), NOT a content-addressed (content-hashed) cache. There is no content
 * verification; coherence with a mutated value is maintained by INVALIDATING the
 * entry on every value-mutating op (Store/Delete) via
 * kvdev_rados_nkvx_wasm_cache_invalidate (spdk-ii0 D2). Historical comments and
 * counters may still say "content" for the cache-hit counter; read it as
 * "identity-cache hit".
 *
 * The executor owns the object store; it never calls librados. Cold fill is
 * inverted into a CALLBACK the executor invokes ONLY on a cache miss:
 *
 *   - On a miss the executor allocates the cache slot, calls \c fill to populate
 *     it from RADOS (cold fill), and keeps the buffer content-addressed by
 *     \c obj_key. The fill callback is the ONLY time librados is touched.
 *   - On a HIT (same \c obj_key seen before) the cached buffer is reused and
 *     \c fill is NEVER called — no librados refetch. This is the load-bearing
 *     property TB4 proves (a fake fill that counts invocations asserts 0 on the
 *     2nd Exec).
 *   - The cached buffer backs the module's wasm linear memory ZERO-COPY via the
 *     wasmtime custom MemoryCreator (on-demand strategy, ADR-0013) — no gather /
 *     bounce copy-in. wasmtime_memory_data() aliases the cache buffer.
 *   - The EXPENSIVE artifacts (engine + compiled module) are cached keyed by
 *     (module, obj_key) and REUSED on a subsequent Exec of the same pair (warm
 *     hit). For STATE ISOLATION (spdk-yc1) each Exec instantiates a FRESH
 *     store+instance from those warm artifacts and tears it down afterwards, so
 *     wasm globals / declared data segments reset between Execs and module state
 *     never leaks from one Exec to the next; the costly compile stays warm.
 *
 * \c fill is called with the executor-allocated, page-rounded cache buffer and
 * its capacity; it must write up to \c cap bytes and set \c *out_len to the true
 * object length (<= cap). It returns 0 on success or negative errno on a fill
 * failure (the slot is then discarded; nothing is cached).
 */
typedef int (*kvdev_rados_nkvx_fill_fn)(void *buf, size_t cap, size_t *out_len,
					void *fill_arg);

/*
 * Run module \c name against the object identified by \c obj_key, cold-filling it
 * via \c fill on a cache miss and serving it locally (zero-copy) on a hit.
 *
 * \param name        wasm module name (after "nkvx:wasm:"), e.g. "checksum".
 * \param obj_key     stable content/identity key for the object (e.g. the oid).
 *                    Keys the content-addressed object cache AND, with \c name,
 *                    the warm-instance cache.
 * \param object_len  the object's true length (so the cache slot is sized before
 *                    fill; the fill callback confirms the actual length).
 * \param fill        cold-fill callback, invoked ONLY on a content-cache miss.
 * \param fill_arg    opaque context for \c fill.
 * \param out         host output buffer for the module result.
 * \param out_len     capacity of \c out.
 * \param result_len  set to the TRUE result length the module produced.
 *
 * Returns an SPDK_KVDEV_IO_STATUS_* code (same contract as the plain run above,
 * plus FAILED if \c fill fails on a miss).
 */
int kvdev_rados_nkvx_wasm_run_cached(const char *name,
				     const struct kvdev_rados_nkvx_module *mod,
				     const char *obj_key, size_t object_len,
				     kvdev_rados_nkvx_fill_fn fill, void *fill_arg,
				     void *out, uint32_t out_len, uint32_t *result_len);

/* Drop the entire object + warm-instance cache (test teardown / shutdown). */
void kvdev_rados_nkvx_wasm_cache_reset(void);

/*
 * Probe whether \c obj_key is already present (filled) in the identity-addressed
 * object cache. Stateless boolean probe; prefer kvdev_rados_nkvx_wasm_cache_pin
 * on the datapath where the bytes will be read later (it is race-safe against a
 * concurrent invalidation). Returns false in the stub build (no cache).
 */
bool kvdev_rados_nkvx_wasm_cache_has(const char *obj_key);

/*
 * Probe-and-PIN (spdk-ii0 D2): if \c obj_key is present and filled, take a
 * reference on its buffer and return an opaque handle (non-NULL); else return
 * NULL. The pin makes the probe -> later-read sequence race-safe: a Store/Delete
 * invalidation between the pin and the eventual run cannot free the buffer, and
 * the in-flight Exec serves exactly the pinned version while subsequent Execs
 * miss the invalidated entry and cold-fill fresh (no stale read). The caller MUST
 * release the handle with kvdev_rados_nkvx_wasm_cache_unpin. Returns NULL in the
 * stub build.
 */
void *kvdev_rados_nkvx_wasm_cache_pin(const char *obj_key);

/* Release a handle from kvdev_rados_nkvx_wasm_cache_pin (NULL-safe). */
void kvdev_rados_nkvx_wasm_cache_unpin(void *handle);

/*
 * Run module \c name against a PINNED object (the handle from cache_pin), serving
 * exactly that version with NO librados refetch and NO cold fill. Same status
 * contract as run_cached. The caller still owns the pin and must unpin after.
 */
int kvdev_rados_nkvx_wasm_run_pinned(const char *name,
				     const struct kvdev_rados_nkvx_module *mod, void *pin,
				     void *out, uint32_t out_len, uint32_t *result_len);

/*
 * Invalidate every cache entry (object + warm instances) for \c obj_key. MUST be
 * called from every value-mutating datapath op (Store, Delete, ...) so a later
 * Exec cannot serve stale cached bytes (spdk-ii0 D2). No-op in the stub build and
 * for an unknown key.
 */
void kvdev_rados_nkvx_wasm_cache_invalidate(const char *obj_key);

/*
 * Tear down process-wide wasm runtime resources owned by the executor (spdk-0k1):
 * stop and JOIN the background epoch ticker thread so it does not outlive the
 * executor (no 1-thread leak on an executor stop/restart). Idempotent; a no-op in
 * the stub build and when the ticker was never started. Called from
 * kvdev_rados_nkvx_stop. A later executor restart lazily re-creates a fresh ticker.
 */
void kvdev_rados_nkvx_wasm_runtime_teardown(void);

/*
 * Test-only introspection counters (TB4 acceptance proof). Defined unconditionally
 * so the unit test links them in both --with-wasm and --without-wasm builds.
 *   - cold_fills:        number of times a fill callback actually ran (cold fill).
 *   - content_hits:      number of content-cache hits (served locally, no fill).
 *   - warm_hits:         number of warm-instance reuses.
 *   - last_mem_base:     base pointer of the last run's wasm linear memory.
 *   - last_cache_base:   base pointer of the cache buffer backing it.
 *                        Equal iff the linear memory was backed ZERO-COPY.
 */
struct kvdev_rados_nkvx_wasm_stats {
	uint64_t	cold_fills;
	uint64_t	content_hits;
	uint64_t	warm_hits;
	const void	*last_mem_base;
	const void	*last_cache_base;
};

void kvdev_rados_nkvx_wasm_get_stats(struct kvdev_rados_nkvx_wasm_stats *out);

#endif /* SPDK_KVDEV_RADOS_NKVX_WASM_H */
