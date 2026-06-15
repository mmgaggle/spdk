/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Inter-tier Exec RPC contract (Slice C3, ADR-0015 + design doc
 * docs/design/slice-c-exec-rpc-mercury.md §1).
 *
 * One Mercury RPC, `nkvx_exec`: the front (`rados-nkv`, an SPDK reactor that
 * terminates the tenant NVMe-KV Exec) sends a request to the executor
 * (`rados-nkvx`, a standalone non-SPDK Mercury service) and gets back a result.
 *
 * This header is SHARED between the two separately-compiled tiers. Wire encoding
 * is Mercury's portable `hg_proc` serialization (XDR-style, endianness-safe),
 * NOT a C-struct memcpy: the executor is non-SPDK and may differ in ABI. Every
 * field is encoded explicitly (opaque/string/optional-bulk), so the in-memory
 * struct layout is irrelevant to the wire — only the encode/decode order is.
 *
 * The ONLY SPDK dependency this contract pulls in is <spdk/kvdev.h>, for the
 * `enum spdk_kvdev_io_status` codes that the wire status mirrors (§3). That
 * keeps the standalone executor (Slice C2) able to include this header without
 * dragging in the SPDK reactor/event libraries.
 *
 * BUILD INVARIANT (design §3, NORMATIVE): the executor MUST be built against the
 * IDENTICAL <spdk/kvdev.h> as the front (same `enum spdk_kvdev_io_status`
 * definition, same numeric values). The wire status is carried as a fixed int32
 * whose value-stability is a BUILD/PACKAGING invariant of the two-tier
 * deployment, NOT an ABI guarantee of the wire format. A front and executor
 * built from divergent kvdev.h are an UNSUPPORTED configuration. The
 * SPDK_STATIC_ASSERTs in nkvx_exec_rpc.c pin every enum value so that an
 * enum-value bump breaks the BUILD (assert fires) instead of silently re-mapping
 * a tenant CQE.
 */

#ifndef NKVX_EXEC_RPC_H
#define NKVX_EXEC_RPC_H

#include <mercury.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>
#include <mercury_proc_bulk.h>

#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, SPDK_KVDEV_EXEC_KEY_MAX_LEN, SHA256 len */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inline-vs-bulk threshold (design §1.1, OQ-1: 4 KiB proposed). Inputs and
 * results at or below this travel inline in the Mercury SEND/response; larger
 * payloads use a front-origin bulk RMA handle (input PULL / result-sink WRITE).
 *
 * The threshold compares against `min(result_len, osize)` for results (only the
 * bytes that will actually be delivered) and against `input_len` for inputs.
 */
#define NKVX_INLINE_MAX (4u * 1024u)

/**
 * Exec RPC REQUEST envelope (design §1.1, `nkvx_exec_in_t`).
 *
 * Field roles mirror the front's existing `struct spdk_kv_exec_binding`
 * (kvdev.h) plus the per-op arguments the front passes into kvdev_rados_exec()
 * (op_id, read_only, key, input, output-buffer cap). All inline fields ride in
 * the Mercury SEND; the object bytes are NEVER carried here (the executor
 * cold-fills them, design §2).
 */
typedef struct {
	/** Tenant CDW13; opaque to the executor, for tracing/telemetry only. */
	uint32_t	op_id;

	/**
	 * Front-assigned, per-front-UNIQUE handshake token (Slice C6b). op_id is the
	 * tenant CDW13 and is NOT unique among a front's concurrent in-flight Execs, so
	 * it cannot key the executor's cancel registry (two same-op_id Execs would alias
	 * and a cancel could abort the wrong one / ack the other early — a UAF). This
	 * monotonic per-front id names exactly one in-flight Exec. 0 = unset/none.
	 */
	uint64_t	client_call_id;

	/**
	 * Read-only invariant (design OQ-5): the front passes the per-op
	 * read_only bit; the executor enforces it at its mutation point.
	 */
	uint8_t		read_only;

	/** Module runtime kind; mirrors enum spdk_kv_exec_runtime (WASM=1, CLS=2). */
	uint8_t		runtime;

	/**
	 * Per-invocation capability TIER selector, [0, SPDK_KV_EXEC_CAPS_TIER_MAX].
	 * Carried as the binding's full uint64 caps word.
	 */
	uint64_t	caps;

	/** Key length, 1..SPDK_KVDEV_EXEC_KEY_MAX_LEN (255). */
	uint8_t		key_len;
	/** Key bytes (the RADOS oid identity). Only key_len bytes are encoded. */
	uint8_t		key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];

	/** Artifact content hash; the SOLE auth/integrity anchor (ADR-0010). */
	uint8_t		sha256[SPDK_KV_EXEC_SHA256_LEN];
	/** True when sha256 carries a real hash (false on the legacy cls path). */
	uint8_t		sha256_valid;

	/**
	 * Cold-fetch locator key (module object name / cls method). NUL-terminated,
	 * may be NULL/empty. Owned by the caller on encode; malloc'd on decode and
	 * freed by nkvx_exec_in_free().
	 */
	char		*module_key;
	/**
	 * Cold-fetch locator namespace (pool or pool/namespace). NUL-terminated,
	 * may be NULL/empty. Owned/freed same as module_key.
	 */
	char		*module_ns;

	/**
	 * Host output-buffer cap (tenant CDW12, clamped) == the front's
	 * output_buf_len. Drives result truncation (design §1.2).
	 */
	uint32_t	osize;

	/**
	 * Length of the per-request input. When input_len <= NKVX_INLINE_MAX the
	 * bytes are carried inline in `input_inline`; otherwise they are delivered
	 * out of band via `input_bulk` (PULL, executor-side; transfer is Slice C7).
	 */
	uint32_t	input_len;
	/**
	 * Inline input bytes. Valid (input_len bytes) iff input_len <= NKVX_INLINE_MAX.
	 * Owned by the caller on encode; malloc'd on decode and freed by
	 * nkvx_exec_in_free().
	 */
	void		*input_inline;

	/**
	 * Optional large-input RMA handle (design §1.3). Present (non-HG_BULK_NULL)
	 * iff input_len > NKVX_INLINE_MAX. The executor issues HG_Bulk_transfer(PULL)
	 * to read it. The actual transfer is Slice C7; here it is contract-only.
	 */
	hg_bulk_t	input_bulk;

	/**
	 * Optional large-result sink handle (design §1.3, `result_sink`). When
	 * present (non-HG_BULK_NULL), the front has pre-registered its host output
	 * buffer (the tenant DPTR) as a WRITE-mode bulk handle and the executor
	 * PUSHes the result straight into it (then `result_inline` in the response
	 * is empty). When HG_BULK_NULL, the executor inlines the result. The actual
	 * transfer is Slice C7; here it is contract-only.
	 */
	hg_bulk_t	result_sink;
} nkvx_exec_in_t;

/**
 * Exec RPC RESPONSE envelope (design §1.2, `nkvx_exec_out_t`).
 */
typedef struct {
	/**
	 * Result status: an `enum spdk_kvdev_io_status` value carried on the wire
	 * as a FIXED int32 via hg_proc (endianness/width-safe). The front runs it
	 * through nkvx_status_from_wire() before feeding nvmf_kvdev_complete().
	 */
	int32_t		status;

	/**
	 * The TRUE result length (Retrieve-style truncation, ADR-0014/design §1.2):
	 * always the full length the module produced, even when truncated to osize.
	 * The front sets tenant CQE DW0 to this. When result_len > osize the front
	 * maps status BUFFER_TOO_SMALL exactly as the tenant edge does.
	 */
	uint32_t	result_len;

	/**
	 * Inline result bytes. Present iff min(result_len, osize) <= NKVX_INLINE_MAX
	 * AND the request carried no result_sink. The number of inline bytes encoded
	 * is `result_inline_len` == min(result_len, osize) in that case, else 0
	 * (large result was/will be delivered via the result_sink WRITE bulk).
	 * Owned by the caller on encode; malloc'd on decode, freed by
	 * nkvx_exec_out_free().
	 */
	uint32_t	result_inline_len;
	void		*result_inline;
} nkvx_exec_out_t;

/**
 * Exec CANCEL RPC (Slice C6b, bead spdk-5ia; design §C6b). A SECOND, additive
 * Mercury RPC the front forwards to the executor to make the cross-process abort
 * use-after-free-safe: the executor sets do-not-PUSH / HG_Bulk_cancels any
 * in-flight result PUSH and acks ONLY once its remote bulk view is quiescent; the
 * front defers releasing the result_sink MR / tenant DPTR until that ack. The new
 * RPC is registered by name ("nkvx_cancel"), so its name-hashed id does NOT
 * perturb the frozen nkvx_exec contract — this is a freeze-respecting EXTENSION,
 * not a change to nkvx_exec_in_t (HITL nod).
 *
 * Registry key on the executor is (origin_addr, op_id), NOT op_id alone: op_id is
 * the tenant CDW13 and is not unique across fronts; a single executor serves
 * several. The origin address rides implicitly via HG_Get_info(handle)->addr, so
 * ONLY op_id is on the wire — deliberately avoiding a new field on the frozen
 * request envelope.
 */
typedef struct {
	/** The front-unique client_call_id of the in-flight nkvx_exec to cancel
	 *  (matches nkvx_exec_in_t.client_call_id — NOT op_id, which is not unique). */
	uint64_t	call_id;
} nkvx_cancel_in_t;

/**
 * Cancel ack code (TELEMETRY only). All three values mean the same thing for
 * SAFETY — "the executor's remote view of result_sink is gone" — so the front
 * never branches on the value; the DELIVERED ack is itself the proof. The value
 * only lets the executor log / tests assert which cancel case (a/b/c) was taken.
 */
enum nkvx_cancel_ack {
	NKVX_CANCEL_ALREADY_DONE = 0,	/* (a) op not found: already finished / unknown / dup */
	NKVX_CANCEL_ABORTED      = 1,	/* (b) found, no PUSH in flight: do-not-PUSH honored */
	NKVX_CANCEL_PUSH_CANCELED = 2,	/* (c) found, PUSH in flight: HG_Bulk_cancel'd */
};

/** Cancel RPC RESPONSE: a fixed int32 ack (one of enum nkvx_cancel_ack), endian-safe. */
typedef struct {
	int32_t		ack;
} nkvx_cancel_out_t;

/*
 * hg_proc serializers (design §1.2 / C3). One routine per envelope; each
 * encodes/decodes every field explicitly. Usable as the proc callback in
 * HG_Register() and directly in a manual hg_proc_create_set() round-trip.
 *
 * Return HG_SUCCESS or a Mercury error. On HG_DECODE these allocate
 * module_key/module_ns/input_inline/result_inline; call the matching _free()
 * (which is also the HG_FREE path of the proc) to release them.
 */
hg_return_t hg_proc_nkvx_exec_in_t(hg_proc_t proc, void *data);
hg_return_t hg_proc_nkvx_exec_out_t(hg_proc_t proc, void *data);

/*
 * Cancel RPC serializers (Slice C6b). Each is a single fixed-width scalar
 * (uint32 op_id / int32 ack), so there is nothing to allocate on decode and
 * therefore no matching _free() helper.
 */
hg_return_t hg_proc_nkvx_cancel_in_t(hg_proc_t proc, void *data);
hg_return_t hg_proc_nkvx_cancel_out_t(hg_proc_t proc, void *data);

/** Release heap allocations a decoded request holds (strings + inline input). */
void nkvx_exec_in_free(nkvx_exec_in_t *in);
/** Release heap allocations a decoded response holds (inline result). */
void nkvx_exec_out_free(nkvx_exec_out_t *out);

/*
 * Wire-status mapping (design §3). The wire status is a fixed int32; these map
 * between the wire value and `enum spdk_kvdev_io_status`. The front does NOT
 * blindly cast the incoming int32 — it runs it through nkvx_status_from_wire(),
 * which returns FAILED for any value outside the known set (a transport-level
 * fault synthesized to a retryable device error, design §3).
 */

/** Map an enum spdk_kvdev_io_status to its fixed int32 wire value. */
int32_t nkvx_status_to_wire(enum spdk_kvdev_io_status status);

/**
 * Map a fixed int32 wire value back to an enum spdk_kvdev_io_status. An
 * unrecognized wire value (outside the 10 known codes) maps to
 * SPDK_KVDEV_IO_STATUS_FAILED (design §3: never cast an unknown value through).
 */
enum spdk_kvdev_io_status nkvx_status_from_wire(int32_t wire);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_EXEC_RPC_H */
