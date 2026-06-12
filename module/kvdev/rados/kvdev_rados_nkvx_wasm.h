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

#endif /* SPDK_KVDEV_RADOS_NKVX_WASM_H */
