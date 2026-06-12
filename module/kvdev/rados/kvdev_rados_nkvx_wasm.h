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
			      const void *object, size_t object_len,
			      void *out, uint32_t out_len, uint32_t *result_len);

/*
 * TB4 (spdk-ii0 / ADR-0013): content-addressed object cache + zero-copy DMA into
 * wasm linear memory, with a warm-instance cache keyed by (module, object).
 *
 * The executor owns the object store; it never calls librados. Cold fill is
 * inverted into a CALLBACK the executor invokes ONLY on a content-cache miss:
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
 *   - The instantiated wasm instance (engine/store/module/instance) is cached
 *     keyed by (module, obj_key) and REUSED on a subsequent Exec of the same
 *     pair (warm instance).
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
				     const char *obj_key, size_t object_len,
				     kvdev_rados_nkvx_fill_fn fill, void *fill_arg,
				     void *out, uint32_t out_len, uint32_t *result_len);

/* Drop the entire content + warm-instance cache (test teardown / shutdown). */
void kvdev_rados_nkvx_wasm_cache_reset(void);

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
